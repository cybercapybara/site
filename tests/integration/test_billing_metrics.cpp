/**
 * @file test_billing_metrics.cpp
 * @brief Integration tests for GET /api/v1/admin/billing/metrics (Task 3,
 *        billing-emails-dashboard): module guard + admin gate, period
 *        validation, and the revenue/conversion/refund/outstanding math and
 *        gap-free series against a real Postgres schema.
 *
 * Seeding goes through the REAL money paths (PaymentRepository::create +
 * Billing::credit_capture / Billing::refund_capture), same as
 * test_admin_billing_api.cpp — only `created_at` is rewritten afterward
 * (directly, via SQL) so payments/refunds can be placed at controlled
 * offsets relative to `now()` without waiting real time. This keeps
 * wallet_balances/billing_refunds exactly as consistent as they'd be in
 * production, which matters here because outstanding_credits/
 * outstanding_value_cents read wallet_balances directly.
 */

#include <optional>
#include <string>

#include <drogon/HttpRequest.h>
#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "api/AdminBillingController.hpp"
#include "billing/Wallet.hpp"
#include "database/Database.hpp"
#include "domain/Billing.hpp"
#include "repositories/BillingRepository.hpp"
#include "repositories/RoleRepository.hpp"
#include "repositories/UserRepository.hpp"
#include "security/Auth.hpp"
#include "test_helpers.hpp"

using namespace drogon;
using json = nlohmann::json;

namespace {

class BillingMetricsApiTest : public TestHelpers::CoreBackedTest {
protected:
    Api::AdminBillingController controller;
    Repositories::RoleRepository roles;
    Repositories::UserRepository users;
    Repositories::PaymentRepository payments;
    Repositories::PackageRepository packages;
    Repositories::BillingSettingsRepository settings;

    std::string config_file_name() const override { return "billing_metrics_api_test_config.json"; }

