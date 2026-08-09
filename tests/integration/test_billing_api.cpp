/**
 * @file test_billing_api.cpp
 * @brief Integration tests for the user-facing billing API
 *        (src/api/BillingController.hpp): packages, wallet, topup, capture.
 *
 * PayPal is stubbed via Billing::install_for_testing() (the test seam added
 * in Task 3, src/billing/PayPalClient.hpp) — a FakePayPalClient subclass
 * overrides create_order/capture_order with canned data. No network call is
 * possible from this file.
 *
 * Security-focused coverage (see the Task 4 brief's explicit requirements):
 *   - CaptureIsOwnerScoped: user B must not be able to drive a capture of
 *     user A's order via POST .../capture.
 *   - TopupIgnoresClientSuppliedCredits: a client-supplied "credits" field
 *     on POST .../topup must have zero effect on the credited amount.
 *   - WalletShowsOwnBalanceAndHistoryOnly: GET .../wallet never accepts (or
 *     is influenced by) any user-id-shaped parameter.
 */

#include <memory>
#include <string>

#include <drogon/HttpRequest.h>
#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "api/BillingController.hpp"
#include "billing/PayPalClient.hpp"
#include "billing/Wallet.hpp"
#include "database/Database.hpp"
#include "repositories/BillingRepository.hpp"
#include "repositories/RoleRepository.hpp"
#include "repositories/UserRepository.hpp"
#include "security/Auth.hpp"
#include "test_helpers.hpp"

using namespace drogon;
using json = nlohmann::json;

namespace {

/// Test double for Billing::PayPalClient: canned create_order/capture_order,
/// zero network I/O. Installed via Billing::install_for_testing().
class FakePayPalClient : public Billing::PayPalClient {
public:
    FakePayPalClient() : Billing::PayPalClient(Billing::PayPalClientConfig{}) {}

    int create_order_calls = 0;
    int capture_order_calls = 0;

    std::string next_order_id = "ORDER-FAKE-1";
    std::string next_capture_id = "CAPTURE-FAKE-1";
    // What capture_order() reports back — the test sets these to match the
    // payment row's real amount/currency so Billing::credit_capture's
    // amount/currency guard passes.
    std::int64_t capture_amount_cents = 0;
    std::string capture_currency = "USD";

    std::int64_t last_create_amount_cents = 0;
    std::string last_create_currency;
    std::string last_captured_order_id;

    Billing::PayPalOrder create_order(std::int64_t amount_cents,
                                      const std::string& currency,
                                      const std::string& /*reference*/,
                                      const std::string& /*return_url*/,
                                      const std::string& /*cancel_url*/) override {
        ++create_order_calls;
        last_create_amount_cents = amount_cents;
        last_create_currency = currency;
        Billing::PayPalOrder out;
        out.order_id = next_order_id;
        out.approve_url = "https://paypal.example.com/checkoutnow?token=" + next_order_id;
        return out;
    }

    Billing::PayPalCapture capture_order(const std::string& order_id) override {
        ++capture_order_calls;
        last_captured_order_id = order_id;
        Billing::PayPalCapture out;
        out.capture_id = next_capture_id;
        out.amount_cents = capture_amount_cents;
        out.currency = capture_currency;
        return out;
    }
};

class BillingApiTest : public TestHelpers::CoreBackedTest {
protected:
    Api::BillingController controller;
    Repositories::RoleRepository roles;
    Repositories::UserRepository users;
    Repositories::PaymentRepository payments;
    Repositories::PackageRepository packages;
    FakePayPalClient* fake = nullptr;  // non-owning — owned by Billing::global_paypal_client

    std::string config_file_name() const override { return "billing_api_test_config.json"; }

    void config_overrides(json& cfg) override {
        cfg["database"]["migrations_enabled"] = true;
        cfg["database"]["migrations_dir"] = "migrations";
        cfg["billing"]["enabled"] = true;
        cfg["billing"]["provider"] = "paypal";
        cfg["billing"]["currency"] = "USD";
        cfg["billing"]["credits_per_unit"] = 100;
        cfg["billing"]["min_amount_cents"] = 100;
        cfg["billing"]["max_amount_cents"] = 100000;
        cfg["billing"]["paypal"] = json{{"environment", "sandbox"},
                                        {"client_id", ""},
                                        {"client_secret", ""},
                                        {"webhook_id", ""},
                                        {"return_url", "https://site.example/billing/return"},
                                        {"cancel_url", "https://site.example/billing/cancel"}};
    }

    void post_init() override {
        auto owned = std::make_unique<FakePayPalClient>();
        fake = owned.get();
        Billing::install_for_testing(std::move(owned));
    }

