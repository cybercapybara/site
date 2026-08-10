/**
 * @file BillingMetricsRepository.hpp
 * @brief Aggregate queries backing GET /api/v1/admin/billing/metrics (Task 3,
 *        billing-emails-dashboard). One parameterized query per aggregate
 *        block, run inside a single read transaction so the whole response
 *        is a consistent snapshot.
 *
 * Window semantics (see AdminBillingController::metrics for the period ->
 * MetricsWindow mapping):
 *   - revenue/count/avg, conversion, and refunds are all windowed as
 *     `created_at >= now() - interval '<N>'` — a ROLLING window, not
 *     calendar-aligned.
 *   - the `series` block is calendar/bucket-aligned instead (it has to be,
 *     to produce fixed-width buckets with no gaps): its span is
 *     `[date_trunc(bucket, now()) - (bucket_count-1)*step, date_trunc(bucket, now())]`,
 *     which is very close to but not bit-identical to the rolling window
 *     above (it can extend up to one bucket-width earlier). Both are correct
 *     readings of "the last day/week/month"; they just don't sum to bit-for-
 *     bit the same total, same as any dashboard mixing a rolling KPI with a
 *     calendar-bucketed chart.
 *   - `outstanding_credits` / `outstanding_value_cents` are NOT windowed at
 *     all — `wallet_balances` is a point-in-time liability snapshot, not an
 *     event stream, so "outstanding as of now" is the only meaningful
 *     reading.
 *
 * Money math: `outstanding_value_cents = outstanding_credits * 100 /
 * credits_per_unit` (integer division) — the exact inverse of how
 * BillingController::resolve_topup_plan computes `credits_expected =
 * amount_cents * credits_per_unit / 100` for a custom top-up (see that
 * file's doc comment / migration 009's column comment: credits_per_unit is
 * "credits granted per 100 cents"). The caller passes the CURRENT rate
 * (Repositories::BillingSettingsRepository::get().credits_per_unit) in —
 * this repository does not read billing_settings itself, so there is
 * exactly one place (AdminBillingController) that decides which rate is
 * "current".
 */

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <pqxx/pqxx>

#include "database/Database.hpp"
#include "utils/Time.hpp"

namespace Repositories {

/**
 * @brief `period` (day/week/month, validated by the controller) resolved
 *        into the SQL knobs the repository needs: the rolling window for
 *        the scalar aggregates, and the bucket shape for `series`.
 */
struct MetricsWindow {
    std::string period;            // "day" | "week" | "month" — echoed back in the response
    std::string rolling_interval;  // "24 hours" | "7 days" | "30 days" — the scalar-aggregate window
    std::string bucket_field;      // "hour" | "day" — date_trunc() field for `series`
    std::string bucket_step;       // "1 hour" | "1 day" — generate_series() step
    std::string bucket_span;       // (bucket_count - 1) * bucket_step, precomputed as its own interval
                                    // literal so the SQL never has to multiply an interval by an
                                    // integer parameter (Postgres's operator resolution for that combination
                                    // is not guaranteed to be unambiguous across versions — a literal offset
                                    // sidesteps the question entirely).
    int bucket_count;              // 24 | 7 | 30 — exact row count `series` will have

    static MetricsWindow for_period(const std::string& period) {
        if (period == "day")
            return {"day", "24 hours", "hour", "1 hour", "23 hours", 24};
        if (period == "month")
            return {"month", "30 days", "day", "1 day", "29 days", 30};
        // "week" is the default the controller falls back to — also the
        // fallback here so an internal misuse degrades to something sane
        // instead of an unrelated period silently reusing "day"'s shape.
        return {"week", "7 days", "day", "1 day", "6 days", 7};
    }
};

struct MetricsSeriesPoint {
    std::string bucket_start;  // ISO-8601, UTC
    std::int64_t revenue_cents{0};
    std::int64_t payments_count{0};
};

struct MetricsTopPackage {
    std::string package_id;
    std::string title;
    std::int64_t revenue_cents{0};
    std::int64_t payments_count{0};
};

struct MetricsTopUser {
    std::string user_id;
    std::string email;
    std::int64_t topup_credits{0};
    std::int64_t revenue_cents{0};
};

struct BillingMetrics {
    std::int64_t revenue_cents{0};
    std::int64_t payments_count{0};
    std::int64_t avg_payment_cents{0};

    std::int64_t conversion_created{0};
    std::int64_t conversion_captured{0};
    double conversion_rate{0.0};

    std::int64_t refunds_cents{0};
    std::int64_t refunds_count{0};

    std::int64_t outstanding_credits{0};
    std::int64_t outstanding_value_cents{0};