    void config_overrides(json& cfg) override {
        // Admin gate is only real when auth is on — see AdminBillingApiTest's
        // config_overrides for why this isn't optional.
        cfg["auth"]["mode"] = "jwt";
        cfg["auth"]["jwt"]["secret"] = "test-jwt-secret-for-billing-metrics-api-flow-pad";
        cfg["database"]["migrations_enabled"] = true;
        cfg["database"]["migrations_dir"] = "migrations";
        cfg["billing"]["enabled"] = true;
        cfg["billing"]["provider"] = "paypal";
        cfg["billing"]["currency"] = "USD";
        // Matches migration 009's seeded defaults — rate 100 credits per 100
        // cents, i.e. 1 credit per 1 cent, which is what every amount/credits
        // pair below relies on (credits_expected == amount_cents).
        cfg["billing"]["credits_per_unit"] = 100;
        cfg["billing"]["min_amount_cents"] = 100;
        cfg["billing"]["max_amount_cents"] = 100000;
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
            // billing_settings is never truncated (single seeded row) — reset to
            // the known default so a leaked rate from another suite can't skew
            // outstanding_value_cents here.
            txn.exec(
                "UPDATE billing_settings SET credits_per_unit = 100, min_amount_cents = 100, "
                "max_amount_cents = 100000 WHERE id = 1");
            return 0;
        });
    }

    void TearDown() override {
        if (!::testing::Test::IsSkipped()) {
            Database::get().execute_write([](auto& txn) {
                txn.exec(
                    "UPDATE billing_settings SET credits_per_unit = 100, min_amount_cents = 100, "
                    "max_amount_cents = 100000 WHERE id = 1");
                return 0;
            });
        }
        TestHelpers::CoreBackedTest::TearDown();
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

    Domain::Package seed_package(const std::string& title) {
        return packages.create(title, /*amount_cents=*/1, /*credits=*/1, /*active=*/true, /*sort=*/0);
    }

    /// The capture id seed_captured() always uses for a given order id — a
    /// fixed, deterministic mapping so seed_refund() can address the right
    /// payment without a second DB round trip.
    static std::string capture_id_for(const std::string& order_id) { return order_id + "-CAP"; }

    /// Rewrite created_at directly. @p offset is a Postgres interval literal
    /// ("2 hours", "5 days", ...). Called AFTER the real write path below, so
    /// only the timestamp is synthetic — every money invariant (wallet
    /// crediting, idempotency markers) was exercised for real.
    static void backdate_payment(const std::string& payment_id, const std::string& offset) {
        Database::get().execute_write([&](auto& txn) {
            txn.exec_params("UPDATE payments SET created_at = now() - $1::interval WHERE id = $2", offset, payment_id);
            return 0;
        });
    }

    static void backdate_refund(const std::string& provider_refund_id, const std::string& offset) {
        Database::get().execute_write([&](auto& txn) {
            txn.exec_params(
                "UPDATE billing_refunds SET created_at = now() - $1::interval WHERE provider_refund_id = $2",
                offset,
                provider_refund_id);
            return 0;
        });
    }

    /// Create a payment and drive it through Billing::credit_capture for
    /// real (so wallet_entries/wallet_balances end up exactly as a real
    /// top-up would leave them), then backdate created_at. @p credits_expected
    /// defaults to a 1:1 ratio with @p amount_cents (matching the fixture's
    /// credits_per_unit=100 rate); pass it explicitly to force a different
    /// ratio — e.g. Wallet::refund_capture's `skipped_zero_credits` branch
    /// needs a payment whose credits_expected/amount_cents ratio is small
    /// enough that a legal (>0) refund amount floors to 0 credits.
    Domain::Payment seed_captured(const std::string& user_id,
                                  const std::string& order_id,
                                  std::int64_t amount_cents,
                                  const std::string& created_offset,
                                  std::optional<std::string> package_id = std::nullopt,
                                  std::optional<std::int64_t> credits_expected = std::nullopt) {
        const std::int64_t credits = credits_expected.value_or(amount_cents);
        auto p = payments.create(user_id, order_id, amount_cents, "USD", credits, /*rate_snapshot=*/100, package_id);
        auto result = Billing::credit_capture(order_id, capture_id_for(order_id), amount_cents, "USD");
        EXPECT_TRUE(result.credited);
        backdate_payment(p.id, created_offset);
        auto reloaded = payments.find(p.id);
        EXPECT_TRUE(reloaded.has_value());
        return *reloaded;
    }

    /// A payment that never got past `created` — no wallet effect at all.
    Domain::Payment seed_created_only(const std::string& user_id,
                                      const std::string& order_id,
                                      std::int64_t amount_cents,
                                      const std::string& created_offset) {
        auto p = payments.create(
            user_id, order_id, amount_cents, "USD", /*credits_expected=*/amount_cents, /*rate_snapshot=*/100, std::nullopt);
        backdate_payment(p.id, created_offset);
        return p;
    }

    /// A payment forced straight to `failed` — never captured, no wallet effect.
    Domain::Payment seed_failed(const std::string& user_id,
                                const std::string& order_id,
                                std::int64_t amount_cents,
                                const std::string& created_offset) {
        auto p = payments.create(
            user_id, order_id, amount_cents, "USD", /*credits_expected=*/amount_cents, /*rate_snapshot=*/100, std::nullopt);
        Database::get().execute_write([&](auto& txn) {
            txn.exec_params("UPDATE payments SET status = 'failed' WHERE id = $1", p.id);
            return 0;
        });
        backdate_payment(p.id, created_offset);
        return p;
    }

    /// Refund an already-captured payment for real (drives Billing::refund_capture,
    /// which debits wallet_balances) then backdate the billing_refunds row's
    /// created_at. The caller is responsible for leaving the target user's
    /// balance sufficient to cover it.
    void seed_refund(const std::string& order_id,
                     const std::string& refund_id,
                     std::int64_t refunded_amount_cents,
                     const std::string& created_offset) {
        auto result = Billing::refund_capture(capture_id_for(order_id), refund_id, refunded_amount_cents);
        EXPECT_TRUE(result.credited);
        backdate_refund(refund_id, created_offset);
    }

    /// `billing_refunds.outcome` for a given provider_refund_id — used to
    /// confirm a seeding helper actually produced the outcome a test assumes
    /// (e.g. `skipped_zero_credits`), rather than just trusting the arithmetic.
    static std::string refund_outcome(const std::string& refund_id) {
        return Database::get().execute_read([&](auto& txn) {
            auto r = txn.exec_params("SELECT outcome FROM billing_refunds WHERE provider_refund_id = $1", refund_id);
            EXPECT_EQ(r.size(), 1u);
            return r.empty() ? std::string{} : r[0]["outcome"].template as<std::string>();
        });
    }

    /// Drive the controller with ?period=@p period (omit the parameter
    /// entirely when @p period is empty, to exercise the default).
    json call_metrics(const Security::Auth::AuthPrincipal& principal, const std::string& period, int* status_out) {
        auto req = TestHelpers::authed(principal);
        if (!period.empty())
            req->setParameter("period", period);
        HttpResponsePtr resp;
        controller.metrics(req, [&](const HttpResponsePtr& r) { resp = r; });
        *status_out = static_cast<int>(resp->statusCode());
        return resp->body().empty() ? json::object() : json::parse(std::string(resp->body()));
    }
};

