/**
 * @file test_admin_billing_api.cpp
 * @brief Integration tests for AdminBillingController (Task 6): the module
 *        guard + admin gate on every route, package CRUD, the billing
 *        rate/bounds settings round-trip (including that it changes what a
 *        NEW top-up computes without touching an already-created payment's
 *        frozen snapshot — Task 2/4 behavior), the payments list filters,
 *        and manual wallet adjustments (mandatory note, audit trail,
 *        created_by attribution, and the optional `notify` adjustment-email
 *        flag from the billing-emails-dashboard Task 2 brief).
 *
 * The `notify` cases observe email delivery the same way
 * test_billing_emails.cpp does: jobs enabled + mail.enabled=false, so a
 * dispatched BillingEmails::adjustment() shows up as exactly one
 * "email.send" job on the Redis queue (Email::SendEmail::kJobType) —
 * nothing here asserts against a real SMTP connection.
 */

#include <memory>
#include <string>
#include <vector>

#include <drogon/HttpRequest.h>
#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "api/AdminBillingController.hpp"
#include "api/BillingController.hpp"
#include "billing/PayPalClient.hpp"
#include "billing/Wallet.hpp"
#include "cache/Cache.hpp"
#include "database/Database.hpp"
#include "email/GenericEmail.hpp"
#include "jobs/Jobs.hpp"
#include "repositories/BillingRepository.hpp"
#include "repositories/RoleRepository.hpp"
#include "repositories/UserRepository.hpp"
#include "security/Auth.hpp"
#include "test_helpers.hpp"

using namespace drogon;
using json = nlohmann::json;

namespace {

constexpr const char* kSecret = "test-jwt-secret-for-admin-billing-api-flow-pad";

/// Minimal PayPal test double — only create_order is needed to prove a NEW
/// top-up picks up a settings change; capture_order is never exercised here.
class TopupFakePayPalClient : public Billing::PayPalClient {
public:
    TopupFakePayPalClient() : Billing::PayPalClient(Billing::PayPalClientConfig{}) {}

    std::string next_order_id = "ORDER-FAKE";

    Billing::PayPalOrder create_order(std::int64_t /*amount_cents*/,
                                      const std::string& /*currency*/,
                                      const std::string& /*reference*/,
                                      const std::string& /*return_url*/,
                                      const std::string& /*cancel_url*/) override {
        Billing::PayPalOrder out;
        out.order_id = next_order_id;
        out.approve_url = "https://paypal.example.com/checkoutnow?token=" + next_order_id;
        return out;
    }

    Billing::PayPalCapture capture_order(const std::string& /*order_id*/) override {
        ADD_FAILURE() << "capture_order should never be called in this suite";
        return {};
    }
};

class AdminBillingApiTest : public TestHelpers::CoreBackedTest {
protected:
    Api::AdminBillingController controller;
    Repositories::RoleRepository roles;
    Repositories::UserRepository users;
    Repositories::PaymentRepository payments;
    Repositories::PackageRepository packages;
    Repositories::BillingSettingsRepository settings;

    std::string config_file_name() const override { return "admin_billing_api_test_config.json"; }

    void config_overrides(json& cfg) override {
        // Every route in this controller is admin-gated in-handler via
        // API_REQUIRE_ADMIN, which is a no-op unless auth.mode != "none"
        // (see Guards.hpp / Security::Auth::require_admin). Without this,
        // NonAdminGets403OnEveryRoute would pass trivially for the wrong
        // reason — the guard never runs, not because it correctly rejects.
        // Matches the convention in test_posts_api.cpp / test_admin_flow.cpp.
        cfg["auth"]["mode"] = "jwt";
        cfg["auth"]["jwt"]["secret"] = kSecret;
        cfg["database"]["migrations_enabled"] = true;
        cfg["database"]["migrations_dir"] = "migrations";
        cfg["billing"]["enabled"] = true;
        cfg["billing"]["provider"] = "paypal";
        cfg["billing"]["currency"] = "USD";
        // These match migration 009's seeded defaults exactly (100/100/100000)
        // — kept here only as the defensive config fallback billing_limits()
        // uses if the billing_settings row were ever missing.
        cfg["billing"]["credits_per_unit"] = 100;
        cfg["billing"]["min_amount_cents"] = 100;
        cfg["billing"]["max_amount_cents"] = 100000;
        // Jobs on, mail off — same pattern test_billing_emails.cpp uses to
        // observe adjustWallet's optional `notify` dispatch via the Redis
        // queue instead of a real SMTP connection (see that file's config
        // comment for why mail.enabled=false alone does NOT stop enqueueing).
        cfg["jobs"]["enabled"] = true;
        cfg["mail"]["enabled"] = false;
    }

