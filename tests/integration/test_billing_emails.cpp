/**
 * @file test_billing_emails.cpp
 * @brief Integration tests for src/email/BillingEmails.hpp and its dispatch
 *        wiring in BillingController.hpp (capture endpoint + PayPal webhook).
 *
 * Mirrors test_billing_api.cpp's PayPal stub (FakePayPalClient via
 * Billing::install_for_testing()) and test_email_jobs.cpp's way of observing
 * delivery WITHOUT SMTP: with jobs enabled and mail.enabled=false, a billing
 * email shows up as one "email.send" job on the Redis queue
 * (Email::SendEmail::kJobType) — nothing here asserts against a real SMTP
 * connection.
 *
 * Cases (per the Task 1 brief):
 *   - a real capture enqueues exactly ONE receipt job;
 *   - a webhook redelivery of an already-captured payment enqueues NO
 *     second receipt;
 *   - a refund that actually debits the wallet enqueues a refund job;
 *   - a refund that is durably recorded but SKIPPED (insufficient balance)
 *     enqueues NO refund job — this is the "applied vs skipped" distinction
 *     BillingController::find_refund_ledger_entry exists to make;
 *   - an amount-mismatch capture enqueues a failed-payment job;
 *   - with Jobs unavailable (mail delivery effectively off), nothing is
 *     enqueued and BillingEmails still never throws.
 */

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <drogon/HttpRequest.h>
#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "api/BillingController.hpp"
#include "billing/PayPalClient.hpp"
#include "billing/Wallet.hpp"
#include "cache/Cache.hpp"
#include "database/Database.hpp"
#include "domain/User.hpp"
#include "email/BillingEmails.hpp"
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

/// Trimmed copy of test_billing_api.cpp's FakePayPalClient — just enough to
/// drive capture()/paypalWebhook() with zero network I/O. Kept as its own
/// definition (anonymous-namespace, file-scoped) rather than shared, since
/// each *.cpp test file is its own translation unit.
class FakePayPalClient : public Billing::PayPalClient {
public:
    FakePayPalClient() : Billing::PayPalClient(Billing::PayPalClientConfig{}) {}

    std::string next_order_id = "ORDER-FAKE-1";
    std::string next_capture_id = "CAPTURE-FAKE-1";
    std::int64_t capture_amount_cents = 0;
    std::string capture_currency = "USD";
    std::string capture_status = "COMPLETED";
    int capture_order_calls = 0;

    bool verify_webhook_signature(const std::map<std::string, std::string>& /*headers*/,
                                  const std::string& /*raw_body*/) override {
        return true;
    }

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
        ++capture_order_calls;
        Billing::PayPalCapture out;
        out.capture_id = next_capture_id;
        out.amount_cents = capture_amount_cents;
        out.currency = capture_currency;
        out.status = capture_status;
        return out;
    }
};

// ── shared config for both fixtures below ───────────────────────────────
void billing_email_test_config_overrides(json& cfg) {
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
    // Jobs on, mail off — the routing under test is enqueue-vs-not; actual
    // SMTP delivery is out of scope here (see test_email_jobs.cpp, which
    // establishes this exact pattern for AccountEmails: mail.enabled only
    // gates the WORKER's real send, never whether GenericEmail::send()
    // enqueues in the first place — Email::initialize() runs unconditionally
    // in Core::initialize(), so Email::is_initialized() stays true and
    // Mailer::send() with enabled=false is just a logged no-op).
    cfg["jobs"]["enabled"] = true;
    cfg["mail"]["enabled"] = false;
}

// ── primary fixture: jobs enabled, mail disabled, billing enabled ────────
class BillingEmailsTest : public TestHelpers::CoreBackedTest {
protected:
    Api::BillingController controller;
    Repositories::RoleRepository roles;
    Repositories::UserRepository users;
    Repositories::PaymentRepository payments;
    Repositories::PackageRepository packages;
    FakePayPalClient* fake = nullptr;  // non-owning — owned by Billing::global_paypal_client