// ── admin gate + module guard ───────────────────────────────────────────────

TEST_F(BillingMetricsApiTest, NonAdminGets403) {
    auto user = seed_user("plain@example.com");
    int status = 0;
    call_metrics(user, "", &status);
    EXPECT_EQ(status, k403Forbidden);
}

// ── period validation ────────────────────────────────────────────────────────

TEST_F(BillingMetricsApiTest, RejectsUnknownPeriod) {
    auto admin = seed_admin();
    int status = 0;
    auto body = call_metrics(admin, "fortnight", &status);
    EXPECT_EQ(status, k400BadRequest);
    EXPECT_EQ(body["error"], "invalid_period");
}

TEST_F(BillingMetricsApiTest, DefaultsToWeekWhenPeriodOmitted) {
    auto admin = seed_admin();
    int status = 0;
    auto body = call_metrics(admin, "", &status);
    ASSERT_EQ(status, k200OK);
    EXPECT_EQ(body["data"]["period"], "week");
    EXPECT_EQ(body["data"]["series"].size(), 7u);
}

// ── bucket counts per period (no payments needed — every bucket is zero) ────

TEST_F(BillingMetricsApiTest, SeriesBucketCountMatchesPeriod) {
    auto admin = seed_admin();
    int status = 0;

    auto day_body = call_metrics(admin, "day", &status);
    ASSERT_EQ(status, k200OK);
    EXPECT_EQ(day_body["data"]["series"].size(), 24u);

    auto week_body = call_metrics(admin, "week", &status);
    ASSERT_EQ(status, k200OK);
    EXPECT_EQ(week_body["data"]["series"].size(), 7u);

    auto month_body = call_metrics(admin, "month", &status);
    ASSERT_EQ(status, k200OK);
    EXPECT_EQ(month_body["data"]["series"].size(), 30u);
}

// ── series has no gaps ───────────────────────────────────────────────────────