    void SetUp() override {
        TestHelpers::CoreBackedTest::SetUp();
        if (::testing::Test::IsSkipped())
            return;
        Database::get().execute_write([](auto& txn) {
            txn.exec(
                "TRUNCATE TABLE wallet_entries, wallet_balances, billing_refunds, payments, billing_packages CASCADE");
            txn.exec("TRUNCATE TABLE users CASCADE");
            txn.exec("TRUNCATE TABLE audit_log CASCADE");
            txn.exec("DELETE FROM roles WHERE name NOT IN ('User', 'Administrator')");
            // billing_settings is seeded exactly once by migration 009 and is
            // never truncated (its repository assumes the single row always
            // exists) — reset it to the known defaults so a settings-mutating
            // test can't leak state into whichever test runs next.
            txn.exec(
                "UPDATE billing_settings SET credits_per_unit = 100, min_amount_cents = 100, "
                "max_amount_cents = 100000 WHERE id = 1");
            return 0;
        });
        drain_queue();
    }

    // billing_settings is a single, never-truncated row shared with every
    // other billing suite in this binary (test_billing_api.cpp reads it via
    // the same BillingController::billing_limits() this suite mutates
    // directly) — reset it on the way out too, not just on the way in, so a
    // settings-mutating test here can't leave a non-default rate for
    // whichever suite the test runner happens to execute next.
    void TearDown() override {
        if (!::testing::Test::IsSkipped()) {
            drain_queue();
            Database::get().execute_write([](auto& txn) {
                txn.exec(
                    "UPDATE billing_settings SET credits_per_unit = 100, min_amount_cents = 100, "
                    "max_amount_cents = 100000 WHERE id = 1");
                return 0;
            });
        }
        TestHelpers::CoreBackedTest::TearDown();
    }

    static void drain_queue() { TestHelpers::drain_jobs({Email::SendEmail::kJobType}); }

    static long queue_depth() {
        return static_cast<long>(Cache::get().get_client().llen(Jobs::queue_key(Email::SendEmail::kJobType)));
    }

    // Reads the single job currently on the email.send queue and asserts
    // there is exactly one — mirrors test_billing_emails.cpp's helper of the
    // same name.
    static json only_enqueued_email_job() {
        auto& redis = Cache::get().get_client();
        std::vector<std::string> ids;
        redis.lrange(Jobs::queue_key(Email::SendEmail::kJobType), 0, -1, std::back_inserter(ids));
        EXPECT_EQ(ids.size(), 1u);
        if (ids.empty())
            return json::object();
        auto job = Jobs::get().get_status(ids[0]);
        EXPECT_TRUE(job.has_value());
        return job ? job->payload : json::object();
    }

    Security::Auth::AuthPrincipal seed_principal(const std::string& email, const std::string& role_name) {
        auto role = roles.find_by_name(role_name);
        EXPECT_TRUE(role.has_value());
        auto u = users.create(email, std::string("$argon2id$placeholder"), std::nullopt, std::nullopt, role->id, true);
        Security::Auth::AuthPrincipal p;
        p.subject = u.id;
        if (role)
            p.roles.push_back(role->name);
        p.raw_claims = json{{"sub", u.id}, {"permissions", role ? role->permissions : 0u}};
        return p;
    }

    Security::Auth::AuthPrincipal seed_admin(const std::string& email = "admin@example.com") {
        return seed_principal(email, "Administrator");
    }