    void SetUp() override {
        TestHelpers::CoreBackedTest::SetUp();
        if (::testing::Test::IsSkipped())
            return;
        Database::get().execute_write([](auto& txn) {
            txn.exec(
                "TRUNCATE TABLE wallet_entries, wallet_balances, billing_refunds, payments, billing_packages CASCADE");
            txn.exec("TRUNCATE TABLE users CASCADE");
            txn.exec("DELETE FROM roles WHERE name NOT IN ('User', 'Administrator')");
            return 0;
        });
    }

    void TearDown() override {
        // Don't leak the fake client (or its state) into a later suite in the
        // same test binary — see PayPalClient.hpp's own test-discipline note.
        Billing::reset_for_testing();
        TestHelpers::CoreBackedTest::TearDown();
    }

    Security::Auth::AuthPrincipal seed_user(const std::string& email) {
        auto role = roles.find_by_name("User");
        EXPECT_TRUE(role.has_value());
        auto u = users.create(email, std::string("$argon2id$placeholder"), std::nullopt, std::nullopt, role->id, true);
        Security::Auth::AuthPrincipal p;
        p.subject = u.id;
        p.raw_claims = json{{"sub", u.id}, {"permissions", role ? role->permissions : 0u}};
        return p;
    }

    Domain::Package seed_package(const std::string& title, std::int64_t amount_cents, std::int64_t credits) {
        return packages.create(title, amount_cents, credits, /*active=*/true, /*sort=*/0);
    }

    json do_topup(const Security::Auth::AuthPrincipal& p, const json& body, int* status = nullptr) {
        HttpResponsePtr resp;
        controller.topup(TestHelpers::authed_json(p, body), [&](const HttpResponsePtr& r) { resp = r; });
        if (status)
            *status = resp->statusCode();
        return json::parse(std::string(resp->body()));
    }

    json do_capture(const Security::Auth::AuthPrincipal& p, const std::string& order_id, int* status = nullptr) {
        HttpResponsePtr resp;
        controller.capture(TestHelpers::authed_json(p, json{{"order_id", order_id}}), [&](const HttpResponsePtr& r) {
            resp = r;
        });
        if (status)
            *status = resp->statusCode();
        return json::parse(std::string(resp->body()));
    }

