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

#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
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
    // PayPal's own capture status — "COMPLETED" by default. Set to
    // "PENDING"/"DECLINED" (or anything else) to exercise
    // BillingController::capture's "don't credit unless COMPLETED" branch.
    std::string capture_status = "COMPLETED";
    // If set, capture_order() throws std::runtime_error(*capture_throw_message)
    // instead of returning — simulates a PayPal-side failure (transient, or a
    // structured error like ORDER_NOT_APPROVED).
    std::optional<std::string> capture_throw_message;

    std::int64_t last_create_amount_cents = 0;
    std::string last_create_currency;
    std::string last_captured_order_id;

    // ── webhook signature verification (Task 5) ─────────────────────────
    // true by default so a webhook test only has to override what it means
    // to exercise. Task 3's report: verify_webhook_signature THROWS for
    // "PayPal's own verify API was unreachable" and RETURNS false for
    // "malformed input / PayPal said no" — verify_throw_message simulates
    // the former, verify_signature_result=false the latter.
    bool verify_signature_result = true;
    std::optional<std::string> verify_throw_message;
    int verify_webhook_signature_calls = 0;
    std::map<std::string, std::string> last_verify_headers;
    std::string last_verify_raw_body;

    bool verify_webhook_signature(const std::map<std::string, std::string>& headers,
                                  const std::string& raw_body) override {
        ++verify_webhook_signature_calls;
        last_verify_headers = headers;
        last_verify_raw_body = raw_body;
        if (verify_throw_message)
            throw std::runtime_error(*verify_throw_message);
        return verify_signature_result;
    }

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
        if (capture_throw_message)
            throw std::runtime_error(*capture_throw_message);
        Billing::PayPalCapture out;
        out.capture_id = next_capture_id;
        out.amount_cents = capture_amount_cents;
        out.currency = capture_currency;
        out.status = capture_status;
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
        controller.capture(TestHelpers::authed_json(p, json{{"order_id", order_id}}),
                           [&](const HttpResponsePtr& r) { resp = r; });
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

    // ── webhook (Task 5) ─────────────────────────────────────────────────
    // POST /api/v1/billing/paypal/webhook is public/unauthenticated — no
    // principal is stamped on the request, mirroring the real PayPal caller.
    // include_headers=false drops the paypal-* headers entirely (a distinct
    // way verify_webhook_signature can legitimately return false, per
    // Task 3's report — exercised implicitly by the fake here since the fake
    // itself decides verified/unverified, but kept for shape-completeness).
    json do_webhook(const json& event, int* status = nullptr, bool include_headers = true) {
        auto req = TestHelpers::make_request(drogon::Post, event);
        if (include_headers) {
            req->addHeader("Paypal-Auth-Algo", "SHA256withRSA");
            req->addHeader("Paypal-Cert-Url", "https://api.sandbox.paypal.com/cert");
            req->addHeader("Paypal-Transmission-Id", "txn-" + event.value("id", std::string("none")));
            req->addHeader("Paypal-Transmission-Sig", "sig");
            req->addHeader("Paypal-Transmission-Time", "2026-01-01T00:00:00Z");
        }
        HttpResponsePtr resp;
        controller.paypalWebhook(req, [&](const HttpResponsePtr& r) { resp = r; });
        if (status)
            *status = resp->statusCode();
        return json::parse(std::string(resp->body()));
    }

    // Real PAYMENT.CAPTURE.COMPLETED shape: the order id lives at
    // resource.supplementary_data.related_ids.order_id (BillingController::
    // handleCaptureCompleted's own extraction path).
    static json capture_completed_event(const std::string& event_id,
                                        const std::string& order_id,
                                        const std::string& capture_id,
                                        const std::string& amount_value,
                                        const std::string& currency = "USD") {
        return json{{"id", event_id},
                    {"event_type", "PAYMENT.CAPTURE.COMPLETED"},
                    {"resource",
                     {{"id", capture_id},
                      {"status", "COMPLETED"},
                      {"amount", {{"value", amount_value}, {"currency_code", currency}}},
                      {"supplementary_data", {{"related_ids", {{"order_id", order_id}}}}}}}};
    }

    // Real PAYMENT.CAPTURE.REFUNDED shape: the refund resource carries no
    // direct capture_id field — it's recovered from the "up" link, exactly
    // as BillingController::extract_capture_id_from_links does.
    static json capture_refunded_event(const std::string& event_id,
                                       const std::string& refund_id,
                                       const std::string& capture_id,
                                       const std::string& amount_value,
                                       const std::string& currency = "USD") {
        return json{
            {"id", event_id},
            {"event_type", "PAYMENT.CAPTURE.REFUNDED"},
            {"resource",
             {{"id", refund_id},
              {"status", "COMPLETED"},
              {"amount", {{"value", amount_value}, {"currency_code", currency}}},
              {"links",
               json::array({{{"rel", "up"},
                             {"href", "https://api.sandbox.paypal.com/v2/payments/captures/" + capture_id}}})}}}};
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

TEST_F(BillingApiTest, ListPackagesIncludesRateAndBounds) {
    // Task 8's custom-amount input validates against these client-side; the
    // brief requires them alongside the package list, per config_overrides.
    auto user = seed_user("rate-checker@example.com");

    HttpResponsePtr resp;
    controller.listPackages(TestHelpers::authed(user), [&](const HttpResponsePtr& r) { resp = r; });
    ASSERT_EQ(resp->statusCode(), k200OK);
    auto body = json::parse(std::string(resp->body()));
    EXPECT_EQ(body["credits_per_unit"], 100);
    EXPECT_EQ(body["min_amount_cents"], 100);
    EXPECT_EQ(body["max_amount_cents"], 100000);
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
    EXPECT_EQ(payment->credits_expected, 500);  // package.credits, verbatim
    EXPECT_EQ(payment->rate_snapshot, 100);     // billing.credits_per_unit at the time of purchase
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

TEST_F(BillingApiTest, TopupWithCustomAmountAcceptsExactBoundaries) {
    auto user = seed_user("boundary-buyer@example.com");

    fake->next_order_id = "ORDER-MIN";
    int min_status = 0;
    do_topup(user, json{{"amount_cents", 100}}, &min_status);  // exactly min
    EXPECT_EQ(min_status, k201Created);

    fake->next_order_id = "ORDER-MAX";
    int max_status = 0;
    do_topup(user, json{{"amount_cents", 100000}}, &max_status);  // exactly max
    EXPECT_EQ(max_status, k201Created);

    EXPECT_EQ(fake->create_order_calls, 2);
}

TEST_F(BillingApiTest, TopupEnforcesMinMaxOnPackagePriceToo) {
    auto user = seed_user("cheap-package-buyer@example.com");
    // Priced below billing.min_amount_cents (100) — an admin data-entry
    // mistake must not be silently sellable just because it came from the
    // package catalogue instead of a client-supplied amount_cents.
    auto pkg = seed_package("Too Cheap", /*amount_cents=*/50, /*credits=*/50);

    int status = 0;
    auto body = do_topup(user, json{{"package_id", pkg.id}}, &status);
    EXPECT_EQ(status, k400BadRequest);
    EXPECT_EQ(body["error"], "package_price_out_of_range");
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

// IMPORTANT-1 fix-round-1: PayPal answers 2xx for a PENDING capture (eCheck,
// fraud review) too — a 2xx response is not proof the money settled. The
// wallet must stay untouched and the payment must stay uncaptured until a
// later resolution (Task 5's webhook) sees a final status.
TEST_F(BillingApiTest, CapturePendingLeavesPaymentUncapturedAndWalletUntouched) {
    auto user = seed_user("pending-buyer@example.com");
    fake->next_order_id = "ORDER-PENDING-1";
    do_topup(user, json{{"amount_cents", 1000}});
    fake->capture_amount_cents = 1000;
    fake->capture_currency = "USD";
    fake->capture_status = "PENDING";

    int status = 0;
    auto body = do_capture(user, "ORDER-PENDING-1", &status);
    EXPECT_EQ(status, k200OK);
    EXPECT_FALSE(body["data"]["credited"].get<bool>());
    EXPECT_EQ(body["data"]["balance"], 0);
    EXPECT_EQ(body["data"]["status"], "PENDING");
    EXPECT_TRUE(body["data"]["pending"].get<bool>());

    EXPECT_EQ(Billing::balance_of(user.subject), 0);
    EXPECT_EQ(Billing::history(user.subject, 10, 0).size(), 0u);  // no ledger row at all
    auto payment = payments.find_by_order_id("ORDER-PENDING-1");
    ASSERT_TRUE(payment.has_value());
    EXPECT_NE(payment->status, "captured");
    EXPECT_FALSE(payment->provider_capture_id.has_value());
}

// Same as above, DECLINED instead of PENDING — PayPal answers 2xx for this
// too, and it must be treated identically: no credit, payment left uncaptured.
TEST_F(BillingApiTest, CaptureDeclinedLeavesPaymentUncapturedAndWalletUntouched) {
    auto user = seed_user("declined-buyer@example.com");
    fake->next_order_id = "ORDER-DECLINED-1";
    do_topup(user, json{{"amount_cents", 1000}});
    fake->capture_amount_cents = 1000;
    fake->capture_currency = "USD";
    fake->capture_status = "DECLINED";

    int status = 0;
    auto body = do_capture(user, "ORDER-DECLINED-1", &status);
    EXPECT_EQ(status, k200OK);
    EXPECT_FALSE(body["data"]["credited"].get<bool>());
    EXPECT_EQ(body["data"]["status"], "DECLINED");
    EXPECT_TRUE(body["data"]["pending"].get<bool>());

    EXPECT_EQ(Billing::balance_of(user.subject), 0);
    auto payment = payments.find_by_order_id("ORDER-DECLINED-1");
    ASSERT_TRUE(payment.has_value());
    EXPECT_NE(payment->status, "captured");
}

TEST_F(BillingApiTest, CaptureOfFailedPaymentReturns409WithoutCallingPayPal) {
    auto user = seed_user("failed-payment-buyer@example.com");
    fake->next_order_id = "ORDER-FAILED-1";
    do_topup(user, json{{"amount_cents", 1000}});
    // Force the payment to 'failed' directly (amount mismatch), the same way
    // test_wallet.cpp does — bypasses the controller/fake entirely, so this
    // doesn't touch fake->capture_order_calls.
    Billing::credit_capture("ORDER-FAILED-1", "CAPTURE-MISMATCH", /*captured_amount_cents=*/999, "USD");
    auto pre = payments.find_by_order_id("ORDER-FAILED-1");
    ASSERT_TRUE(pre.has_value());
    ASSERT_EQ(pre->status, "failed");

    int status = 0;
    auto body = do_capture(user, "ORDER-FAILED-1", &status);
    EXPECT_EQ(status, k409Conflict);
    EXPECT_EQ(body["error"], "payment_not_capturable");
    EXPECT_EQ(fake->capture_order_calls, 0);  // never even asked PayPal
}

TEST_F(BillingApiTest, CaptureOfRefundedPaymentReturns409WithoutCallingPayPalAgain) {
    auto user = seed_user("refunded-payment-buyer@example.com");
    fake->next_order_id = "ORDER-REFUNDED-1";
    do_topup(user, json{{"amount_cents", 1000}});
    fake->capture_amount_cents = 1000;
    fake->capture_currency = "USD";
    int captured_status = 0;
    do_capture(user, "ORDER-REFUNDED-1", &captured_status);
    ASSERT_EQ(captured_status, k200OK);
    EXPECT_EQ(fake->capture_order_calls, 1);

    Billing::refund_capture("CAPTURE-FAKE-1", "REFUND-FULL", /*refunded_amount_cents=*/1000);
    auto refunded = payments.find_by_order_id("ORDER-REFUNDED-1");
    ASSERT_TRUE(refunded.has_value());
    ASSERT_EQ(refunded->status, "refunded");

    int status = 0;
    auto body = do_capture(user, "ORDER-REFUNDED-1", &status);
    EXPECT_EQ(status, k409Conflict);
    EXPECT_EQ(body["error"], "payment_not_capturable");
    // The earlier successful capture is the only real PayPal call made.
    EXPECT_EQ(fake->capture_order_calls, 1);
}

TEST_F(BillingApiTest, CaptureMapsOrderNotApprovedToConflict) {
    auto user = seed_user("not-approved-buyer@example.com");
    fake->next_order_id = "ORDER-NOT-APPROVED-1";
    do_topup(user, json{{"amount_cents", 1000}});
    // Shape of a real PayPal Orders API 422 for this issue, after
    // describe_error_body's details[].issue extraction.
    fake->capture_throw_message =
        "paypal: capture_order failed with HTTP 422: name=UNPROCESSABLE_ENTITY issue=ORDER_NOT_APPROVED";

    int status = 0;
    auto body = do_capture(user, "ORDER-NOT-APPROVED-1", &status);
    EXPECT_EQ(status, k409Conflict);
    EXPECT_EQ(body["error"], "order_not_approved");

    // No partial state: the payment is untouched, nothing was credited.
    EXPECT_EQ(Billing::balance_of(user.subject), 0);
    auto payment = payments.find_by_order_id("ORDER-NOT-APPROVED-1");
    ASSERT_TRUE(payment.has_value());
    EXPECT_NE(payment->status, "captured");
    EXPECT_NE(payment->status, "failed");
}

// IMPORTANT-3 fix-round-1: a PayPal failure that ISN'T a recognized 4xx-shaped
// issue must surface as a plain 500 with no partial/inconsistent DB state —
// no ledger row, no payment mutation, and the request is safely retryable.
TEST_F(BillingApiTest, CaptureSurfacesGenericPayPalFailureAsInternalErrorWithNoPartialState) {
    auto user = seed_user("outage-buyer@example.com");
    fake->next_order_id = "ORDER-OUTAGE-1";
    do_topup(user, json{{"amount_cents", 1000}});
    fake->capture_throw_message = "paypal: capture_order failed with HTTP 500: internal server error";

    int status = 0;
    auto body = do_capture(user, "ORDER-OUTAGE-1", &status);
    EXPECT_EQ(status, k500InternalServerError);
    EXPECT_EQ(body["error"], "internal_error");

    EXPECT_EQ(Billing::balance_of(user.subject), 0);
    EXPECT_EQ(Billing::history(user.subject, 10, 0).size(), 0u);
    auto payment = payments.find_by_order_id("ORDER-OUTAGE-1");
    ASSERT_TRUE(payment.has_value());
    EXPECT_EQ(payment->status, "created");  // untouched — not failed, not captured
    EXPECT_FALSE(payment->provider_capture_id.has_value());
}

// ── paypal webhook ───────────────────────────────────────────────────────

TEST_F(BillingApiTest, WebhookRejectsInvalidSignature) {
    auto user = seed_user("webhook-badsig@example.com");
    fake->next_order_id = "ORDER-BADSIG-1";
    do_topup(user, json{{"amount_cents", 1000}});
    fake->verify_signature_result = false;

    int status = 0;
    auto body =
        do_webhook(capture_completed_event("WH-BADSIG-1", "ORDER-BADSIG-1", "CAPTURE-BADSIG-1", "10.00"), &status);
    EXPECT_EQ(status, k401Unauthorized);
    EXPECT_EQ(body["error"], "invalid_signature");

    EXPECT_EQ(Billing::balance_of(user.subject), 0);
    EXPECT_EQ(Billing::history(user.subject, 10, 0).size(), 0u);
    auto payment = payments.find_by_order_id("ORDER-BADSIG-1");
    ASSERT_TRUE(payment.has_value());
    EXPECT_FALSE(payment->provider_capture_id.has_value());
}

// verify_webhook_signature THROWS (not returns false) when PayPal's own
// verify API is unreachable/non-2xx — Task 3's report is explicit this must
// map to 5xx (PayPal retries later), never conflated with an actual bad
// signature (401).
TEST_F(BillingApiTest, WebhookVerificationApiUnreachableReturns5xx) {
    auto user = seed_user("webhook-unreachable@example.com");
    fake->next_order_id = "ORDER-UNREACHABLE-1";
    do_topup(user, json{{"amount_cents", 1000}});
    fake->verify_throw_message = "paypal: verify-webhook-signature failed with HTTP 503: service unavailable";

    int status = 0;
    auto body = do_webhook(
        capture_completed_event("WH-UNREACHABLE-1", "ORDER-UNREACHABLE-1", "CAPTURE-UNREACHABLE-1", "10.00"), &status);
    EXPECT_GE(status, 500);
    EXPECT_LT(status, 600);
    EXPECT_EQ(body["error"], "internal_error");

    EXPECT_EQ(Billing::balance_of(user.subject), 0);
    auto payment = payments.find_by_order_id("ORDER-UNREACHABLE-1");
    ASSERT_TRUE(payment.has_value());
    EXPECT_FALSE(payment->provider_capture_id.has_value());
}

// The core "crediting when the user never returned" scenario: no call to
// POST .../capture ever happened for this order — the webhook alone credits.
TEST_F(BillingApiTest, WebhookCreditsWhenUserNeverReturned) {
    auto user = seed_user("webhook-neverreturned@example.com");
    fake->next_order_id = "ORDER-NEVERRETURNED-1";
    do_topup(user, json{{"amount_cents", 1000}});
    // fake->capture_order is never invoked in this test — the return-flow
    // capture endpoint is never called at all.

    int status = 0;
    auto body = do_webhook(
        capture_completed_event("WH-NEVERRETURNED-1", "ORDER-NEVERRETURNED-1", "CAPTURE-NEVERRETURNED-1", "10.00"),
        &status);
    EXPECT_EQ(status, k200OK);
    EXPECT_TRUE(body["data"]["handled"].get<bool>());

    EXPECT_EQ(Billing::balance_of(user.subject, /*from_primary=*/true), 1000);
    EXPECT_EQ(Billing::history(user.subject, 10, 0, /*from_primary=*/true).size(), 1u);
    auto payment = payments.find_by_order_id("ORDER-NEVERRETURNED-1", /*from_primary=*/true);
    ASSERT_TRUE(payment.has_value());
    EXPECT_EQ(payment->status, "captured");
    ASSERT_TRUE(payment->provider_capture_id.has_value());
    EXPECT_EQ(*payment->provider_capture_id, "CAPTURE-NEVERRETURNED-1");
}

// The return flow already credited this order (fake->next_capture_id is the
// capture id BillingController::capture recorded) — the webhook redelivering
// (or independently reporting) the same completed capture must be a true
// no-op: exactly one ledger row, unchanged balance.
TEST_F(BillingApiTest, WebhookAfterCaptureIsNoop) {
    auto user = seed_user("webhook-afterreturn@example.com");
    fake->next_order_id = "ORDER-AFTERRETURN-1";
    do_topup(user, json{{"amount_cents", 1000}});
    fake->capture_amount_cents = 1000;
    fake->capture_currency = "USD";
    int capture_status = 0;
    do_capture(user, "ORDER-AFTERRETURN-1", &capture_status);
    ASSERT_EQ(capture_status, k200OK);
    ASSERT_EQ(Billing::balance_of(user.subject, /*from_primary=*/true), 1000);

    int status = 0;
    auto body =
        do_webhook(capture_completed_event(
                       "WH-AFTERRETURN-1", "ORDER-AFTERRETURN-1", fake->next_capture_id /* same capture id */, "10.00"),
                   &status);
    EXPECT_EQ(status, k200OK);
    EXPECT_TRUE(body["data"]["handled"].get<bool>());

    EXPECT_EQ(Billing::balance_of(user.subject, /*from_primary=*/true), 1000);
    EXPECT_EQ(Billing::history(user.subject, 10, 0, /*from_primary=*/true).size(), 1u);  // exactly one ledger row
}

// A capture that was PENDING when the user hit the return-flow /capture
// endpoint (BillingController::capture leaves provider_capture_id NULL for
// that) later resolves via this webhook. Redelivering the SAME webhook a
// second time must not credit twice.
TEST_F(BillingApiTest, WebhookResolvesPendingCaptureExactlyOnce) {
    auto user = seed_user("webhook-pending@example.com");
    fake->next_order_id = "ORDER-PENDING-WH-1";
    do_topup(user, json{{"amount_cents", 1000}});
    fake->capture_amount_cents = 1000;
    fake->capture_currency = "USD";
    fake->capture_status = "PENDING";
    int pending_status = 0;
    auto pending_body = do_capture(user, "ORDER-PENDING-WH-1", &pending_status);
    ASSERT_EQ(pending_status, k200OK);
    ASSERT_FALSE(pending_body["data"]["credited"].get<bool>());
    ASSERT_EQ(Billing::balance_of(user.subject), 0);
    auto pending_payment = payments.find_by_order_id("ORDER-PENDING-WH-1");
    ASSERT_TRUE(pending_payment.has_value());
    ASSERT_FALSE(pending_payment->provider_capture_id.has_value());

    auto event = capture_completed_event(
        "WH-PENDING-RESOLVED-1", "ORDER-PENDING-WH-1", fake->next_capture_id /* PayPal's real capture id */, "10.00");

    int first_status = 0;
    auto first_body = do_webhook(event, &first_status);
    EXPECT_EQ(first_status, k200OK);
    EXPECT_TRUE(first_body["data"]["handled"].get<bool>());
    EXPECT_EQ(Billing::balance_of(user.subject, /*from_primary=*/true), 1000);
    EXPECT_EQ(Billing::history(user.subject, 10, 0, /*from_primary=*/true).size(), 1u);

    // Redelivery of the exact same event — credit_capture's guarded UPDATE
    // (WHERE provider_capture_id IS NULL) makes this a no-op.
    int second_status = 0;
    auto second_body = do_webhook(event, &second_status);
    EXPECT_EQ(second_status, k200OK);
    EXPECT_TRUE(second_body["data"]["handled"].get<bool>());
    EXPECT_EQ(Billing::balance_of(user.subject, /*from_primary=*/true), 1000);           // unchanged
    EXPECT_EQ(Billing::history(user.subject, 10, 0, /*from_primary=*/true).size(), 1u);  // still exactly one row
}

TEST_F(BillingApiTest, WebhookRefundWritesNegativeEntry) {
    auto user = seed_user("webhook-refund@example.com");
    fake->next_order_id = "ORDER-REFUND-WH-1";
    do_topup(user, json{{"amount_cents", 1000}});
    fake->capture_amount_cents = 1000;
    fake->capture_currency = "USD";
    int capture_status = 0;
    do_capture(user, "ORDER-REFUND-WH-1", &capture_status);
    ASSERT_EQ(capture_status, k200OK);
    ASSERT_EQ(Billing::balance_of(user.subject, /*from_primary=*/true), 1000);

    int status = 0;
    auto body =
        do_webhook(capture_refunded_event("WH-REFUND-1", "REFUND-WH-1", fake->next_capture_id, "10.00"), &status);
    EXPECT_EQ(status, k200OK);
    EXPECT_TRUE(body["data"]["handled"].get<bool>());

    EXPECT_EQ(Billing::balance_of(user.subject, /*from_primary=*/true), 0);
    auto hist = Billing::history(user.subject, 10, 0, /*from_primary=*/true);
    ASSERT_EQ(hist.size(), 2u);          // topup + refund
    const auto& refund_entry = hist[0];  // newest first
    EXPECT_EQ(refund_entry.kind, "refund");
    EXPECT_EQ(refund_entry.delta_credits, -1000);
    EXPECT_EQ(refund_entry.reference, "REFUND-WH-1");  // PayPal's refund id is the idempotency marker

    auto payment = payments.find_by_order_id("ORDER-REFUND-WH-1", /*from_primary=*/true);
    ASSERT_TRUE(payment.has_value());
    EXPECT_EQ(payment->status, "refunded");
}

TEST_F(BillingApiTest, WebhookDuplicateRefundEventIsNoop) {
    auto user = seed_user("webhook-refund-dup@example.com");
    fake->next_order_id = "ORDER-REFUND-DUP-1";
    do_topup(user, json{{"amount_cents", 1000}});
    fake->capture_amount_cents = 1000;
    fake->capture_currency = "USD";
    do_capture(user, "ORDER-REFUND-DUP-1");
    ASSERT_EQ(Billing::balance_of(user.subject, /*from_primary=*/true), 1000);

    auto event = capture_refunded_event("WH-REFUND-DUP-1", "REFUND-DUP-1", fake->next_capture_id, "10.00");

    int first_status = 0;
    do_webhook(event, &first_status);
    ASSERT_EQ(first_status, k200OK);
    ASSERT_EQ(Billing::balance_of(user.subject, /*from_primary=*/true), 0);
    ASSERT_EQ(Billing::history(user.subject, 10, 0, /*from_primary=*/true).size(), 2u);

    // Redelivery of the exact same refund event (same PayPal refund id) —
    // Billing::refund_capture's durable billing_refunds marker makes this a
    // no-op: no second negative ledger row, balance unchanged.
    int second_status = 0;
    auto second_body = do_webhook(event, &second_status);
    EXPECT_EQ(second_status, k200OK);
    EXPECT_TRUE(second_body["data"]["handled"].get<bool>());
    EXPECT_EQ(Billing::balance_of(user.subject, /*from_primary=*/true), 0);
    EXPECT_EQ(Billing::history(user.subject, 10, 0, /*from_primary=*/true).size(), 2u);  // still just topup + refund
}

TEST_F(BillingApiTest, WebhookIgnoresUnrelatedEventTypes) {
    auto user = seed_user("webhook-unrelated@example.com");
    fake->next_order_id = "ORDER-UNRELATED-1";
    do_topup(user, json{{"amount_cents", 1000}});

    json event = {{"id", "WH-UNRELATED-1"}, {"event_type", "CHECKOUT.ORDER.APPROVED"}, {"resource", json::object()}};
    int status = 0;
    auto body = do_webhook(event, &status);
    EXPECT_EQ(status, k200OK);
    EXPECT_FALSE(body["data"]["handled"].get<bool>());

    EXPECT_EQ(Billing::balance_of(user.subject), 0);
    EXPECT_EQ(Billing::history(user.subject, 10, 0).size(), 0u);
    auto payment = payments.find_by_order_id("ORDER-UNRELATED-1");
    ASSERT_TRUE(payment.has_value());
    EXPECT_FALSE(payment->provider_capture_id.has_value());
}

// KNOWN GAP (see BillingController::handleCaptureReversed): a reversal event
// still acks 200 (nothing to retry into existing) but must NOT touch the
// wallet or the original refund's billing_refunds row — there's no code
// path here that could even if it tried, this just pins that down.
TEST_F(BillingApiTest, WebhookReversalEventIsAcknowledgedButNotApplied) {
    auto user = seed_user("webhook-reversed@example.com");
    fake->next_order_id = "ORDER-REVERSED-1";
    do_topup(user, json{{"amount_cents", 1000}});
    fake->capture_amount_cents = 1000;
    fake->capture_currency = "USD";
    do_capture(user, "ORDER-REVERSED-1");
    do_webhook(capture_refunded_event("WH-REVERSED-REFUND-1", "REFUND-REVERSED-1", fake->next_capture_id, "10.00"));
    ASSERT_EQ(Billing::balance_of(user.subject, /*from_primary=*/true), 0);
    auto balance_before = Billing::balance_of(user.subject, /*from_primary=*/true);
    auto hist_before = Billing::history(user.subject, 10, 0, /*from_primary=*/true).size();

    json reversal = {
        {"id", "WH-REVERSED-1"},
        {"event_type", "PAYMENT.CAPTURE.REVERSED"},
        {"resource",
         {{"id", "REFUND-REVERSED-1"},
          {"links",
           json::array(
               {{{"rel", "up"},
                 {"href", "https://api.sandbox.paypal.com/v2/payments/captures/" + fake->next_capture_id}}})}}}};
    int status = 0;
    auto body = do_webhook(reversal, &status);
    EXPECT_EQ(status, k200OK);
    EXPECT_FALSE(body["data"]["handled"].get<bool>());

    EXPECT_EQ(Billing::balance_of(user.subject, /*from_primary=*/true), balance_before);
    EXPECT_EQ(Billing::history(user.subject, 10, 0, /*from_primary=*/true).size(), hist_before);
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

// MINOR fix-round-1: created_by (an admin's raw UUID) must never leak into
// the user-facing wallet view, even for ledger kinds (adjustment) that carry
// one internally.
TEST_F(BillingApiTest, WalletHistoryNeverExposesCreatedBy) {
    auto user = seed_user("adjusted-user@example.com");
    auto admin = seed_user("some-admin@example.com");
    Billing::adjust(user.subject, 250, "goodwill credit", admin.subject);

    auto wallet = do_wallet(user);
    ASSERT_EQ(wallet["data"]["history"].size(), 1u);
    const auto& entry = wallet["data"]["history"][0];
    EXPECT_EQ(entry["kind"], "adjustment");
    EXPECT_FALSE(entry.contains("created_by"));
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

    controller.topup(TestHelpers::authed_json(user, json{{"amount_cents", 500}}),
                     [&](const HttpResponsePtr& r) { resp = r; });
    EXPECT_EQ(resp->statusCode(), k404NotFound);

    controller.capture(TestHelpers::authed_json(user, json{{"order_id", "X"}}),
                       [&](const HttpResponsePtr& r) { resp = r; });
    EXPECT_EQ(resp->statusCode(), k404NotFound);
}

}  // namespace