    Security::Auth::AuthPrincipal seed_user(const std::string& email) { return seed_principal(email, "User"); }

    Domain::Package seed_package(const std::string& title, std::int64_t amount_cents, std::int64_t credits) {
        return packages.create(title, amount_cents, credits, /*active=*/true, /*sort=*/0);
    }

    // Default 1000-cents-for-1000-credits at rate 100 — matches test_wallet.cpp's
    // convention. @p status is applied via a direct UPDATE after creation
    // (PaymentRepository::create always lands rows in 'created').
    Domain::Payment seed_payment(const std::string& user_id,
                                 const std::string& order_id,
                                 const std::string& status = "captured",
                                 std::int64_t amount_cents = 1000,
                                 std::int64_t credits_expected = 1000,
                                 std::int64_t rate_snapshot = 100) {
        auto p = payments.create(user_id, order_id, amount_cents, "USD", credits_expected, rate_snapshot, std::nullopt);
        if (status != "created") {
            Database::get().execute_write([&](auto& txn) {
                txn.exec_params("UPDATE payments SET status = $1 WHERE id = $2", status, p.id);
                return 0;
            });
            p.status = status;
        }
        return p;
    }

    long audit_count(const std::string& action, const std::string& target_id) {
        return Database::get().execute_read([&](auto& txn) {
            auto r = txn.exec_params(
                "SELECT COUNT(*) FROM audit_log WHERE action = $1 AND target_id = $2", action, target_id);
            return r[0][0].template as<long>();
        });
    }
};

// ── admin gate + module guard ───────────────────────────────────────────────

TEST_F(AdminBillingApiTest, NonAdminGets403OnEveryRoute) {
    auto user = seed_user("plain@example.com");
    auto victim = seed_user("victim@example.com");
    auto pkg = seed_package("Existing", 500, 500);

    HttpResponsePtr resp;

    controller.listPayments(TestHelpers::authed(user), [&](const HttpResponsePtr& r) { resp = r; });
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->statusCode(), k403Forbidden);

    controller.listPackages(TestHelpers::authed(user), [&](const HttpResponsePtr& r) { resp = r; });
    EXPECT_EQ(resp->statusCode(), k403Forbidden);

    controller.createPackage(
        TestHelpers::authed_json(user, json{{"title", "X"}, {"amount_cents", 100}, {"credits", 100}}),
        [&](const HttpResponsePtr& r) { resp = r; });
    EXPECT_EQ(resp->statusCode(), k403Forbidden);

    controller.updatePackage(
        TestHelpers::authed_json(user, json{{"title", "Y"}}, Patch),
        [&](const HttpResponsePtr& r) { resp = r; },
        pkg.id);
    EXPECT_EQ(resp->statusCode(), k403Forbidden);

    controller.deletePackage(
        TestHelpers::authed(user, Delete), [&](const HttpResponsePtr& r) { resp = r; }, pkg.id);
    EXPECT_EQ(resp->statusCode(), k403Forbidden);
    EXPECT_TRUE(packages.find(pkg.id).has_value());  // refused, not deleted

    controller.getSettings(TestHelpers::authed(user), [&](const HttpResponsePtr& r) { resp = r; });
    EXPECT_EQ(resp->statusCode(), k403Forbidden);

    controller.updateSettings(
        TestHelpers::authed_json(
            user, json{{"credits_per_unit", 1}, {"min_amount_cents", 1}, {"max_amount_cents", 2}}, Put),
        [&](const HttpResponsePtr& r) { resp = r; });
    EXPECT_EQ(resp->statusCode(), k403Forbidden);
    EXPECT_EQ(settings.get().credits_per_unit, 100);  // untouched

    controller.adjustWallet(
        TestHelpers::authed_json(user, json{{"delta_credits", 10}, {"note", "test"}}),
        [&](const HttpResponsePtr& r) { resp = r; },
        victim.subject);
    EXPECT_EQ(resp->statusCode(), k403Forbidden);
    EXPECT_EQ(Billing::balance_of(victim.subject), 0);
}