    json do_wallet(const Security::Auth::AuthPrincipal& p, int* status = nullptr) {
        HttpResponsePtr resp;
        controller.getWallet(TestHelpers::authed(p), [&](const HttpResponsePtr& r) { resp = r; });
        if (status)
            *status = resp->statusCode();
        return json::parse(std::string(resp->body()));
    }
};

// ── packages ─────────────────────────────────────────────────────────────

TEST_F(BillingApiTest, ListPackagesReturnsOnlyActiveOnes) {
    auto user = seed_user("shopper@example.com");
    seed_package("Starter", 500, 500);
    auto inactive = packages.create("Retired", 100, 100, /*active=*/false, 1);
    (void)inactive;

    HttpResponsePtr resp;
    controller.listPackages(TestHelpers::authed(user), [&](const HttpResponsePtr& r) { resp = r; });
    ASSERT_EQ(resp->statusCode(), k200OK);
    auto body = json::parse(std::string(resp->body()));
    ASSERT_EQ(body["data"].size(), 1u);
    EXPECT_EQ(body["data"][0]["title"], "Starter");
}

// ── topup ────────────────────────────────────────────────────────────────

TEST_F(BillingApiTest, TopupWithPackageFreezesRateAndCredits) {
    auto user = seed_user("buyer1@example.com");
    auto pkg = seed_package("Booster", /*amount_cents=*/400, /*credits=*/500);
    fake->next_order_id = "ORDER-PKG-1";

    int status = 0;
    auto body = do_topup(user, json{{"package_id", pkg.id}}, &status);
    ASSERT_EQ(status, k201Created);
    EXPECT_EQ(body["data"]["order_id"], "ORDER-PKG-1");
    EXPECT_FALSE(body["data"]["approve_url"].get<std::string>().empty());
    // The credit count is never present in the response at all.
    EXPECT_FALSE(body["data"].contains("credits"));
    EXPECT_FALSE(body["data"].contains("credits_expected"));

    auto payment = payments.find_by_order_id("ORDER-PKG-1");
    ASSERT_TRUE(payment.has_value());
    EXPECT_EQ(payment->credits_expected, 500);       // package.credits, verbatim
    EXPECT_EQ(payment->rate_snapshot, 100);           // billing.credits_per_unit at the time of purchase
    EXPECT_EQ(payment->amount_cents, 400);
    ASSERT_TRUE(payment->package_id.has_value());
    EXPECT_EQ(*payment->package_id, pkg.id);
    EXPECT_EQ(payment->user_id, user.subject);
}

TEST_F(BillingApiTest, TopupWithCustomAmountRespectsMinMax) {
    auto user = seed_user("buyer2@example.com");

    int below_status = 0;
    auto below = do_topup(user, json{{"amount_cents", 50}}, &below_status);  // min is 100
    EXPECT_EQ(below_status, k400BadRequest);
    EXPECT_EQ(below["error"], "amount_out_of_range");
    EXPECT_FALSE(below["message"].get<std::string>().empty());

    int above_status = 0;
    auto above = do_topup(user, json{{"amount_cents", 200000}}, &above_status);  // max is 100000
    EXPECT_EQ(above_status, k400BadRequest);
    EXPECT_EQ(above["error"], "amount_out_of_range");

    // Neither attempt created a payment row.
    EXPECT_EQ(fake->create_order_calls, 0);
}

TEST_F(BillingApiTest, TopupIgnoresClientSuppliedCredits) {
    auto user = seed_user("buyer3@example.com");
    fake->next_order_id = "ORDER-IGNORE-CREDITS";

    int status = 0;
    // 1000 cents * credits_per_unit(100) / 100 = 1000 credits — NOT 999999.
    auto body = do_topup(user, json{{"amount_cents", 1000}, {"credits", 999999}}, &status);
    ASSERT_EQ(status, k201Created);

    auto payment = payments.find_by_order_id("ORDER-IGNORE-CREDITS");
    ASSERT_TRUE(payment.has_value());
    EXPECT_EQ(payment->credits_expected, 1000);
    EXPECT_NE(payment->credits_expected, 999999);
}

TEST_F(BillingApiTest, TopupRejectsNeitherOrBothOfPackageAndAmount) {
    auto user = seed_user("buyer4@example.com");

    int neither_status = 0;
    do_topup(user, json::object(), &neither_status);
    EXPECT_EQ(neither_status, k400BadRequest);

    auto pkg = seed_package("Dual", 500, 500);
    int both_status = 0;
    do_topup(user, json{{"package_id", pkg.id}, {"amount_cents", 500}}, &both_status);
    EXPECT_EQ(both_status, k400BadRequest);

    EXPECT_EQ(fake->create_order_calls, 0);
}

// ── capture ──────────────────────────────────────────────────────────────

TEST_F(BillingApiTest, CaptureCreditsWalletOnce) {
    auto user = seed_user("capturer@example.com");
    fake->next_order_id = "ORDER-CAP-1";

    int topup_status = 0;
    do_topup(user, json{{"amount_cents", 1000}}, &topup_status);
    ASSERT_EQ(topup_status, k201Created);
    fake->capture_amount_cents = 1000;
    fake->capture_currency = "USD";

    int first_status = 0;
    auto first = do_capture(user, "ORDER-CAP-1", &first_status);
    ASSERT_EQ(first_status, k200OK);
    EXPECT_TRUE(first["data"]["credited"].get<bool>());
    EXPECT_EQ(first["data"]["balance"], 1000);
    EXPECT_EQ(fake->capture_order_calls, 1);

    int second_status = 0;
    auto second = do_capture(user, "ORDER-CAP-1", &second_status);
    ASSERT_EQ(second_status, k200OK);
    EXPECT_FALSE(second["data"]["credited"].get<bool>());
    EXPECT_EQ(second["data"]["balance"], 1000);
    // The controller short-circuits on an already-captured order — PayPal's
    // capture_order is never called a second time.
    EXPECT_EQ(fake->capture_order_calls, 1);

    EXPECT_EQ(Billing::balance_of(user.subject, /*from_primary=*/true), 1000);
}

TEST_F(BillingApiTest, CaptureIsOwnerScoped) {
    auto alice = seed_user("alice@example.com");
    auto bob = seed_user("bob@example.com");
    fake->next_order_id = "ORDER-OWNER-1";

    do_topup(alice, json{{"amount_cents", 1000}});
    fake->capture_amount_cents = 1000;
    fake->capture_currency = "USD";

    // Bob supplies Alice's order id in the request body — must be refused,
    // not captured on Bob's behalf and not crediting Alice either.
    int status = 0;
    auto resp = do_capture(bob, "ORDER-OWNER-1", &status);
    EXPECT_EQ(status, k404NotFound);
    (void)resp;
    EXPECT_EQ(fake->capture_order_calls, 0);  // PayPal was never even called

    EXPECT_EQ(Billing::balance_of(alice.subject), 0);
    EXPECT_EQ(Billing::balance_of(bob.subject), 0);

    // Alice herself can still capture it.
    int alice_status = 0;
    auto alice_resp = do_capture(alice, "ORDER-OWNER-1", &alice_status);
    EXPECT_EQ(alice_status, k200OK);
    EXPECT_TRUE(alice_resp["data"]["credited"].get<bool>());
    EXPECT_EQ(Billing::balance_of(alice.subject, /*from_primary=*/true), 1000);
}

TEST_F(BillingApiTest, CaptureUnknownOrderReturns404) {
    auto user = seed_user("noorder@example.com");
    int status = 0;
    do_capture(user, "NO-SUCH-ORDER", &status);
    EXPECT_EQ(status, k404NotFound);
    EXPECT_EQ(fake->capture_order_calls, 0);
}

// ── wallet ───────────────────────────────────────────────────────────────

TEST_F(BillingApiTest, WalletShowsOwnBalanceAndHistoryOnly) {
    auto alice = seed_user("alicewallet@example.com");
    auto bob = seed_user("bobwallet@example.com");
    fake->next_order_id = "ORDER-WALLET-A";

    do_topup(alice, json{{"amount_cents", 1000}});
    fake->capture_amount_cents = 1000;
    fake->capture_currency = "USD";
    do_capture(alice, "ORDER-WALLET-A");

    int alice_status = 0;
    auto alice_wallet = do_wallet(alice, &alice_status);
    EXPECT_EQ(alice_status, k200OK);
    EXPECT_EQ(alice_wallet["data"]["balance"], 1000);
    ASSERT_EQ(alice_wallet["data"]["history"].size(), 1u);
    EXPECT_EQ(alice_wallet["data"]["history"][0]["kind"], "topup");

    // Bob's wallet is untouched by Alice's activity — GET .../wallet takes no
    // user-id parameter at all, so there is no way for Bob to ask for
    // Alice's wallet even if he tried to smuggle one in as a query param.
    HttpResponsePtr resp;
    auto req = TestHelpers::authed(bob);
    req->setParameter("user_id", alice.subject);
    controller.getWallet(req, [&](const HttpResponsePtr& r) { resp = r; });
    ASSERT_EQ(resp->statusCode(), k200OK);
    auto bob_wallet = json::parse(std::string(resp->body()));
    EXPECT_EQ(bob_wallet["data"]["balance"], 0);
    EXPECT_EQ(bob_wallet["data"]["history"].size(), 0u);
}

// ── module gate ──────────────────────────────────────────────────────────

class BillingDisabledApiTest : public TestHelpers::CoreBackedTest {
protected:
    Api::BillingController controller;
    Repositories::RoleRepository roles;
    Repositories::UserRepository users;

