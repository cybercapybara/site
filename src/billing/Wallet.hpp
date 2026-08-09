/**
 * @file Wallet.hpp
 * @brief The credit wallet's crediting/refund/adjustment service — the ONLY
 *        code in this codebase allowed to write `wallet_entries` or
 *        `wallet_balances`. Everything else (the top-up endpoint, the PayPal
 *        webhook, the admin adjust endpoint — Tasks 4-6) calls into these
 *        four functions instead of touching the ledger tables directly.
 *
 * Money invariants enforced here (see migrations/007_billing.sql and
 * docs/superpowers/specs/2026-08-09-billing-paypal-design.md):
 *   - every amount is a BIGINT (cents / credits) — no floating point;
 *   - `wallet_entries` is append-only: this file only ever INSERTs into it;
 *   - `wallet_balances` is a cache kept in the SAME transaction as the
 *     ledger insert that changed it;
 *   - `credit_capture` moves `payments.credits_expected` — frozen at order
 *     creation — never a value re-derived from the current rate;
 *   - `credit_capture` is idempotent on `provider_capture_id`: the guarded
 *     UPDATE (`... WHERE provider_capture_id IS NULL`) is the structural
 *     race guard, backed by the UNIQUE index in the migration.
 */

#pragma once

#include <cstdint>
#include <optional>
#include <pqxx/pqxx>
#include <string>
#include <vector>

#include <spdlog/spdlog.h>

#include "database/Database.hpp"
#include "domain/Billing.hpp"
#include "repositories/BillingRepository.hpp"  // Repositories::PaymentNotFound
#include "repositories/RepoErrors.hpp"
#include "repositories/SqlErrors.hpp"  // Repositories::detail::translate_sql