// ── package CRUD ─────────────────────────────────────────────────────────────

TEST_F(AdminBillingApiTest, PackageCrudRoundtrip) {
    auto admin = seed_admin();

    HttpResponsePtr resp;
    controller.createPackage(
        TestHelpers::authed_json(admin, json{{"title", "Starter"}, {"amount_cents", 500}, {"credits", 600}}),
        [&](const HttpResponsePtr& r) { resp = r; });
    ASSERT_EQ(resp->statusCode(), k201Created);
    auto created = json::parse(std::string(resp->body()))["data"];
    const std::string id = created["id"].get<std::string>();
    EXPECT_EQ(created["title"], "Starter");
    EXPECT_EQ(created["amount_cents"], 500);
    EXPECT_EQ(created["credits"], 600);
    EXPECT_TRUE(created["active"].get<bool>());
    EXPECT_EQ(audit_count("billing.package.create", id), 1);

    controller.listPackages(TestHelpers::authed(admin), [&](const HttpResponsePtr& r) { resp = r; });
    ASSERT_EQ(resp->statusCode(), k200OK);
    auto list_body = json::parse(std::string(resp->body()));
    ASSERT_EQ(list_body["data"].size(), 1u);
    EXPECT_EQ(list_body["data"][0]["id"], id);

    // Partial update: only title + active change; amount_cents/credits untouched.
    controller.updatePackage(
        TestHelpers::authed_json(admin, json{{"title", "Starter Plus"}, {"active", false}}, Patch),
        [&](const HttpResponsePtr& r) { resp = r; },
        id);
    ASSERT_EQ(resp->statusCode(), k200OK);
    auto updated = json::parse(std::string(resp->body()))["data"];
    EXPECT_EQ(updated["title"], "Starter Plus");
    EXPECT_FALSE(updated["active"].get<bool>());
    EXPECT_EQ(updated["amount_cents"], 500);
    EXPECT_EQ(updated["credits"], 600);
    EXPECT_EQ(audit_count("billing.package.update", id), 1);

    // Admin list still shows it (inactive included, unlike the user-facing view).
    controller.listPackages(TestHelpers::authed(admin), [&](const HttpResponsePtr& r) { resp = r; });
    list_body = json::parse(std::string(resp->body()));
    ASSERT_EQ(list_body["data"].size(), 1u);
    EXPECT_FALSE(list_body["data"][0]["active"].get<bool>());

    controller.deletePackage(
        TestHelpers::authed(admin, Delete), [&](const HttpResponsePtr& r) { resp = r; }, id);
    ASSERT_EQ(resp->statusCode(), k200OK);
    EXPECT_EQ(audit_count("billing.package.delete", id), 1);
    EXPECT_FALSE(packages.find(id).has_value());

    // Deleting again (unknown id) -> 404, not a second audit row.
    controller.deletePackage(
        TestHelpers::authed(admin, Delete), [&](const HttpResponsePtr& r) { resp = r; }, id);
    EXPECT_EQ(resp->statusCode(), k404NotFound);
    EXPECT_EQ(audit_count("billing.package.delete", id), 1);
}

TEST_F(AdminBillingApiTest, CreatePackageRejectsInvalidFields) {
    auto admin = seed_admin();
    HttpResponsePtr resp;
    controller.createPackage(TestHelpers::authed_json(admin, json{{"title", ""}, {"amount_cents", 0}, {"credits", -5}}),
                             [&](const HttpResponsePtr& r) { resp = r; });
    EXPECT_EQ(resp->statusCode(), k400BadRequest);
}

TEST_F(AdminBillingApiTest, UpdatePackageRejectsEmptyPatchAndUnknownId) {
    auto admin = seed_admin();
    auto pkg = seed_package("Existing", 500, 500);
    HttpResponsePtr resp;

    controller.updatePackage(
        TestHelpers::authed_json(admin, json::object(), Patch), [&](const HttpResponsePtr& r) { resp = r; }, pkg.id);
    EXPECT_EQ(resp->statusCode(), k400BadRequest);

    controller.updatePackage(
        TestHelpers::authed_json(admin, json{{"title", "Nope"}}, Patch),
        [&](const HttpResponsePtr& r) { resp = r; },
        "00000000-0000-0000-0000-000000000000");
    EXPECT_EQ(resp->statusCode(), k404NotFound);
}