    std::vector<MetricsSeriesPoint> series;
    std::vector<MetricsTopPackage> top_packages;
    std::vector<MetricsTopUser> top_users;
};

class BillingMetricsRepository {
public:
    /**
     * @brief Run every aggregate block in one read transaction.
     * @param w calendar/window shape for @p w.period (MetricsWindow::for_period).
     * @param credits_per_unit the CURRENT rate (credits per 100 cents) from
     *        `billing_settings`, supplied by the caller — see file doc comment.
     */
    BillingMetrics get(const MetricsWindow& w, std::int64_t credits_per_unit) {
        return Database::get().execute_read([&](auto& txn) -> BillingMetrics {
            BillingMetrics m;

            // ── revenue / count / avg (captured only, rolling window) ──────────
            auto rev = txn.exec_params(
                "SELECT COALESCE(SUM(amount_cents), 0) AS revenue, COUNT(*) AS cnt "
                "FROM payments "
                "WHERE status = 'captured' AND created_at >= now() - $1::interval",
                w.rolling_interval);
            m.revenue_cents = rev[0]["revenue"].template as<std::int64_t>();
            m.payments_count = rev[0]["cnt"].template as<std::int64_t>();
            m.avg_payment_cents = m.payments_count > 0 ? m.revenue_cents / m.payments_count : 0;

            // ── conversion: captured vs every payment created in-window ────────
            auto conv = txn.exec_params(
                "SELECT COUNT(*) AS total, COUNT(*) FILTER (WHERE status = 'captured') AS captured "
                "FROM payments "
                "WHERE created_at >= now() - $1::interval",
                w.rolling_interval);
            m.conversion_created = conv[0]["total"].template as<std::int64_t>();
            m.conversion_captured = conv[0]["captured"].template as<std::int64_t>();
            m.conversion_rate = m.conversion_created > 0
                                    ? static_cast<double>(m.conversion_captured) / static_cast<double>(m.conversion_created)
                                    : 0.0;

            // ── refunds: only rows that actually debited the wallet ────────────
            // (outcome='applied' — 'skipped_insufficient'/'skipped_zero_credits'
            // rows are durable attempt markers, not money that actually moved;
            // see migrations/008_billing_refunds.sql and Wallet::refund_capture).
            auto refunds = txn.exec_params(
                "SELECT COALESCE(SUM(amount_cents), 0) AS total, COUNT(*) AS cnt "
                "FROM billing_refunds "
                "WHERE outcome = 'applied' AND created_at >= now() - $1::interval",
                w.rolling_interval);
            m.refunds_cents = refunds[0]["total"].template as<std::int64_t>();
            m.refunds_count = refunds[0]["cnt"].template as<std::int64_t>();

            // ── outstanding liability: all-time, not windowed ──────────────────
            auto outstanding = txn.exec("SELECT COALESCE(SUM(credits), 0) AS total FROM wallet_balances");
            m.outstanding_credits = outstanding[0]["total"].template as<std::int64_t>();
            // Integer math, exact inverse of credits = amount_cents * credits_per_unit / 100
            // (BillingController::resolve_topup_plan) — see file doc comment.
            // credits_per_unit > 0 is a DB CHECK constraint (migration 009), but
            // guard anyway rather than trust that invariant across a boundary.
            m.outstanding_value_cents =
                credits_per_unit > 0 ? (m.outstanding_credits * 100) / credits_per_unit : 0;

            // ── series: calendar-bucketed, LEFT JOINed so empty buckets are 0 ──
            auto series_rows = txn.exec_params(
                "SELECT gs.bucket_start AS bucket_start, "
                "       COALESCE(SUM(p.amount_cents), 0) AS revenue, "
                "       COUNT(p.id) AS cnt "
                "FROM generate_series("
                "       date_trunc($1::text, now()) - $2::interval, "
                "       date_trunc($1::text, now()), "
                "       $3::interval"
                "     ) AS gs(bucket_start) "
                "LEFT JOIN payments p "
                "       ON date_trunc($1::text, p.created_at) = gs.bucket_start "
                "      AND p.status = 'captured' "
                "GROUP BY gs.bucket_start "
                "ORDER BY gs.bucket_start",
                w.bucket_field,
                w.bucket_span,
                w.bucket_step);
            m.series.reserve(series_rows.size());
            for (const auto& row : series_rows) {
                MetricsSeriesPoint pt;
                pt.bucket_start = Utils::Time::pg_to_iso8601(row["bucket_start"].template as<std::string>());
                pt.revenue_cents = row["revenue"].template as<std::int64_t>();
                pt.payments_count = row["cnt"].template as<std::int64_t>();
                m.series.push_back(std::move(pt));
            }

            // ── top packages: captured payments in-window, by revenue ──────────
            auto pkg_rows = txn.exec_params(
                "SELECT bp.id AS package_id, bp.title AS title, "
                "       SUM(p.amount_cents) AS revenue, COUNT(*) AS cnt "
                "FROM payments p "
                "JOIN billing_packages bp ON bp.id = p.package_id "
                "WHERE p.status = 'captured' AND p.created_at >= now() - $1::interval "
                "GROUP BY bp.id, bp.title "
                "ORDER BY revenue DESC "
                "LIMIT 5",
                w.rolling_interval);
            m.top_packages.reserve(pkg_rows.size());
            for (const auto& row : pkg_rows) {
                MetricsTopPackage tp;
                tp.package_id = row["package_id"].template as<std::string>();
                tp.title = row["title"].template as<std::string>();
                tp.revenue_cents = row["revenue"].template as<std::int64_t>();
                tp.payments_count = row["cnt"].template as<std::int64_t>();
                m.top_packages.push_back(std::move(tp));
            }

            // ── top users: captured payments in-window, by top-up credits ──────
            auto user_rows = txn.exec_params(
                "SELECT p.user_id AS user_id, u.email AS email, "
                "       SUM(p.credits_expected) AS credits, SUM(p.amount_cents) AS revenue "
                "FROM payments p "
                "JOIN users u ON u.id = p.user_id "
                "WHERE p.status = 'captured' AND p.created_at >= now() - $1::interval "
                "GROUP BY p.user_id, u.email "
                "ORDER BY credits DESC "
                "LIMIT 5",
                w.rolling_interval);
            m.top_users.reserve(user_rows.size());
            for (const auto& row : user_rows) {
                MetricsTopUser tu;
                tu.user_id = row["user_id"].template as<std::string>();
                tu.email = row["email"].template as<std::string>();
                tu.topup_credits = row["credits"].template as<std::int64_t>();
                tu.revenue_cents = row["revenue"].template as<std::int64_t>();
                m.top_users.push_back(std::move(tu));
            }

            return m;
        });
    }
};

}  // namespace Repositories