TEST_F(BillingMetricsApiTest, DaySeriesIsGapFreeWithZeroBucketsRendered) {
    auto admin = seed_admin();
    auto alice = seed_user("alice-series@example.com");

    // Both offsets are comfortably inside the day window's actual span (see
    // BillingMetricsRepository.hpp's file comment: the calendar-aligned
    // bucket window can start up to ~1 hour later than a plain
    // now()-24h, so anything <= 20h old is unambiguously inside it) and land
    // on distinct hourly buckets.
    auto p1 = seed_captured(alice.subject, "ORDER-SERIES-1", 300, "2 hours");
    auto p2 = seed_captured(alice.subject, "ORDER-SERIES-2", 700, "10 hours");

    int status = 0;
    auto body = call_metrics(admin, "day", &status);
    ASSERT_EQ(status, k200OK);

    const auto& series = body["data"]["series"];
    ASSERT_EQ(series.size(), 24u);

    int zero_buckets = 0, nonzero_buckets = 0;
    std::int64_t total_revenue = 0, total_count = 0;
    for (const auto& pt : series) {
        ASSERT_TRUE(pt.contains("bucket_start"));
        const auto revenue = pt["revenue_cents"].get<std::int64_t>();
        const auto count = pt["payments_count"].get<std::int64_t>();
        if (revenue == 0 && count == 0)
            ++zero_buckets;
        else
            ++nonzero_buckets;
        total_revenue += revenue;
        total_count += count;
    }

    // "No gaps" means every bucket in range is PRESENT (24 of them), not that
    // every bucket has data — most of these 24 hours had no captured
    // payment, and those buckets must still show up as explicit zeros.
    EXPECT_GT(zero_buckets, 0);
    EXPECT_EQ(nonzero_buckets, 2);
    EXPECT_EQ(total_revenue, p1.amount_cents + p2.amount_cents);
    EXPECT_EQ(total_count, 2);
}

// ── the full aggregate math ──────────────────────────────────────────────────

TEST_F(BillingMetricsApiTest, WeekMetricsMathIsCorrect) {
    auto admin = seed_admin();
    auto alice = seed_user("alice@example.com");
    auto bob = seed_user("bob@example.com");

    auto starter = seed_package("Starter");
    auto mini = seed_package("Mini");

    // ── in-window (last 7 days) ────────────────────────────────────────────
    // Captured, contributes to revenue/count/avg/conversion/top lists.
    auto p1 = seed_captured(alice.subject, "ORDER-W1", 1000, "1 days", mini.id);     // alice, Mini
    auto p2 = seed_captured(alice.subject, "ORDER-W2", 2000, "3 days", starter.id);  // alice, Starter
    auto p3 = seed_captured(bob.subject, "ORDER-W3", 1500, "5 days", starter.id);    // bob, Starter

    // created (never captured) and failed — count toward `conversion.created`
    // only, never toward revenue/count or top lists.
    seed_created_only(bob.subject, "ORDER-W4", 800, "2 days");
    seed_failed(alice.subject, "ORDER-W5", 300, "4 days");

    // Partial refund of p2 (500 of its 2000 cents) — payment stays 'captured'
    // (cumulative refunds < amount_cents), so it still counts toward revenue;
    // the refund itself is counted separately.
    seed_refund("ORDER-W2", "REFUND-W2-PARTIAL", 500, "3 days");

    // ── outside the 7-day window ───────────────────────────────────────────
    // Captured 10 days ago: excluded from revenue/count/conversion/top lists/
    // refunds-in-window, but its wallet credit still counts toward the
    // all-time outstanding liability.
    auto p6 = seed_captured(alice.subject, "ORDER-W6", 5000, "10 days");
    // Refund of p6, also 10 days ago -> excluded from the week's refunds sum.
    seed_refund("ORDER-W6", "REFUND-W6-OLD", 1000, "10 days");

    int status = 0;
    auto body = call_metrics(admin, "week", &status);
    ASSERT_EQ(status, k200OK);
    const auto& data = body["data"];

    EXPECT_EQ(data["period"], "week");

    // revenue/count/avg: p1 + p2 + p3 only (p6 is out of window; p4 was never
    // captured; p5 failed). amount_cents is unaffected by the partial refund.
    const std::int64_t expected_revenue = p1.amount_cents + p2.amount_cents + p3.amount_cents;
    EXPECT_EQ(data["revenue_cents"], expected_revenue);
    EXPECT_EQ(data["payments_count"], 3);
    EXPECT_EQ(data["avg_payment_cents"], expected_revenue / 3);

    // conversion: 5 payments created in-window (p1..p5), 3 captured.
    EXPECT_EQ(data["conversion"]["created"], 5);
    EXPECT_EQ(data["conversion"]["captured"], 3);
    EXPECT_DOUBLE_EQ(data["conversion"]["rate"].get<double>(), 3.0 / 5.0);

    // refunds: only the in-window partial refund of p2 (500 cents); the
    // 10-day-old refund of p6 is excluded.
    EXPECT_EQ(data["refunds_cents"], 500);
    EXPECT_EQ(data["refunds_count"], 1);

    // outstanding: all-time, not windowed.
    //   alice: +1000 (p1) +2000 (p2) +5000 (p6) -500 (refund p2) -1000 (refund p6) = 6500
    //   bob:   +1500 (p3)                                                          = 1500
    //   total: 8000 credits, credits_per_unit=100 -> value_cents = 8000*100/100 = 8000
    EXPECT_EQ(data["outstanding_credits"], 8000);
    EXPECT_EQ(data["outstanding_value_cents"], 8000);
    EXPECT_EQ(Billing::balance_of(alice.subject, /*from_primary=*/true), 6500);
    EXPECT_EQ(Billing::balance_of(bob.subject, /*from_primary=*/true), 1500);

    // top_packages: Starter (p2 2000 + p3 1500 = 3500 cents, 2 payments)
    // ahead of Mini (p1 1000 cents, 1 payment) — ordered by revenue desc.
    const auto& top_packages = data["top_packages"];
    ASSERT_EQ(top_packages.size(), 2u);
    EXPECT_EQ(top_packages[0]["package_id"], starter.id);
    EXPECT_EQ(top_packages[0]["title"], "Starter");
    EXPECT_EQ(top_packages[0]["revenue_cents"], 3500);
    EXPECT_EQ(top_packages[0]["payments_count"], 2);
    EXPECT_EQ(top_packages[1]["package_id"], mini.id);
    EXPECT_EQ(top_packages[1]["title"], "Mini");
    EXPECT_EQ(top_packages[1]["revenue_cents"], 1000);
    EXPECT_EQ(top_packages[1]["payments_count"], 1);

    // top_users: alice's in-window top-up credits (p1 1000 + p2 2000 = 3000)
    // ahead of bob's (p3 1500 credits) — ordered by topup_credits desc.
    const auto& top_users = data["top_users"];
    ASSERT_EQ(top_users.size(), 2u);
    EXPECT_EQ(top_users[0]["user_id"], alice.subject);
    EXPECT_EQ(top_users[0]["email"], "alice@example.com");
    EXPECT_EQ(top_users[0]["topup_credits"], 3000);
    EXPECT_EQ(top_users[0]["revenue_cents"], 3000);
    EXPECT_EQ(top_users[1]["user_id"], bob.subject);
    EXPECT_EQ(top_users[1]["email"], "bob@example.com");
    EXPECT_EQ(top_users[1]["topup_credits"], 1500);
    EXPECT_EQ(top_users[1]["revenue_cents"], 1500);
}

