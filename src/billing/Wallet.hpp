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
 *     race guard, backed by the UNIQUE index in the migration;
 *   - `refund_capture` is idempotent on the PayPal refund id (NOT on payment
 *     status — a payment can receive more than one partial refund, and each
 *     distinct refund id must apply exactly once).
 *
 * `CreditResult::credited` means "the ledger moved credits on THIS call".
 * It is false for every idempotent no-op (already processed). The one
 * exception is refund_capture's insufficient-balance branch: it returns
 * `credited=true` because the payment-level fact ("PayPal refunded this
 * capture") did change even though the wallet could not be debited — see
 * refund_capture's docs below. Tasks 4-6: branch on `credited` to mean
 * "did the ledger move", and inspect the payment/response body separately
 * for "was the refund/capture itself recorded".
 */

#pragma once

#include <cstdint>
#include <optional>
#include <pqxx/pqxx>
#include <string>
#include <string_view>
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

/**
 * @brief → 400. `refund_capture` refuses a nonsensical or over-large refund
 *        amount up front — nothing is written at all (payment status
 *        included), unlike the insufficient-balance case which still marks
 *        the payment refunded.
 */
struct InvalidRefundAmount : Repositories::ValidationError {
    InvalidRefundAmount()
        : Repositories::ValidationError("invalid_refund_amount",
                                        "refunded_amount_cents must be > 0 and <= the payment's amount_cents") {}
};

/**
 * @brief → 400. `adjust()` refuses a no-op delta outright instead of letting
 *        it trip the wallet_entries CHECK (delta_credits <> 0) and surface
 *        as a confusing "insufficient_balance".
 */
struct ZeroAdjustment : Repositories::ValidationError {
    ZeroAdjustment() : Repositories::ValidationError("zero_adjustment", "delta_credits must not be zero") {}
};

/// → 404. `adjust()`'s user_id is a syntactically valid UUID that doesn't
/// reference an existing user (SQLSTATE 23503 on the wallet FK).
struct UnknownUser : Repositories::NotFoundError {
    UnknownUser() : Repositories::NotFoundError("user") {}
};

/// → 400. `adjust()`'s user_id is not a syntactically valid UUID at all
/// (SQLSTATE 22P02) — distinct from UnknownUser.
struct MalformedUserId : Repositories::ValidationError {
    MalformedUserId() : Repositories::ValidationError("invalid_user_id", "user_id is not a valid UUID") {}
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
 * @brief Read the cached balance. @p from_primary forces the primary
 *        instead of a (possibly lagging) replica — pass true right after a
 *        write in the same request (e.g. immediately after credit_capture)
 *        so the caller never sees a stale pre-write balance.
 */
inline std::int64_t balance_of(const std::string& user_id, bool from_primary = false) {
    auto query = [&](auto& txn) -> std::int64_t { return detail::read_balance(txn, user_id); };
    return from_primary ? Database::get().execute_read_primary(query) : Database::get().execute_read(query);
}

/// Paged ledger history, newest first. @p from_primary — see balance_of().
inline std::vector<Domain::WalletEntry> history(const std::string& user_id,
                                                int limit,
                                                int offset,
                                                bool from_primary = false) {
    auto query = [&](auto& txn) {
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
    };
    return from_primary ? Database::get().execute_read_primary(query) : Database::get().execute_read(query);
}

/**
 * @brief Idempotent capture-and-credit. Called from both the return-flow
 *        capture endpoint and the webhook (Tasks 4/5) — both funnel into
 *        this one function so there is exactly one place that can ever
 *        write a `topup` ledger row.
 *
 * ONE transaction: the guarded status/capture-id UPDATE, the amount+currency
 * check, the ledger insert and the balance upsert all commit or roll back
 * together.
 *
 * @throws Repositories::PaymentNotFound if @p provider_order_id is unknown.
 * @throws DuplicateCaptureId if @p provider_capture_id already belongs to a
 *         different payment (SQLSTATE 23505 on the UNIQUE index).
 */
inline CreditResult credit_capture(const std::string& provider_order_id,
                                   const std::string& provider_capture_id,
                                   std::int64_t captured_amount_cents,
                                   const std::string& captured_currency) {
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
                    "RETURNING id, user_id, credits_expected, amount_cents, currency",
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
                const std::string currency = ur[0]["currency"].template as<std::string>();

                const bool amount_ok = captured_amount_cents == amount_cents;
                const bool currency_ok = captured_currency == currency;
                if (!amount_ok || !currency_ok) {
                    // The capture-id UPDATE above already ran, so re-processing
                    // this exact order can never reach this branch again
                    // (provider_capture_id is no longer NULL) — the failure is
                    // terminal, not retried.
                    std::string reason;
                    if (!amount_ok)
                        reason += "amount mismatch: captured=" + std::to_string(captured_amount_cents) +
                                  " expected=" + std::to_string(amount_cents);
                    if (!currency_ok) {
                        if (!reason.empty())
                            reason += "; ";
                        reason += "currency mismatch: captured=" + captured_currency + " expected=" + currency;
                    }
                    txn.exec_params(
                        "UPDATE payments SET status = 'failed', failure_reason = $1 WHERE id = $2", reason, payment_id);
                    spdlog::error(
                        "billing: capture mismatch for order {} (payment {}): {} — payment marked failed, wallet "
                        "untouched",
                        provider_order_id,
                        payment_id,
                        reason);
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
 * ONE transaction (a prior version split this into two — see git history —
 * to dodge the wallet_balances CHECK; that left the refund permanently lost
 * whenever the SECOND transaction failed for any reason OTHER than the
 * CHECK, since execute_write does not retry most errors and PayPal's
 * redelivery would then find payments.status already `refunded` and treat
 * it as a no-op forever. Fixed by never relying on catching the CHECK at
 * all — the sufficiency check happens in C++, before any write):
 *
 *   1. Look up the payment by @p provider_capture_id and validate
 *      @p refunded_amount_cents (> 0, <= payments.amount_cents). Invalid →
 *      throw InvalidRefundAmount; the whole transaction rolls back, nothing
 *      is written (not even the payment status).
 *   2. Idempotency: if a `refund` ledger row already carries
 *      @p provider_refund_id as its `reference`, this is a redelivery of an
 *      already-applied refund — return the existing state, write nothing.
 *      Keying on the refund id (not on payments.status) is what makes
 *      multiple distinct partial refunds against the same capture each
 *      apply exactly once instead of the second one collapsing into a
 *      silent no-op.
 *   3. `SELECT credits FROM wallet_balances ... FOR UPDATE` locks the
 *      user's balance row and lets the sufficiency check happen in C++
 *      BEFORE any write — so the wallet_balances CHECK (credits >= 0) is
 *      never actually exercised by this function. If the refund would
 *      drive the balance negative (the user already spent below it), the
 *      ledger insert is skipped, an error is logged for manual
 *      reconciliation, and `payments.status` is still set to `refunded` IN
 *      THE SAME TRANSACTION — the refund is real (PayPal returned the
 *      money) even though the wallet couldn't be debited, and it must
 *      never be silently dropped. `credited=true` is returned in this
 *      branch specifically because the payment-level fact changed, even
 *      though the ledger did not.
 *   4. Otherwise the ledger insert + balance upsert + status update commit
 *      together, atomically, all in this one transaction — so a crash
 *      between them is impossible; either all three landed or none did.
 *
 * A concurrent duplicate (two redeliveries of the same refund id racing
 * each other) loses on the migration's partial UNIQUE index
 * (`wallet_entries (reference) WHERE kind='refund'`) exactly like
 * credit_capture's capture-id race — caught below and reported as the
 * now-idempotent existing state instead of a raw 500.
 *
 * @throws Repositories::PaymentNotFound if @p provider_capture_id is unknown.
 * @throws InvalidRefundAmount if @p refunded_amount_cents is out of range.
 */
inline CreditResult refund_capture(const std::string& provider_capture_id,
                                   const std::string& provider_refund_id,
                                   std::int64_t refunded_amount_cents) {
    auto attempt = [&](auto& txn) -> CreditResult {
        auto pr = txn.exec_params(
            "SELECT id, user_id, amount_cents, rate_snapshot FROM payments WHERE provider_capture_id = $1",
            provider_capture_id);
        if (pr.empty())
            throw Repositories::PaymentNotFound{};
        const std::string payment_id = pr[0]["id"].template as<std::string>();
        const std::string user_id = pr[0]["user_id"].template as<std::string>();
        const std::int64_t amount_cents = pr[0]["amount_cents"].template as<std::int64_t>();
        const std::int64_t rate_snapshot = pr[0]["rate_snapshot"].template as<std::int64_t>();

        if (refunded_amount_cents <= 0 || refunded_amount_cents > amount_cents) {
            spdlog::error(
                "billing: refund {} for payment {} (capture {}) has an out-of-range amount: refunded={} "
                "payment_amount={} — refused, nothing written",
                provider_refund_id,
                payment_id,
                provider_capture_id,
                refunded_amount_cents,
                amount_cents);
            throw InvalidRefundAmount{};
        }

        // Idempotency: this exact refund id already landed a ledger row.
        auto existing =
            txn.exec_params("SELECT id FROM wallet_entries WHERE kind = 'refund' AND reference = $1", provider_refund_id);
        if (!existing.empty()) {
            return CreditResult{false, detail::read_balance(txn, user_id), payment_id};
        }

        // Integer division against the frozen per-payment rate — no floats.
        // For a full refund (refunded_amount_cents == amount_cents) this is
        // exactly payments.credits_expected.
        const std::int64_t refund_credits = (refunded_amount_cents * rate_snapshot) / 100;

        // Lock the balance row so the sufficiency check below can't race a
        // concurrent write — decide BEFORE touching wallet_entries, so the
        // CHECK (credits >= 0) is never actually exercised.
        auto br = txn.exec_params("SELECT credits FROM wallet_balances WHERE user_id = $1 FOR UPDATE", user_id);
        const std::int64_t current_balance = br.empty() ? 0 : br[0]["credits"].template as<std::int64_t>();

        std::int64_t new_balance = current_balance;
        if (current_balance - refund_credits >= 0) {
            txn.exec_params(
                "INSERT INTO wallet_entries (user_id, delta_credits, kind, reference) VALUES ($1, $2, 'refund', $3)",
                user_id,
                -refund_credits,
                provider_refund_id);
            auto upserted = txn.exec_params(
                "INSERT INTO wallet_balances (user_id, credits) VALUES ($1, $2) "
                "ON CONFLICT (user_id) DO UPDATE SET credits = wallet_balances.credits + EXCLUDED.credits, "
                "updated_at = now() RETURNING credits",
                user_id,
                -refund_credits);
            new_balance = upserted[0]["credits"].template as<std::int64_t>();
        } else {
            spdlog::error(
                "billing: refund {} for payment {} user {} needs {} credits but only {} are available — payment "
                "is being marked refunded, but the wallet was NOT debited; manual reconciliation required",
                provider_refund_id,
                payment_id,
                user_id,
                refund_credits,
                current_balance);
        }

        // Idempotent SET regardless of whether this is the first or a later
        // partial refund on this capture — no WHERE guard needed since
        // idempotency is keyed on the refund id above, not on this status.
        txn.exec_params("UPDATE payments SET status = 'refunded' WHERE id = $1", payment_id);

        // credited=true past this point unconditionally: the payment-level
        // fact ("this refund id was processed") changed on THIS call either
        // way, whether or not the ledger itself could move — see the
        // file-level note on CreditResult::credited. Only the idempotency
        // short-circuit above (an already-seen refund id) returns
        // credited=false.
        return CreditResult{true, new_balance, payment_id};
    };

    try {
        return Database::get().execute_write(attempt);
    } catch (const pqxx::sql_error& e) {
        if (std::string_view(e.sqlstate()) != "23505")
            throw;
        // Lost a race against a concurrent identical refund id — the winner's
        // insert already applied. Report the current state instead of
        // retrying; do NOT re-attempt the write ourselves. from_primary=true:
        // read the primary the winner just committed to, not a possibly
        // lagging replica.
        auto pr = Database::get().execute_read([&](auto& txn) {
            return txn.exec_params("SELECT id, user_id FROM payments WHERE provider_capture_id = $1",
                                   provider_capture_id);
        });
        if (pr.empty())
            throw Repositories::PaymentNotFound{};
        const std::string payment_id = pr[0]["id"].template as<std::string>();
        const std::string user_id = pr[0]["user_id"].template as<std::string>();
        return CreditResult{false, balance_of(user_id, /*from_primary=*/true), payment_id};
    }
}

/**
 * @brief Admin manual entry (positive or negative). The caller is
 *        responsible for audit-logging this action (mirrors how every other
 *        admin mutation in this codebase writes its own audit_log row) —
 *        this function only owns the ledger + balance write.
 *
 * @throws ZeroAdjustment if @p delta_credits is 0.
 * @throws InsufficientBalance if a negative @p delta_credits would drive the
 *         balance below zero (the wallet_balances CHECK rejects it). Unlike
 *         refund_capture, no real-world money has moved yet for an
 *         adjustment, so it is refused outright rather than applied with a
 *         reconciliation warning.
 * @throws UnknownUser if @p user_id doesn't reference an existing user
 *         (SQLSTATE 23503).
 * @throws MalformedUserId if @p user_id isn't a syntactically valid UUID
 *         (SQLSTATE 22P02).
 */
inline CreditResult adjust(const std::string& user_id,
                           std::int64_t delta_credits,
                           const std::string& note,
                           const std::string& admin_id) {
    if (delta_credits == 0)
        throw ZeroAdjustment{};

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
            if (ss == "23503")
                throw UnknownUser{};
            if (ss == "22P02")
                throw MalformedUserId{};
        });
}

}  // namespace Billing