namespace Billing {

/**
 * @brief → 409. A manual `adjust()` whose negative delta would drive the
 *        wallet below zero. Distinct from a refund going negative: an admin
 *        adjustment hasn't already moved real money, so it is refused up
 *        front instead of applied-with-a-warning.
 */
struct InsufficientBalance : Repositories::ConflictError {
    InsufficientBalance()
        : Repositories::ConflictError("insufficient_balance", "This adjustment would drive the wallet balance negative") {}
};

/**
 * @brief → 409. `provider_capture_id` is globally UNIQUE across `payments`
 *        (the structural double-credit guard). This fires only if a capture
 *        id that already belongs to a DIFFERENT order is presented for this
 *        one — a same-order retry never reaches this path, it lands in the
 *        ordinary "already captured" idempotent branch instead. Pathological
 *        (PayPal capture ids are unique by construction), but money code
 *        must not let a constraint violation surface as a bare 500.
 */
struct DuplicateCaptureId : Repositories::ConflictError {
    DuplicateCaptureId()
        : Repositories::ConflictError("capture_id_conflict", "This capture id is already recorded against a different payment") {}
};

struct CreditResult {
    bool credited;
    std::int64_t balance;
    std::string payment_id;
};

namespace detail {

/// Current cached balance for @p user_id, read through the caller's own
/// transaction (never opens a second connection) — 0 if the user has never
/// had a wallet_balances row written.
template <typename Txn>
std::int64_t read_balance(Txn& txn, const std::string& user_id) {
    auto r = txn.exec_params("SELECT credits FROM wallet_balances WHERE user_id = $1", user_id);
    if (r.empty())
        return 0;
    return r[0]["credits"].template as<std::int64_t>();
}

}  // namespace detail

/**
 * @brief Read the cached balance outside of any in-flight transaction. 0 if
 *        the user has no wallet_balances row yet (never topped up).
 */
inline std::int64_t balance_of(const std::string& user_id) {
    return Database::get().execute_read([&](auto& txn) -> std::int64_t { return detail::read_balance(txn, user_id); });
}

/// Paged ledger history, newest first.
inline std::vector<Domain::WalletEntry> history(const std::string& user_id, int limit, int offset) {
    return Database::get().execute_read([&](auto& txn) {
        auto r = txn.exec_params(
            "SELECT id, user_id, delta_credits, kind, reference, note, created_by, created_at "
            "FROM wallet_entries WHERE user_id = $1 ORDER BY created_at DESC LIMIT $2 OFFSET $3",
            user_id,
            limit,
            offset);
        std::vector<Domain::WalletEntry> out;
        out.reserve(r.size());
        for (const auto& row : r)
            out.push_back(Domain::WalletEntry::from_row(row));
        return out;
    });
}

/**
 * @brief Idempotent capture-and-credit. Called from both the return-flow
 *        capture endpoint and the webhook (Tasks 4/5) — both funnel into
 *        this one function so there is exactly one place that can ever
 *        write a `topup` ledger row.
 *
 * ONE transaction: the guarded status/capture-id UPDATE, the amount check,
 * the ledger insert and the balance upsert all commit or roll back together.
 *
 * @throws Repositories::PaymentNotFound if @p provider_order_id is unknown.
 */
inline CreditResult credit_capture(const std::string& provider_order_id,
                                   const std::string& provider_capture_id,
                                   std::int64_t captured_amount_cents) {
    return Repositories::detail::translate_sql(
        [&] {
            return Database::get().execute_write([&](auto& txn) -> CreditResult {
                // Idempotency guard: only a payment that has never been captured
                // matches this UPDATE. A same-order concurrent duplicate
                // (return-flow racing the webhook) blocks on this row's lock and
                // then loses the WHERE once the winner commits, landing in the
                // empty-result branch below. If provider_capture_id already
                // belongs to a DIFFERENT order (pathological — PayPal capture
                // ids are unique by construction) the UNIQUE index raises 23505,
                // translated to DuplicateCaptureId below instead of surfacing as
                // a bare 500.
                auto ur = txn.exec_params(
                    "UPDATE payments SET provider_capture_id = $1, status = 'captured' "
                    "WHERE provider_order_id = $2 AND provider_capture_id IS NULL "
                    "RETURNING id, user_id, credits_expected, amount_cents",
                    provider_capture_id,
                    provider_order_id);

                if (ur.empty()) {
                    // Already captured, or an unknown order — re-read and report
                    // the existing state instead of crediting a second time.
                    auto pr = txn.exec_params("SELECT id, user_id FROM payments WHERE provider_order_id = $1",
                                              provider_order_id);
                    if (pr.empty())
                        throw Repositories::PaymentNotFound{};
                    const std::string payment_id = pr[0]["id"].template as<std::string>();
                    const std::string user_id = pr[0]["user_id"].template as<std::string>();
                    return CreditResult{false, detail::read_balance(txn, user_id), payment_id};
                }

                const std::string payment_id = ur[0]["id"].template as<std::string>();
                const std::string user_id = ur[0]["user_id"].template as<std::string>();
                const std::int64_t credits_expected = ur[0]["credits_expected"].template as<std::int64_t>();
                const std::int64_t amount_cents = ur[0]["amount_cents"].template as<std::int64_t>();

                if (captured_amount_cents != amount_cents) {
                    // The capture-id UPDATE above already ran, so re-processing
                    // this exact order can never reach this branch again
                    // (provider_capture_id is no longer NULL) — the failure is
                    // terminal, not retried.
                    const std::string reason = "amount mismatch: captured=" + std::to_string(captured_amount_cents) +
                                               " expected=" + std::to_string(amount_cents);
                    txn.exec_params(
                        "UPDATE payments SET status = 'failed', failure_reason = $1 WHERE id = $2", reason, payment_id);
                    spdlog::error(
                        "billing: capture amount mismatch for order {} (payment {}): captured={} expected={} — "
                        "payment marked failed, wallet untouched",
                        provider_order_id,
                        payment_id,
                        captured_amount_cents,
                        amount_cents);
                    return CreditResult{false, detail::read_balance(txn, user_id), payment_id};
                }

                txn.exec_params(
                    "INSERT INTO wallet_entries (user_id, delta_credits, kind, reference) VALUES ($1, $2, 'topup', $3)",
                    user_id,
                    credits_expected,
                    payment_id);
                auto br = txn.exec_params(
                    "INSERT INTO wallet_balances (user_id, credits) VALUES ($1, $2) "
                    "ON CONFLICT (user_id) DO UPDATE SET credits = wallet_balances.credits + EXCLUDED.credits, "
                    "updated_at = now() RETURNING credits",
                    user_id,
                    credits_expected);

                return CreditResult{true, br[0]["credits"].template as<std::int64_t>(), payment_id};
            });
        },
        [](std::string_view ss) {
            if (ss == "23505")
                throw DuplicateCaptureId{};
        });
}

/**
 * @brief Idempotent refund. Writes a negative `refund` ledger entry for
 *        @p refunded_amount_cents, converted to credits via the payment's
 *        frozen `rate_snapshot` (integer division — no floating point).
 *
 * Two transactions by design, not one:
 *   1. Flip `payments.status` captured→refunded. This is the durable,
 *      idempotent fact "PayPal refunded this capture" and must survive
 *      even if step 2 below can't apply.
 *   2. Write the ledger entry + balance upsert. If the wallet_balances
 *      CHECK (credits >= 0) rejects it — the user already spent below the
 *      refund amount — the refund is NOT silently dropped: the payment from
 *      step 1 stays `refunded`, and an error is logged for manual
 *      reconciliation, per the spec's "a refund must never be silently
 *      dropped" requirement.
 *
 * @throws Repositories::PaymentNotFound if @p provider_capture_id is unknown.
 */
inline CreditResult refund_capture(const std::string& provider_capture_id, std::int64_t refunded_amount_cents) {
    struct RefundTarget {
        std::string payment_id;
        std::string user_id;
        std::int64_t rate_snapshot;
        bool newly_refunded;
    };

    const RefundTarget target = Database::get().execute_write([&](auto& txn) -> RefundTarget {
        auto ur = txn.exec_params(
            "UPDATE payments SET status = 'refunded' WHERE provider_capture_id = $1 AND status = 'captured' "
            "RETURNING id, user_id, rate_snapshot",
            provider_capture_id);
        if (!ur.empty()) {
            return RefundTarget{ur[0]["id"].template as<std::string>(),
                                ur[0]["user_id"].template as<std::string>(),
                                ur[0]["rate_snapshot"].template as<std::int64_t>(),
                                /*newly_refunded=*/true};
        }
        // Not in `captured` state: already refunded, or the capture id is
        // unknown entirely.
        auto pr = txn.exec_params("SELECT id, user_id, rate_snapshot FROM payments WHERE provider_capture_id = $1",
                                  provider_capture_id);
        if (pr.empty())
            throw Repositories::PaymentNotFound{};
        return RefundTarget{pr[0]["id"].template as<std::string>(),
                            pr[0]["user_id"].template as<std::string>(),
                            pr[0]["rate_snapshot"].template as<std::int64_t>(),
                            /*newly_refunded=*/false};
    });

    if (!target.newly_refunded) {
        return CreditResult{false, balance_of(target.user_id), target.payment_id};
    }

    // Integer division against the frozen per-payment rate. For a full
    // refund (the common case) refunded_amount_cents == payments.amount_cents
    // and this evaluates to exactly payments.credits_expected.
    const std::int64_t refund_credits = (refunded_amount_cents * target.rate_snapshot) / 100;

    try {
        return Database::get().execute_write([&](auto& txn) -> CreditResult {
            txn.exec_params(
                "INSERT INTO wallet_entries (user_id, delta_credits, kind, reference) VALUES ($1, $2, 'refund', $3)",
                target.user_id,
                -refund_credits,
                target.payment_id);
            auto br = txn.exec_params(
                "INSERT INTO wallet_balances (user_id, credits) VALUES ($1, $2) "
                "ON CONFLICT (user_id) DO UPDATE SET credits = wallet_balances.credits + EXCLUDED.credits, "
                "updated_at = now() RETURNING credits",
                target.user_id,
                -refund_credits);
            return CreditResult{true, br[0]["credits"].template as<std::int64_t>(), target.payment_id};
        });
    } catch (const pqxx::sql_error& e) {
        // 23514 = check_violation. wallet_balances.CHECK(credits >= 0) is the
        // only check constraint this statement pair can trip (delta_credits
        // <> 0 would require refund_credits == 0, i.e. a sub-unit refund —
        // handled the same way: log and move on, the payment stays refunded).
        if (std::string_view(e.sqlstate()) != "23514")
            throw;
        spdlog::error(
            "billing: refund ledger write rejected (payment {} user {} would need {} credits deducted) — payment "
            "is marked refunded but the wallet was NOT debited; manual reconciliation required: {}",
            target.payment_id,
            target.user_id,
            refund_credits,
            e.what());
        return CreditResult{true, balance_of(target.user_id), target.payment_id};
    }
}

/**
 * @brief Admin manual entry (positive or negative). The caller is
 *        responsible for audit-logging this action (mirrors how every other
 *        admin mutation in this codebase writes its own audit_log row) —
 *        this function only owns the ledger + balance write.
 *
 * @throws InsufficientBalance if a negative @p delta_credits would drive the
 *         balance below zero (the wallet_balances CHECK rejects it). Unlike
 *         refund_capture, no real-world money has moved yet for an
 *         adjustment, so it is refused outright rather than applied with a
 *         reconciliation warning.
 */
inline CreditResult adjust(const std::string& user_id,
                           std::int64_t delta_credits,
                           const std::string& note,
                           const std::string& admin_id) {
    return Repositories::detail::translate_sql(
        [&] {
            return Database::get().execute_write([&](auto& txn) -> CreditResult {
                txn.exec_params(
                    "INSERT INTO wallet_entries (user_id, delta_credits, kind, reference, note, created_by) "
                    "VALUES ($1, $2, 'adjustment', '', $3, $4)",
                    user_id,
                    delta_credits,
                    note,
                    admin_id);
                auto br = txn.exec_params(
                    "INSERT INTO wallet_balances (user_id, credits) VALUES ($1, $2) "
                    "ON CONFLICT (user_id) DO UPDATE SET credits = wallet_balances.credits + EXCLUDED.credits, "
                    "updated_at = now() RETURNING credits",
                    user_id,
                    delta_credits);
                return CreditResult{true, br[0]["credits"].template as<std::int64_t>(), std::string{}};
            });
        },
        [](std::string_view ss) {
            if (ss == "23514")
                throw InsufficientBalance{};
        });
}

}  // namespace Billing