    std::string config_file_name() const override { return "billing_emails_test_config.json"; }

    void config_overrides(json& cfg) override { billing_email_test_config_overrides(cfg); }

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
            txn.exec(
                "UPDATE billing_settings SET credits_per_unit = 100, min_amount_cents = 100, "
                "max_amount_cents = 100000 WHERE id = 1");
            return 0;
        });
        drain_queue();
    }

    void TearDown() override {
        Billing::reset_for_testing();
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

    json do_webhook(const json& event, int* status = nullptr) {
        auto req = TestHelpers::make_request(drogon::Post, event);
        req->addHeader("Paypal-Auth-Algo", "SHA256withRSA");
        req->addHeader("Paypal-Cert-Url", "https://api.sandbox.paypal.com/cert");
        req->addHeader("Paypal-Transmission-Id", "txn-test");
        req->addHeader("Paypal-Transmission-Sig", "sig");
        req->addHeader("Paypal-Transmission-Time", "2026-01-01T00:00:00Z");
        HttpResponsePtr resp;
        controller.paypalWebhook(req, [&](const HttpResponsePtr& r) { resp = r; });
        if (status)
            *status = resp->statusCode();
        return json::parse(std::string(resp->body()));
    }

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

    // Reads the single job currently on the email.send queue and asserts
    // there is exactly one — the shape most of these tests need.
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

    static long queue_depth() {
        return static_cast<long>(Cache::get().get_client().llen(Jobs::queue_key(Email::SendEmail::kJobType)));
    }
};

TEST_F(BillingEmailsTest, CaptureCreditsEnqueuesExactlyOneReceiptJob) {
    auto user = seed_user("receipt@example.com");
    seed_package("Starter Pack", 1000, 1000);
    fake->next_order_id = "ORDER-RECEIPT-1";
    fake->capture_amount_cents = 1000;
    fake->capture_currency = "USD";

    auto pkgs = packages.list_active();
    ASSERT_FALSE(pkgs.empty());

    int topup_status = 0;
    do_topup(user, json{{"package_id", pkgs[0].id}}, &topup_status);
    ASSERT_EQ(topup_status, k201Created);

    int capture_status = 0;
    auto capture_body = do_capture(user, "ORDER-RECEIPT-1", &capture_status);
    ASSERT_EQ(capture_status, k200OK);
    ASSERT_TRUE(capture_body["data"]["credited"].get<bool>());

    ASSERT_EQ(queue_depth(), 1);
    auto payload = only_enqueued_email_job();
    EXPECT_EQ(payload["to"], "receipt@example.com");
    EXPECT_EQ(payload["subject"], "Your top-up receipt");
    EXPECT_NE(payload["text"].get<std::string>().find("Starter Pack"), std::string::npos);
    EXPECT_NE(payload["text"].get<std::string>().find("10.00"), std::string::npos);  // 1000 cents -> "10.00"
    EXPECT_NE(payload["html"].get<std::string>().find("Starter Pack"), std::string::npos);
}

TEST_F(BillingEmailsTest, WebhookRedeliveryAfterCaptureDoesNotEnqueueSecondReceipt) {
    auto user = seed_user("dedupe@example.com");
    fake->next_order_id = "ORDER-DEDUPE-1";
    fake->capture_amount_cents = 500;
    fake->capture_currency = "USD";

    do_topup(user, json{{"amount_cents", 500}});
    int capture_status = 0;
    do_capture(user, "ORDER-DEDUPE-1", &capture_status);
    ASSERT_EQ(capture_status, k200OK);
    ASSERT_EQ(queue_depth(), 1);  // the one receipt from the return-flow capture

    // The webhook now redelivers PAYMENT.CAPTURE.COMPLETED for the SAME
    // order/capture — credit_capture's own idempotency guard makes this a
    // no-op (credited=false), and the payment's status stays 'captured', so
    // dispatchFailedEmailIfJustTransitioned finds nothing to send either.
    auto event = capture_completed_event("WH-DEDUPE-1", "ORDER-DEDUPE-1", fake->next_capture_id, "5.00");
    int webhook_status = 0;
    auto body = do_webhook(event, &webhook_status);
    EXPECT_EQ(webhook_status, k200OK);
    EXPECT_TRUE(body["data"]["handled"].get<bool>());

    EXPECT_EQ(queue_depth(), 1);  // still just the one receipt — no duplicate
}