TEST_F(BillingMetricsApiTest, ZeroPaymentsInWindowGuardsDivisionByZero) {
    auto admin = seed_admin();
    int status = 0;
    auto body = call_metrics(admin, "week", &status);
    ASSERT_EQ(status, k200OK);
    const auto& data = body["data"];
    EXPECT_EQ(data["revenue_cents"], 0);
    EXPECT_EQ(data["payments_count"], 0);
    EXPECT_EQ(data["avg_payment_cents"], 0);  // not a NaN/inf from a 0/0 division
    EXPECT_EQ(data["conversion"]["created"], 0);
    EXPECT_EQ(data["conversion"]["captured"], 0);
    EXPECT_DOUBLE_EQ(data["conversion"]["rate"].get<double>(), 0.0);
    EXPECT_EQ(data["refunds_cents"], 0);
    EXPECT_EQ(data["refunds_count"], 0);
    EXPECT_EQ(data["outstanding_credits"], 0);
    EXPECT_EQ(data["outstanding_value_cents"], 0);
    EXPECT_TRUE(data["top_packages"].empty());
    EXPECT_TRUE(data["top_users"].empty());
}

// Regression guard for the `outcome = 'applied'` filter in
// BillingMetricsRepository's refunds query: every OTHER test here only ever
// seeds applied refunds, so a filter wrongly widened to include skipped rows
// would pass every one of them silently. This proves the filter actually
// discriminates by outcome, not just that the number happens to be zero.
TEST_F(BillingMetricsApiTest, RefundsExcludeSkippedOutcomesButIncludeApplied) {
    auto admin = seed_admin();
    auto alice = seed_user("alice-refunds@example.com");

    // Applied: an ordinary 1:1 payment (credits_expected == amount_cents),
    // refunded in full -> Wallet::refund_capture's outcome='applied' branch.
    seed_captured(alice.subject, "ORDER-REFUND-APPLIED", 1000, "1 days");
    seed_refund("ORDER-REFUND-APPLIED", "REFUND-APPLIED", 1000, "1 days");

    // Skipped (skipped_zero_credits): credits_expected is deliberately tiny
    // relative to amount_cents (1 credit for a 1000-cent payment), so
    // Wallet::refund_capture's `credits_expected * refunded_amount_cents /
    // amount_cents` conversion floors to 0 for ANY refund amount below the
    // full 1000 cents — see that function's doc comment on the
    // skipped_zero_credits branch. A 1-cent refund (the smallest legal
    // amount; refund_capture requires > 0) floors (1*1)/1000 == 0, forcing
    // this branch deterministically without needing to first spend down a
    // balance (the skipped_insufficient alternative would).
    seed_captured(
        alice.subject, "ORDER-REFUND-SKIPPED", 1000, "1 days", /*package_id=*/std::nullopt, /*credits_expected=*/1);
    seed_refund("ORDER-REFUND-SKIPPED", "REFUND-SKIPPED-ZERO", 1, "1 days");

    // Confirm the seeding actually produced the outcomes this test assumes,
    // not just that the metrics endpoint happens to report the right numbers.
    ASSERT_EQ(refund_outcome("REFUND-APPLIED"), "applied");
    ASSERT_EQ(refund_outcome("REFUND-SKIPPED-ZERO"), "skipped_zero_credits");

    int status = 0;
    auto body = call_metrics(admin, "week", &status);
    ASSERT_EQ(status, k200OK);
    const auto& data = body["data"];

    // Only the applied refund may count — the skipped one must move neither
    // figure, proving the filter discriminates rather than summing everything.
    EXPECT_EQ(data["refunds_cents"], 1000);
    EXPECT_EQ(data["refunds_count"], 1);
}