// ── settings ────────────────────────────────────────────────────────────────

TEST_F(AdminBillingApiTest, SettingsUpdateChangesComputedCredits) {
    auto admin = seed_admin();
    auto user = seed_user("buyer@example.com");

    // An in-flight payment created under the OLD rate (100).
    auto old_payment = seed_payment(user.subject,
                                    "ORDER-OLD",
                                    "created",
                                    /*amount_cents=*/1000,
                                    /*credits_expected=*/1000,
                                    /*rate_snapshot=*/100);

    HttpResponsePtr resp;
    controller.getSettings(TestHelpers::authed(admin), [&](const HttpResponsePtr& r) { resp = r; });
    ASSERT_EQ(resp->statusCode(), k200OK);
    auto before = json::parse(std::string(resp->body()))["data"];
    EXPECT_EQ(before["credits_per_unit"], 100);

    // Change the rate.
    controller.updateSettings(
        TestHelpers::authed_json(
            admin, json{{"credits_per_unit", 50}, {"min_amount_cents", 100}, {"max_amount_cents", 100000}}, Put),
        [&](const HttpResponsePtr& r) { resp = r; });
    ASSERT_EQ(resp->statusCode(), k200OK);
    auto after = json::parse(std::string(resp->body()))["data"];
    EXPECT_EQ(after["credits_per_unit"], 50);
    EXPECT_EQ(audit_count("billing.settings.update", "1"), 1);

    // The in-flight payment's frozen snapshot is untouched by the rate change.
    auto reloaded_old = payments.find(old_payment.id);
    ASSERT_TRUE(reloaded_old.has_value());
    EXPECT_EQ(reloaded_old->rate_snapshot, 100);
    EXPECT_EQ(reloaded_old->credits_expected, 1000);

    // A NEW top-up, driven through the real user-facing controller, computes
    // with the NEW rate — proves settings -> BillingController::billing_limits()
    // -> resolve_topup_plan is actually wired, not just an admin-side round-trip.
    Api::BillingController billing_controller;
    auto owned = std::make_unique<TopupFakePayPalClient>();
    auto* fake = owned.get();
    Billing::install_for_testing(std::move(owned));
    fake->next_order_id = "ORDER-NEW";

    HttpResponsePtr topup_resp;
    billing_controller.topup(TestHelpers::authed_json(user, json{{"amount_cents", 1000}}),
                             [&](const HttpResponsePtr& r) { topup_resp = r; });
    ASSERT_EQ(topup_resp->statusCode(), k201Created);
    auto new_payment = payments.find_by_order_id("ORDER-NEW");
    ASSERT_TRUE(new_payment.has_value());
    EXPECT_EQ(new_payment->rate_snapshot, 50);
    EXPECT_EQ(new_payment->credits_expected, 500);  // 1000 cents * 50 / 100

    Billing::reset_for_testing();
}

TEST_F(AdminBillingApiTest, UpdateSettingsRejectsInvalidBounds) {
    auto admin = seed_admin();
    HttpResponsePtr resp;

    // max < min.
    controller.updateSettings(
        TestHelpers::authed_json(
            admin, json{{"credits_per_unit", 100}, {"min_amount_cents", 5000}, {"max_amount_cents", 100}}, Put),
        [&](const HttpResponsePtr& r) { resp = r; });
    EXPECT_EQ(resp->statusCode(), k400BadRequest);

    // Missing field.
    controller.updateSettings(TestHelpers::authed_json(admin, json{{"credits_per_unit", 100}}, Put),
                              [&](const HttpResponsePtr& r) { resp = r; });
    EXPECT_EQ(resp->statusCode(), k400BadRequest);

    // Non-positive.
    controller.updateSettings(
        TestHelpers::authed_json(
            admin, json{{"credits_per_unit", 0}, {"min_amount_cents", 100}, {"max_amount_cents", 1000}}, Put),
        [&](const HttpResponsePtr& r) { resp = r; });
    EXPECT_EQ(resp->statusCode(), k400BadRequest);

    // Settings untouched by any of the above.
    EXPECT_EQ(settings.get().credits_per_unit, 100);
}