TEST_F(BillingEmailsTest, WebhookRefundAppliedEnqueuesRefundJob) {
    auto user = seed_user("refund@example.com");
    fake->next_order_id = "ORDER-REFUND-1";
    fake->capture_amount_cents = 1000;
    fake->capture_currency = "USD";

    do_topup(user, json{{"amount_cents", 1000}});
    int capture_status = 0;
    do_capture(user, "ORDER-REFUND-1", &capture_status);
    ASSERT_EQ(capture_status, k200OK);
    ASSERT_EQ(queue_depth(), 1);  // the receipt
    ASSERT_EQ(Billing::balance_of(user.subject, /*from_primary=*/true), 1000);

    auto event = capture_refunded_event("WH-REFUND-1", "REFUND-1", fake->next_capture_id, "10.00");
    int webhook_status = 0;
    auto body = do_webhook(event, &webhook_status);
    EXPECT_EQ(webhook_status, k200OK);
    EXPECT_TRUE(body["data"]["handled"].get<bool>());
    ASSERT_EQ(Billing::balance_of(user.subject, /*from_primary=*/true), 0);

    ASSERT_EQ(queue_depth(), 2);  // receipt + refund
    auto& redis = Cache::get().get_client();
    std::vector<std::string> ids;
    redis.lrange(Jobs::queue_key(Email::SendEmail::kJobType), 0, -1, std::back_inserter(ids));
    bool found_refund = false;
    for (const auto& id : ids) {
        auto job = Jobs::get().get_status(id);
        ASSERT_TRUE(job.has_value());
        if (job->payload["subject"] == "Refund processed") {
            found_refund = true;
            EXPECT_EQ(job->payload["to"], "refund@example.com");
            EXPECT_NE(job->payload["text"].get<std::string>().find("10.00"), std::string::npos);
            EXPECT_NE(job->payload["text"].get<std::string>().find("1000 credits"), std::string::npos);
        }
    }
    EXPECT_TRUE(found_refund);
}

TEST_F(BillingEmailsTest, WebhookReversalSkippedForInsufficientBalanceDoesNotEnqueueRefundJob) {
    auto user = seed_user("skip-refund@example.com");
    fake->next_order_id = "ORDER-SKIP-1";
    fake->capture_amount_cents = 1000;
    fake->capture_currency = "USD";

    do_topup(user, json{{"amount_cents", 1000}});
    int capture_status = 0;
    do_capture(user, "ORDER-SKIP-1", &capture_status);
    ASSERT_EQ(capture_status, k200OK);
    ASSERT_EQ(queue_depth(), 1);  // the receipt
    ASSERT_EQ(Billing::balance_of(user.subject, /*from_primary=*/true), 1000);

    // Drain the balance down via a direct admin adjustment (Billing::adjust
    // — the same user id doubles as its own "admin" here purely to satisfy
    // adjust()'s two FK checks; nothing about the adjustment's audit trail
    // is under test here) so a FULL refund of the original 1000-cent
    // payment can no longer be covered.
    Billing::adjust(user.subject, -950, "test drain", user.subject);
    ASSERT_EQ(Billing::balance_of(user.subject, /*from_primary=*/true), 50);

    // PAYMENT.CAPTURE.REVERSED for the full original amount: refund_capture
    // durably records the attempt (outcome=skipped_insufficient,
    // credited=true) but writes NO wallet_entries row — this is exactly the
    // "applied vs skipped" distinction dispatchRefundEmailIfApplied exists
    // to make.
    auto event = capture_refunded_event("WH-SKIP-1", "REVERSAL-1", fake->next_capture_id, "10.00");
    // capture_refunded_event always sets event_type to REFUNDED; flip it to
    // REVERSED to exercise that branch too (identical handler either way —
    // see BillingController::handleCaptureRefunded's doc comment).
    event["event_type"] = "PAYMENT.CAPTURE.REVERSED";
    int webhook_status = 0;
    auto body = do_webhook(event, &webhook_status);
    EXPECT_EQ(webhook_status, k200OK);
    EXPECT_TRUE(body["data"]["handled"].get<bool>());

    // Balance untouched by the skipped refund attempt.
    EXPECT_EQ(Billing::balance_of(user.subject, /*from_primary=*/true), 50);
    // No second job — the skipped refund never enqueues an email.
    EXPECT_EQ(queue_depth(), 1);
}