// ── module gate ──────────────────────────────────────────────────────────────

class BillingMetricsDisabledApiTest : public TestHelpers::CoreBackedTest {
protected:
    Api::AdminBillingController controller;
    Repositories::RoleRepository roles;
    Repositories::UserRepository users;

    std::string config_file_name() const override { return "billing_metrics_disabled_api_test_config.json"; }

    void config_overrides(json& cfg) override {
        cfg["auth"]["mode"] = "jwt";
        cfg["auth"]["jwt"]["secret"] = "test-jwt-secret-for-billing-metrics-disabled-flow-pad";
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

    Security::Auth::AuthPrincipal seed_admin(const std::string& email = "admin@example.com") {
        auto role = roles.find_by_name("Administrator");
        EXPECT_TRUE(role.has_value());
        auto u = users.create(email, std::string("$argon2id$placeholder"), std::nullopt, std::nullopt, role->id, true);
        Security::Auth::AuthPrincipal p;
        p.subject = u.id;
        if (role)
            p.roles.push_back(role->name);
        p.raw_claims = json{{"sub", u.id}, {"permissions", role ? role->permissions : 0u}};
        return p;
    }
};

TEST_F(BillingMetricsDisabledApiTest, MetricsIs404WhenBillingDisabled) {
    // Module guard runs BEFORE the admin gate (same order as every other
    // handler in this controller) — an admin principal still gets 404, not
    // 403, proving the guard, not the gate, is what fired.
    auto admin = seed_admin();
    HttpResponsePtr resp;
    controller.metrics(TestHelpers::authed(admin), [&](const HttpResponsePtr& r) { resp = r; });
    EXPECT_EQ(resp->statusCode(), k404NotFound);
}

}  // namespace