// ── manual adjustment ────────────────────────────────────────────────────────

TEST_F(AdminBillingApiTest, AdjustRequiresNoteAndWritesAudit) {
    auto admin = seed_admin();
    auto target = seed_user("target@example.com");

    HttpResponsePtr resp;

    // Empty note -> 400, nothing written.
    controller.adjustWallet(
        TestHelpers::authed_json(admin, json{{"delta_credits", 100}, {"note", ""}}),
        [&](const HttpResponsePtr& r) { resp = r; },
        target.subject);
    EXPECT_EQ(resp->statusCode(), k400BadRequest);
    EXPECT_EQ(Billing::balance_of(target.subject), 0);
    EXPECT_EQ(audit_count("billing.wallet.adjust", target.subject), 0);

    // Missing note entirely -> 400.
    controller.adjustWallet(
        TestHelpers::authed_json(admin, json{{"delta_credits", 100}}),
        [&](const HttpResponsePtr& r) { resp = r; },
        target.subject);
    EXPECT_EQ(resp->statusCode(), k400BadRequest);
    EXPECT_EQ(Billing::balance_of(target.subject), 0);

    // Zero delta -> 400 (Billing::adjust's own ZeroAdjustment, surfaced via with_repo_errors).
    controller.adjustWallet(
        TestHelpers::authed_json(admin, json{{"delta_credits", 0}, {"note", "no-op"}}),
        [&](const HttpResponsePtr& r) { resp = r; },
        target.subject);
    EXPECT_EQ(resp->statusCode(), k400BadRequest);

    // Success: non-empty note, writes created_by == admin + one audit_log row.
    controller.adjustWallet(
        TestHelpers::authed_json(admin, json{{"delta_credits", 250}, {"note", "welcome bonus"}}),
        [&](const HttpResponsePtr& r) { resp = r; },
        target.subject);
    ASSERT_EQ(resp->statusCode(), k200OK);
    auto body = json::parse(std::string(resp->body()));
    EXPECT_EQ(body["data"]["balance"], 250);
    EXPECT_TRUE(body["data"]["credited"].get<bool>());

    auto hist = Billing::history(target.subject, 10, 0);
    ASSERT_EQ(hist.size(), 1u);
    EXPECT_EQ(hist[0].delta_credits, 250);
    EXPECT_EQ(hist[0].kind, "adjustment");
    EXPECT_EQ(hist[0].note, "welcome bonus");
    ASSERT_TRUE(hist[0].created_by.has_value());
    EXPECT_EQ(*hist[0].created_by, admin.subject);

    EXPECT_EQ(audit_count("billing.wallet.adjust", target.subject), 1);
}

TEST_F(AdminBillingApiTest, AdjustRejectsMalformedUserIdBeforeTouchingWallet) {
    auto admin = seed_admin();
    HttpResponsePtr resp;
    controller.adjustWallet(
        TestHelpers::authed_json(admin, json{{"delta_credits", 100}, {"note", "test"}}),
        [&](const HttpResponsePtr& r) { resp = r; },
        "not-a-uuid");
    EXPECT_EQ(resp->statusCode(), k400BadRequest);
}

TEST_F(AdminBillingApiTest, AdjustNegativeBeyondBalanceIsConflict) {
    auto admin = seed_admin();
    auto target = seed_user("debtor@example.com");
    HttpResponsePtr resp;
    controller.adjustWallet(
        TestHelpers::authed_json(admin, json{{"delta_credits", -50}, {"note", "overdraft attempt"}}),
        [&](const HttpResponsePtr& r) { resp = r; },
        target.subject);
    EXPECT_EQ(resp->statusCode(), k409Conflict);
    EXPECT_EQ(Billing::balance_of(target.subject), 0);
}