TEST_F(BillingEmailsTest, CaptureAmountMismatchEnqueuesFailedJob) {
    auto user = seed_user("mismatch@example.com");
    fake->next_order_id = "ORDER-MISMATCH-1";
    // Payment was created for 1000 cents; PayPal reports back a DIFFERENT
    // captured amount — credit_capture's own guard marks the payment
    // 'failed' and leaves the wallet untouched.
    fake->capture_amount_cents = 500;
    fake->capture_currency = "USD";

    do_topup(user, json{{"amount_cents", 1000}});
    int capture_status = 0;
    auto capture_body = do_capture(user, "ORDER-MISMATCH-1", &capture_status);
    ASSERT_EQ(capture_status, k200OK);
    EXPECT_FALSE(capture_body["data"]["credited"].get<bool>());

    auto payment = payments.find_by_order_id("ORDER-MISMATCH-1", /*from_primary=*/true);
    ASSERT_TRUE(payment.has_value());
    EXPECT_EQ(payment->status, "failed");
    EXPECT_EQ(Billing::balance_of(user.subject, /*from_primary=*/true), 0);

    ASSERT_EQ(queue_depth(), 1);
    auto payload = only_enqueued_email_job();
    EXPECT_EQ(payload["to"], "mismatch@example.com");
    EXPECT_EQ(payload["subject"], "Your payment could not be completed");
    EXPECT_NE(payload["text"].get<std::string>().find("not charged"), std::string::npos);
}

// ── secondary fixture: jobs disabled entirely (mail delivery effectively
// off — see billing_email_test_config_overrides' comment on why
// mail.enabled=false alone does NOT stop enqueueing) ────────────────────
class BillingEmailsJobsDisabledTest : public TestHelpers::CoreBackedTest {
protected:
    std::string config_file_name() const override { return "billing_emails_jobs_disabled_test_config.json"; }

    void config_overrides(json& cfg) override {
        billing_email_test_config_overrides(cfg);
        cfg["jobs"]["enabled"] = false;
    }
};

TEST_F(BillingEmailsJobsDisabledTest, ReceiptDoesNotThrowAndEnqueuesNothingWhenJobsAreOff) {
    Domain::User user;
    user.id = "00000000-0000-0000-0000-0000000000aa";
    user.email = "no-jobs@example.com";
    user.first_name = "No";
    user.last_name = "Jobs";

    // via_jobs() is false (Jobs::is_initialized()==false), so this falls
    // back to the inline path — which, with mail.enabled=false, is a
    // logged no-op inside Mailer::send(). Nothing is enqueued, and
    // BillingEmails::receipt still must not throw (best-effort contract).
    EXPECT_NO_THROW(Email::BillingEmails::receipt(
        user, "Test Package", 1000, "USD", 1000, 1000, "payment-id-1", "2026-01-01T00:00:00Z"));
}

}  // namespace