    std::string config_file_name() const override { return "billing_disabled_api_test_config.json"; }

    void config_overrides(json& cfg) override {
        cfg["database"]["migrations_enabled"] = true;
        cfg["database"]["migrations_dir"] = "migrations";
        cfg["billing"]["enabled"] = false;
    }

    void SetUp() override {
        TestHelpers::CoreBackedTest::SetUp();
        if (::testing::Test::IsSkipped())
            return;
        Database::get().execute_write([](auto& txn) {
            txn.exec("TRUNCATE TABLE users CASCADE");
            txn.exec("DELETE FROM roles WHERE name NOT IN ('User', 'Administrator')");
            return 0;
        });
    }

    Security::Auth::AuthPrincipal seed_user(const std::string& email) {
        auto role = roles.find_by_name("User");
        EXPECT_TRUE(role.has_value());
        auto u = users.create(email, std::string("$argon2id$placeholder"), std::nullopt, std::nullopt, role->id, true);
        Security::Auth::AuthPrincipal p;
        p.subject = u.id;
        p.raw_claims = json{{"sub", u.id}, {"permissions", role ? role->permissions : 0u}};
        return p;
    }
};

TEST_F(BillingDisabledApiTest, AllRoutes404WhenBillingDisabled) {
    auto user = seed_user("gated@example.com");

    HttpResponsePtr resp;
    controller.listPackages(TestHelpers::authed(user), [&](const HttpResponsePtr& r) { resp = r; });
    EXPECT_EQ(resp->statusCode(), k404NotFound);

    controller.getWallet(TestHelpers::authed(user), [&](const HttpResponsePtr& r) { resp = r; });
    EXPECT_EQ(resp->statusCode(), k404NotFound);

    controller.topup(TestHelpers::authed_json(user, json{{"amount_cents", 500}}), [&](const HttpResponsePtr& r) {
        resp = r;
    });
    EXPECT_EQ(resp->statusCode(), k404NotFound);

    controller.capture(TestHelpers::authed_json(user, json{{"order_id", "X"}}), [&](const HttpResponsePtr& r) {
        resp = r;
    });
    EXPECT_EQ(resp->statusCode(), k404NotFound);
}

}  // namespace