// ── manual adjustment: optional `notify` email (Task 2, billing-emails-dashboard) ──

TEST_F(AdminBillingApiTest, AdjustNotifyTrueEnqueuesExactlyOneAdjustmentEmailWithReason) {
    auto admin = seed_admin();
    auto target = seed_user("notify-target@example.com");

    HttpResponsePtr resp;
    controller.adjustWallet(
        TestHelpers::authed_json(admin, json{{"delta_credits", 250}, {"note", "welcome bonus"}, {"notify", true}}),
        [&](const HttpResponsePtr& r) { resp = r; },
        target.subject);
    ASSERT_EQ(resp->statusCode(), k200OK);
    auto body = json::parse(std::string(resp->body()));
    EXPECT_EQ(body["data"]["balance"], 250);

    // Audit row is still written regardless of notify.
    EXPECT_EQ(audit_count("billing.wallet.adjust", target.subject), 1);

    ASSERT_EQ(queue_depth(), 1);
    auto payload = only_enqueued_email_job();
    EXPECT_EQ(payload["to"], "notify-target@example.com");
    EXPECT_EQ(payload["subject"], "Your wallet balance was adjusted");
    // reason == the mandatory `note`, delta_credits rendered signed ("+250"),
    // new_balance == Billing::CreditResult::balance from the adjust() call.
    EXPECT_NE(payload["text"].get<std::string>().find("welcome bonus"), std::string::npos);
    EXPECT_NE(payload["text"].get<std::string>().find("+250 credits"), std::string::npos);
    EXPECT_NE(payload["text"].get<std::string>().find("New balance: 250 credits"), std::string::npos);
}

TEST_F(AdminBillingApiTest, AdjustNotifyTrueFormatsNegativeDeltaWithMinusSign) {
    auto admin = seed_admin();
    auto target = seed_user("notify-debit@example.com");

    // Seed a starting balance to debit from (notify omitted — this call
    // itself must not enqueue anything).
    HttpResponsePtr seed_resp;
    controller.adjustWallet(
        TestHelpers::authed_json(admin, json{{"delta_credits", 500}, {"note", "seed"}}),
        [&](const HttpResponsePtr& r) { seed_resp = r; },
        target.subject);
    ASSERT_EQ(seed_resp->statusCode(), k200OK);
    ASSERT_EQ(queue_depth(), 0);

    HttpResponsePtr resp;
    controller.adjustWallet(
        TestHelpers::authed_json(admin, json{{"delta_credits", -75}, {"note", "correction"}, {"notify", true}}),
        [&](const HttpResponsePtr& r) { resp = r; },
        target.subject);
    ASSERT_EQ(resp->statusCode(), k200OK);

    ASSERT_EQ(queue_depth(), 1);
    auto payload = only_enqueued_email_job();
    EXPECT_NE(payload["text"].get<std::string>().find("-75 credits"), std::string::npos);
    EXPECT_NE(payload["text"].get<std::string>().find("New balance: 425 credits"), std::string::npos);
    EXPECT_NE(payload["text"].get<std::string>().find("correction"), std::string::npos);
}

TEST_F(AdminBillingApiTest, AdjustNotifyFalseOrOmittedEnqueuesNoEmail) {
    auto admin = seed_admin();
    auto target = seed_user("no-notify@example.com");

    HttpResponsePtr resp;

    // notify omitted entirely -> defaults to false, no email.
    controller.adjustWallet(
        TestHelpers::authed_json(admin, json{{"delta_credits", 100}, {"note", "no notify field"}}),
        [&](const HttpResponsePtr& r) { resp = r; },
        target.subject);
    ASSERT_EQ(resp->statusCode(), k200OK);
    EXPECT_EQ(queue_depth(), 0);

    // notify explicitly false -> still no email.
    controller.adjustWallet(
        TestHelpers::authed_json(admin, json{{"delta_credits", 50}, {"note", "explicit false"}, {"notify", false}}),
        [&](const HttpResponsePtr& r) { resp = r; },
        target.subject);
    ASSERT_EQ(resp->statusCode(), k200OK);
    EXPECT_EQ(queue_depth(), 0);

    // Both adjustments still wrote their audit rows regardless of notify.
    EXPECT_EQ(audit_count("billing.wallet.adjust", target.subject), 2);
}

TEST_F(AdminBillingApiTest, AdjustEmptyNoteStays400EvenWithNotifyTrue) {
    auto admin = seed_admin();
    auto target = seed_user("bad-note@example.com");

    // notify=true must not bypass the mandatory-note validation — the 400
    // happens before Billing::adjust (and therefore before any possible
    // dispatch) is ever reached.
    HttpResponsePtr resp;
    controller.adjustWallet(
        TestHelpers::authed_json(admin, json{{"delta_credits", 100}, {"note", ""}, {"notify", true}}),
        [&](const HttpResponsePtr& r) { resp = r; },
        target.subject);
    EXPECT_EQ(resp->statusCode(), k400BadRequest);
    EXPECT_EQ(Billing::balance_of(target.subject), 0);
    EXPECT_EQ(audit_count("billing.wallet.adjust", target.subject), 0);
    EXPECT_EQ(queue_depth(), 0);
}

// ── payments list ────────────────────────────────────────────────────────────

TEST_F(AdminBillingApiTest, PaymentsListFiltersByStatusAndUser) {
    auto admin = seed_admin();
    auto alice = seed_user("alice@example.com");
    auto bob = seed_user("bob@example.com");

    auto p1 = seed_payment(alice.subject, "ORDER-A1", "captured");
    seed_payment(alice.subject, "ORDER-A2", "created");
    seed_payment(bob.subject, "ORDER-B1", "captured");

    HttpResponsePtr resp;

    // No filter: all three.
    controller.listPayments(TestHelpers::authed(admin), [&](const HttpResponsePtr& r) { resp = r; });
    ASSERT_EQ(resp->statusCode(), k200OK);
    auto body = json::parse(std::string(resp->body()));
    EXPECT_EQ(body["total"], 3);
    EXPECT_EQ(body["data"].size(), 3u);

    // status=captured -> p1 (alice) + bob's payment.
    auto req = TestHelpers::authed(admin);
    req->setParameter("status", "captured");
    controller.listPayments(req, [&](const HttpResponsePtr& r) { resp = r; });
    ASSERT_EQ(resp->statusCode(), k200OK);
    body = json::parse(std::string(resp->body()));
    EXPECT_EQ(body["total"], 2);
    for (const auto& row : body["data"])
        EXPECT_EQ(row["status"], "captured");

    // user_id=alice -> her two payments only.
    req = TestHelpers::authed(admin);
    req->setParameter("user_id", alice.subject);
    controller.listPayments(req, [&](const HttpResponsePtr& r) { resp = r; });
    ASSERT_EQ(resp->statusCode(), k200OK);
    body = json::parse(std::string(resp->body()));
    EXPECT_EQ(body["total"], 2);
    for (const auto& row : body["data"])
        EXPECT_EQ(row["user_id"], alice.subject);

    // Both filters combined -> exactly p1.
    req = TestHelpers::authed(admin);
    req->setParameter("status", "captured");
    req->setParameter("user_id", alice.subject);
    controller.listPayments(req, [&](const HttpResponsePtr& r) { resp = r; });
    ASSERT_EQ(resp->statusCode(), k200OK);
    body = json::parse(std::string(resp->body()));
    ASSERT_EQ(body["total"], 1);
    EXPECT_EQ(body["data"][0]["id"], p1.id);

    // Malformed user_id filter -> 400, not a 500 from a bad ::uuid cast.
    req = TestHelpers::authed(admin);
    req->setParameter("user_id", "not-a-uuid");
    controller.listPayments(req, [&](const HttpResponsePtr& r) { resp = r; });
    EXPECT_EQ(resp->statusCode(), k400BadRequest);
}

}  // namespace
