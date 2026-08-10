/**
 * @file Wallet.hpp
 * @brief The credit wallet's crediting/refund/adjustment service — the ONLY
 *        code in this codebase allowed to write `wallet_entries` or
 *        `wallet_balances`. Everything else (the top-up endpoint, the PayPal
 *        webhook, the admin adjust endpoint — Tasks 4-6) calls into these
 *        four functions instead of touching the ledger tables directly.
 *
 * Money invariants enforced here (see migrations/007_billing.sql,
 * migrations/008_billing_refunds.sql, and
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
 *   - `refund_capture` is idempotent on the PayPal refund id, via a durable
 *     `billing_refunds` row written on EVERY call (applied or skipped) — NOT
 *     on payments.status and NOT on a wallet_entries row, since a skipped
 *     refund (see below) deliberately writes neither of those but must still
 *     be remembered so a redelivery can't re-decide (or worse, re-apply
 *     later once the balance happens to recover).
 *
 * `wallet_balances` upserts NEVER use a self-referencing SQL expression
 * (`SET credits = wallet_balances.credits + EXCLUDED.credits`) — every write
 * here locks the row first (`SELECT ... FOR UPDATE`), computes the new total
 * in C++ from that locked read, and upserts with `SET credits =
 * EXCLUDED.credits` against the explicit computed value. An earlier version
 * of this file used the self-referencing form; CI proved it does not
 * reliably read the pre-existing row inside its own ON CONFLICT DO UPDATE —
 * every write after the first for a given user silently discarded the true
 * prior balance and computed as though it were 0. Invisible for a positive
 * delta (a wrong-but-non-negative result never trips `CHECK (credits >=
 * 0)`); fatal for a negative one (a correctly-sufficient refund/debit
 * against a real balance still computed as negative and 500'd). See
 * refund_capture's docs and the fix-round report for the full diagnosis.
 *
 * Every write also INSERTs the row (`VALUES ($1, 0) ON CONFLICT (user_id) DO
 * NOTHING`) BEFORE the `SELECT ... FOR UPDATE`, then closes with a plain
 * `UPDATE ... WHERE user_id = $1` instead of an upsert. `SELECT ... FOR
 * UPDATE` cannot lock a row that does not exist yet, so without this a
 * brand-new user's first pair of concurrent balance-changing writes (no
 * `wallet_balances` row) would both read `current = 0` with no lock held,
 * and the second writer's result would silently clobber the first —
 * `wallet_balances.credits` disagreeing with `SUM(wallet_entries.delta_credits)`
 * even though the ledger itself stays correct (a lost-update on the cache,
 * not an over-credit). Materializing the row first guarantees the `FOR
 * UPDATE` always has something real to hold, so the second writer blocks
 * and reads the first writer's committed total instead of racing it.
 *
 * `CreditResult::credited` means "this call durably recorded something new"
 * — false only for the idempotent no-op (an already-seen refund id, or an
 * already-captured order). It does NOT mean "the wallet balance moved": a
 * skipped refund (insufficient balance, or a sub-unit amount that converts
 * to 0 credits at the payment's rate) still returns credited=true, because
 * the attempt itself is now durably recorded in `billing_refunds` — see
 * refund_capture's docs below. Tasks 4-6: branch on `credited` to mean "not
 * a duplicate", and inspect the payment/refund row separately for "did the
 * wallet actually move".
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
        : Repositories::ConflictError("insufficient_balance",
                                      "This adjustment would drive the wallet balance negative") {}
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
        : Repositories::ConflictError("capture_id_conflict",
                                      "This capture id is already recorded against a different payment") {}
};

/**
 * @brief → 400. `refund_capture` refuses a nonsensical, over-large, or
 *        cumulatively-over-large refund amount up front — nothing is
 *        written at all (not even a `billing_refunds` row), unlike the
 *        insufficient-balance/zero-credit cases which still record the
 *        attempt.
 */
struct InvalidRefundAmount : Repositories::ValidationError {
    InvalidRefundAmount()
        : Repositories::ValidationError(
              "invalid_refund_amount",
              "refunded_amount_cents must be > 0, <= the payment's amount_cents, and the running total of all "
              "refunds for this payment must not exceed it") {}
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
/// reference an existing user (wallet_entries.user_id FK).
struct UnknownUser : Repositories::NotFoundError {
    UnknownUser() : Repositories::NotFoundError("user") {}
};

/// → 400. `adjust()`'s user_id is not a syntactically valid UUID at all —
/// distinct from UnknownUser.
struct MalformedUserId : Repositories::ValidationError {
    MalformedUserId() : Repositories::ValidationError("invalid_user_id", "user_id is not a valid UUID") {}
};

/// → 404. `adjust()`'s admin_id is a syntactically valid UUID that doesn't
/// reference an existing user (wallet_entries.created_by FK). Distinct from
/// UnknownUser: the same insert has TWO FK columns, and a bare SQLSTATE from
/// that insert can't say which one was bad — see adjust()'s pre-checks.
struct UnknownAdmin : Repositories::NotFoundError {
    UnknownAdmin() : Repositories::NotFoundError("admin") {}
};

/// → 400. `adjust()`'s admin_id is not a syntactically valid UUID at all.
struct MalformedAdminId : Repositories::ValidationError {
    MalformedAdminId() : Repositories::ValidationError("invalid_admin_id", "admin_id is not a valid UUID") {}
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

/// Result of validating a single id against `users`, without ever letting a
/// malformed literal's SQLSTATE 22P02 escape mid-transaction (see
/// check_user_id's doc for why that matters).
enum class IdCheck { Valid, Malformed, Unknown };

/**
 * @brief Is @p id a well-formed UUID that references an existing user?
 *        Run as its OWN standalone read — never inside the transaction that
 *        will later write using this id. A malformed literal raises 22P02;
 *        catching that INSIDE a `pqxx::work` would leave the transaction
 *        aborted for every statement after it (Postgres: one failed
 *        statement poisons the rest of the transaction until rollback), so
 *        this check must fully complete and close its own transaction
 *        before the caller opens the real write. Reads the PRIMARY: a user
 *        or admin id created moments earlier (e.g. a signup immediately
 *        followed by a welcome-bonus adjust(), or a role just promoted to
 *        admin) must not be misreported as unknown because of replica lag.
 */
inline IdCheck check_user_id(const std::string& id) {
    try {
        return Database::get().execute_read_primary([&](auto& txn) {
            auto r = txn.exec_params("SELECT 1 FROM users WHERE id = $1", id);
            return r.empty() ? IdCheck::Unknown : IdCheck::Valid;
        });
    } catch (const pqxx::sql_error& e) {
        if (std::string_view(e.sqlstate()) == "22P02")
            return IdCheck::Malformed;
        throw;
    }
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
 * together. The balance upsert locks the wallet_balances row first
 * (`SELECT ... FOR UPDATE`) and computes the new total in C++ — see the
 * file-level note on why it never uses a self-referencing SQL expression.
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

                // Materialize the row FIRST so the lock below always has
                // something to hold. `SELECT ... FOR UPDATE` cannot lock a
                // row that doesn't exist yet: for a brand-new user (no
                // wallet_balances row), the FIRST pair of concurrent
                // balance-changing writes would both fall through to
                // `current = 0` unlocked, and the second writer's upsert
                // would silently overwrite the first's result with a
                // stale-based total — wallet_balances.credits then disagrees
                // with SUM(wallet_entries.delta_credits) even though the
                // ledger itself stays correct. `DO NOTHING` is safe: this
                // statement only needs a row to exist so the next statement
                // has something to lock; the real value is written by the
                // plain UPDATE below, under that lock.
                txn.exec_params(
                    "INSERT INTO wallet_balances (user_id, credits) VALUES ($1, 0) ON CONFLICT (user_id) DO NOTHING",
                    user_id);

                // Lock the balance row (now guaranteed to exist) and compute
                // the new total explicitly — see the file-level note on why
                // this never uses `SET credits = wallet_balances.credits +
                // EXCLUDED.credits`.
                auto wb = txn.exec_params("SELECT credits FROM wallet_balances WHERE user_id = $1 FOR UPDATE", user_id);
                const std::int64_t current_balance = wb[0]["credits"].template as<std::int64_t>();
                const std::int64_t new_total = current_balance + credits_expected;
                // Plain UPDATE, not an upsert: the row is provably present
                // (materialized above) and locked (SELECT ... FOR UPDATE
                // above), so there is nothing left to conflict against.
                auto br = txn.exec_params(
                    "UPDATE wallet_balances SET credits = $2, updated_at = now() WHERE user_id = $1 RETURNING credits",
                    user_id,
                    new_total);

                return CreditResult{true, br[0]["credits"].template as<std::int64_t>(), payment_id};
            });
        },
        [](std::string_view ss) {
            if (ss == "23505")
                throw DuplicateCaptureId{};
        });
}

/**
 * @brief Idempotent refund. Converts @p refunded_amount_cents to credits by
 *        prorating the payment's frozen `credits_expected` —
 *        `credits_expected * refunded_amount_cents / amount_cents` (integer
 *        division — no floating point) — and writes a negative `refund`
 *        ledger entry IF the conversion and the current balance allow it.
 *
 *        Deliberately NOT `refunded_amount_cents * rate_snapshot / 100`: a
 *        package sale's `credits_expected` is priced independently of
 *        `rate_snapshot` (a bonus/discount package hands out more or fewer
 *        credits than the generic per-unit rate would imply — see
 *        BillingController::topup), so a rate_snapshot-based conversion
 *        silently disagrees with what was actually credited. Prorating off
 *        `credits_expected` instead is exact for a full refund by
 *        construction — `(X * Y) / Y == X` for any nonzero Y, so
 *        `refunded_amount_cents == amount_cents` always yields exactly
 *        `credits_expected` back, whether the payment came from a package or
 *        a custom amount — and proportional (floored) for a partial one.
 *        `rate_snapshot` is retained on `payments` purely as an informational
 *        record of the per-unit rate in effect at purchase time; nothing in
 *        this file reads it anymore.
 *
 * ONE transaction. The durable idempotency marker is a `billing_refunds` row
 * (migrations/008_billing_refunds.sql), written on EVERY non-duplicate call,
 * whatever the outcome:
 *
 *   1. Look up the payment by @p provider_capture_id — `FOR UPDATE`. This is
 *      what makes the aggregate check in step 3 race-safe: TWO concurrent
 *      refunds against the SAME payment (distinct refund ids, e.g. 600 and
 *      500 on a 1000-cent payment) now serialize on this row instead of
 *      both reading `already_refunded=0` and both passing. It also gives
 *      this function the same lock ORDER as credit_capture — payments
 *      first, wallet_balances second (see step 4) — so the two functions
 *      can never deadlock against each other over the same payment/user.
 *   2. Idempotency FIRST: if `billing_refunds` already has a row for
 *      @p provider_refund_id, this is a redelivery — return the existing
 *      state, write nothing. This is checked BEFORE the amount validation
 *      and BEFORE the sufficiency decision below, and it is why a
 *      redelivered refund that was previously skipped (insufficient balance,
 *      or a zero-credit conversion) stays skipped forever, even if the
 *      wallet balance later recovers enough to afford it — an earlier
 *      wallet_entries-based idempotency check could not do this, because a
 *      skipped refund never wrote a wallet_entries row at all.
 *   3. Validate @p refunded_amount_cents: must be `> 0` and
 *      `<= payments.amount_cents`, AND the running total of every
 *      `billing_refunds` row for this payment (applied or skipped — PayPal's
 *      own count of what it refunded, not just what the wallet could
 *      reflect) plus this one must not exceed `payments.amount_cents`. The
 *      payments row lock from step 1 makes this sum trustworthy even under
 *      concurrent refund attempts. Either violation throws
 *      InvalidRefundAmount; the whole transaction rolls back, nothing is
 *      written — not even a `billing_refunds` row, since this refund was
 *      never a real attempt against this payment.
 *   4. Convert to credits (`credits_expected * refunded_amount_cents /
 *      amount_cents`, integer division — see the function-level note above
 *      on why this prorates off `credits_expected`, not `rate_snapshot`).
 *      `SELECT credits FROM wallet_balances ... FOR
 *      UPDATE` locks the user's balance row ONCE, up front, whatever the
 *      outcome — its value seeds the response in every branch below, and is
 *      also the sufficiency-decision input, so there is exactly one query
 *      here rather than a second one only some branches need. Two ways the
 *      credit application can be skipped, NEITHER of which may ever let the
 *      wallet_entries/wallet_balances CHECK constraints fire (that's what
 *      silently 500'd before):
 *        - `refund_credits == 0`: the amount is too small to represent at
 *          this payment's credits_expected/amount_cents ratio (e.g. 10
 *          credits for a 1000-cent payment, refund of 1-99 cents floors to
 *          0). The `wallet_entries.delta_credits <> 0` CHECK would otherwise
 *          reject the insert outright. Outcome: `skipped_zero_credits`.
 *        - the locked balance can't cover it
 *          (`current_balance - refund_credits < 0`), decided in C++ BEFORE
 *          any write, so `wallet_balances.CHECK (credits >= 0)` is never
 *          actually exercised. Outcome: `skipped_insufficient`.
 *      Either way: log at `error` for manual reconciliation, write the
 *      `billing_refunds` row with that outcome and `credits_deducted = 0`,
 *      skip the ledger insert entirely. Otherwise: ledger insert + balance
 *      upsert (the NEW total — `new_balance - refund_credits` — computed
 *      from the locked read above and set explicitly, never via a
 *      self-referencing SQL expression), outcome `applied`,
 *      `credits_deducted = refund_credits`.
 *   5. `payments.status` becomes `refunded` when the CUMULATIVE total of
 *      every refund against this payment (steps 3's running total,
 *      including this one) reaches `payments.amount_cents` AND the payment
 *      is currently `captured` — so a payment fully refunded across several
 *      partials (e.g. 400 then 600 on a 1000-cent payment) DOES end up
 *      `refunded` once the last one lands, not just a single call whose own
 *      amount happens to equal the full total. This fires regardless of the
 *      ledger outcome above (a full-by-now refund is a PayPal-side fact
 *      independent of whether the wallet could reflect every part of it),
 *      but a refund that leaves the cumulative total short of the full
 *      amount never touches `payments.status`, and a refund against a
 *      payment that isn't `captured` (e.g. `failed`) can't accidentally
 *      flip it either.
 *
 * All of the above commits atomically in this one transaction.
 *
 * A concurrent duplicate (two redeliveries of the same refund id racing each
 * other) loses on `billing_refunds.provider_refund_id`'s UNIQUE constraint
 * (SQLSTATE 23505) — caught below and reported as the now-idempotent
 * existing state instead of a raw 500.
 *
 * KNOWN GAP (flagged for Task 5, not fixed here — no PayPal client exists
 * yet to reverse anything): a refund that PayPal later reverses/voids has
 * no representation in this schema. The `billing_refunds` row from the
 * original refund event would stay in place, permanently counting toward
 * the aggregate total in step 3 — a legitimate LATER refund on the same
 * payment could then be wrongly refused as "cumulative total exceeds
 * amount_cents" even though the voided refund never actually took money out
 * a second time. Task 5's webhook handler should account for this (e.g. a
 * `REFUND.REVERSED` event needs its own handling here, not just a bigger
 * refund) before it's reachable in production.
 *
 * @throws Repositories::PaymentNotFound if @p provider_capture_id is unknown.
 * @throws InvalidRefundAmount if @p refunded_amount_cents is out of range,
 *         single-call or cumulative.
 */
inline CreditResult refund_capture(const std::string& provider_capture_id,
                                   const std::string& provider_refund_id,
                                   std::int64_t refunded_amount_cents) {
    auto attempt = [&](auto& txn) -> CreditResult {
        // FOR UPDATE: see step 1 above — this is what makes the aggregate
        // check below race-safe against a concurrent refund on the same
        // payment, and fixes this function's lock order to match
        // credit_capture's (payments, then wallet_balances).
        auto pr = txn.exec_params(
            "SELECT id, user_id, amount_cents, credits_expected FROM payments WHERE provider_capture_id = $1 "
            "FOR UPDATE",
            provider_capture_id);
        if (pr.empty())
            throw Repositories::PaymentNotFound{};
        const std::string payment_id = pr[0]["id"].template as<std::string>();
        const std::string user_id = pr[0]["user_id"].template as<std::string>();
        const std::int64_t amount_cents = pr[0]["amount_cents"].template as<std::int64_t>();
        const std::int64_t credits_expected = pr[0]["credits_expected"].template as<std::int64_t>();

        // Idempotency FIRST, via the durable marker — see the function docs
        // for why this can no longer be "does a wallet_entries row exist".
        auto existing =
            txn.exec_params("SELECT id FROM billing_refunds WHERE provider_refund_id = $1", provider_refund_id);
        if (!existing.empty()) {
            return CreditResult{false, detail::read_balance(txn, user_id), payment_id};
        }

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

        // Trustworthy under concurrency because the payments row is already
        // locked (step 1) — a second concurrent refund on this same payment
        // blocked there until this transaction commits or rolls back.
        auto sum_row = txn.exec_params(
            "SELECT COALESCE(SUM(amount_cents), 0) AS total FROM billing_refunds WHERE payment_id = $1", payment_id);
        const std::int64_t already_refunded = sum_row[0]["total"].template as<std::int64_t>();
        const std::int64_t cumulative_total = already_refunded + refunded_amount_cents;
        if (cumulative_total > amount_cents) {
            spdlog::error(
                "billing: refund {} for payment {} would push cumulative refunds past the payment amount: "
                "already_refunded={} this_refund={} payment_amount={} — refused, nothing written",
                provider_refund_id,
                payment_id,
                already_refunded,
                refunded_amount_cents,
                amount_cents);
            throw InvalidRefundAmount{};
        }

        // Prorate off credits_expected, NOT rate_snapshot — see the
        // function-level doc comment. `(credits_expected * amount_cents) /
        // amount_cents == credits_expected` exactly for a full refund, no
        // matter how credits_expected was priced (package or custom).
        const std::int64_t refund_credits = (credits_expected * refunded_amount_cents) / amount_cents;

        // Materialize the row FIRST so the lock below always has something
        // to hold — see credit_capture's doc comment for the full
        // lost-update diagnosis this guards against (a not-yet-existing row
        // can't be locked by `FOR UPDATE`, so the first pair of concurrent
        // balance-changing writes for a brand-new user could both read
        // current=0 unlocked and the second silently clobber the first).
        // `DO NOTHING` is safe: only existence matters here, not the value.
        txn.exec_params(
            "INSERT INTO wallet_balances (user_id, credits) VALUES ($1, 0) ON CONFLICT (user_id) DO NOTHING",
            user_id);

        // Lock the balance row (now guaranteed to exist) ONCE, up front,
        // whatever the outcome turns out to be — its value seeds the
        // response in all three branches below and is the
        // sufficiency-decision input in one of them, so there's no second
        // (and, on the zero-credit path, wasted) query.
        auto br = txn.exec_params("SELECT credits FROM wallet_balances WHERE user_id = $1 FOR UPDATE", user_id);
        std::int64_t new_balance = br[0]["credits"].template as<std::int64_t>();

        std::string outcome;
        std::int64_t credits_deducted = 0;

        if (refund_credits == 0) {
            // Too small to represent at this payment's credits_expected/
            // amount_cents ratio — inserting delta_credits=0 would trip
            // wallet_entries' CHECK (delta_credits <> 0). Record the attempt
            // and move on instead of letting that escape as a raw constraint
            // violation (that used to 500 and get redelivered by PayPal
            // forever).
            outcome = "skipped_zero_credits";
            spdlog::error(
                "billing: refund {} for payment {} converts to 0 credits (credits_expected={} amount_cents={} "
                "refunded_amount_cents={}) — recorded, wallet untouched; manual reconciliation may be needed",
                provider_refund_id,
                payment_id,
                credits_expected,
                amount_cents,
                refunded_amount_cents);
        } else if (new_balance - refund_credits < 0) {
            outcome = "skipped_insufficient";
            spdlog::error(
                "billing: refund {} for payment {} user {} needs {} credits but only {} are available — "
                "recorded, wallet NOT debited; manual reconciliation required",
                provider_refund_id,
                payment_id,
                user_id,
                refund_credits,
                new_balance);
        } else {
            // Compute the new total explicitly from the locked read above and
            // set it directly — never via `SET credits = wallet_balances.credits
            // + EXCLUDED.credits` (see the file-level note).
            const std::int64_t computed_new_balance = new_balance - refund_credits;
            txn.exec_params(
                "INSERT INTO wallet_entries (user_id, delta_credits, kind, reference) VALUES ($1, $2, 'refund', $3)",
                user_id,
                -refund_credits,
                provider_refund_id);
            // Plain UPDATE, not an upsert: the row is provably present
            // (materialized above) and locked (SELECT ... FOR UPDATE above).
            auto updated = txn.exec_params(
                "UPDATE wallet_balances SET credits = $2, updated_at = now() WHERE user_id = $1 RETURNING credits",
                user_id,
                computed_new_balance);
            new_balance = updated[0]["credits"].template as<std::int64_t>();
            outcome = "applied";
            credits_deducted = refund_credits;
        }

        // The durable marker — written for every outcome, so a redelivery of
        // this exact refund id always short-circuits at the idempotency
        // check above, whatever happened the first time.
        txn.exec_params(
            "INSERT INTO billing_refunds (payment_id, provider_refund_id, amount_cents, credits_deducted, outcome) "
            "VALUES ($1, $2, $3, $4, $5)",
            payment_id,
            provider_refund_id,
            refunded_amount_cents,
            credits_deducted,
            outcome);

        // The CUMULATIVE total (not just this call's amount) reaching the
        // full payment amount flips a currently-captured payment to
        // refunded — regardless of whether the wallet could be debited (the
        // money left PayPal either way). A cumulative total still short of
        // the full amount, or a payment that isn't 'captured' (e.g.
        // 'failed'), leaves status untouched (0 rows affected below is not
        // an error).
        txn.exec_params(
            "UPDATE payments SET status = 'refunded' WHERE id = $1 AND status = 'captured' AND $2 >= amount_cents",
            payment_id,
            cumulative_total);

        // credited=true past this point unconditionally: this call durably
        // recorded a NEW billing_refunds row either way — see the file-level
        // note on CreditResult::credited. Only the idempotency short-circuit
        // above (an already-seen refund id) returns credited=false.
        return CreditResult{true, new_balance, payment_id};
    };

    try {
        return Database::get().execute_write(attempt);
    } catch (const pqxx::sql_error& e) {
        if (std::string_view(e.sqlstate()) != "23505")
            throw;
        // Lost a race against a concurrent identical refund id — the winner's
        // insert already applied. Report the current state instead of
        // retrying; do NOT re-attempt the write ourselves. Read the PRIMARY:
        // the winner just committed there, and a replica could still be lagging.
        auto pr = Database::get().execute_read_primary([&](auto& txn) {
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
 * @p user_id and @p admin_id are both foreign keys on the SAME
 * wallet_entries insert (`user_id`, `created_by`); a bare SQLSTATE from that
 * insert can't say which one was bad, so both are validated explicitly,
 * user_id first, as their own standalone reads BEFORE the write transaction
 * opens (see detail::check_user_id — the same "don't catch 22P02 mid-
 * transaction" reasoning that governs refund_capture's design). The balance
 * upsert locks wallet_balances first and computes the new total in C++ —
 * see the file-level note on why it never uses a self-referencing SQL
 * expression.
 *
 * @throws ZeroAdjustment if @p delta_credits is 0.
 * @throws MalformedUserId / UnknownUser if @p user_id is malformed / unknown.
 * @throws MalformedAdminId / UnknownAdmin if @p admin_id is malformed / unknown.
 * @throws InsufficientBalance if a negative @p delta_credits would drive the
 *         balance below zero. Unlike refund_capture, no real-world money has
 *         moved yet for an adjustment, so it is refused outright rather than
 *         applied with a reconciliation warning.
 */
inline CreditResult adjust(const std::string& user_id,
                           std::int64_t delta_credits,
                           const std::string& note,
                           const std::string& admin_id) {
    if (delta_credits == 0)
        throw ZeroAdjustment{};

    switch (detail::check_user_id(user_id)) {
        case detail::IdCheck::Malformed:
            throw MalformedUserId{};
        case detail::IdCheck::Unknown:
            throw UnknownUser{};
        case detail::IdCheck::Valid:
            break;
    }
    switch (detail::check_user_id(admin_id)) {
        case detail::IdCheck::Malformed:
            throw MalformedAdminId{};
        case detail::IdCheck::Unknown:
            throw UnknownAdmin{};
        case detail::IdCheck::Valid:
            break;
    }

    return Repositories::detail::translate_sql(
        [&] {
            return Database::get().execute_write([&](auto& txn) -> CreditResult {
                // Materialize the row FIRST so the lock below always has
                // something to hold — see credit_capture's doc comment for
                // the full lost-update diagnosis this guards against (a
                // not-yet-existing row can't be locked by `FOR UPDATE`, so
                // the first pair of concurrent balance-changing writes for a
                // brand-new user could both read current=0 unlocked and the
                // second silently clobber the first). `DO NOTHING` is safe:
                // only existence matters here, not the value.
                txn.exec_params(
                    "INSERT INTO wallet_balances (user_id, credits) VALUES ($1, 0) ON CONFLICT (user_id) DO NOTHING",
                    user_id);

                // Lock the balance row (now guaranteed to exist) and compute
                // the new total explicitly — see the file-level note on why
                // this never uses `SET credits = wallet_balances.credits +
                // EXCLUDED.credits`. A negative new_total is still written
                // as-is: the CHECK (credits >= 0) then reliably rejects it
                // (this is now a plain "does this literal value satisfy the
                // constraint" check, not a self-referencing expression),
                // caught below as SQLSTATE 23514.
                auto wb = txn.exec_params("SELECT credits FROM wallet_balances WHERE user_id = $1 FOR UPDATE", user_id);
                const std::int64_t current_balance = wb[0]["credits"].template as<std::int64_t>();
                const std::int64_t new_total = current_balance + delta_credits;

                txn.exec_params(
                    "INSERT INTO wallet_entries (user_id, delta_credits, kind, reference, note, created_by) "
                    "VALUES ($1, $2, 'adjustment', '', $3, $4)",
                    user_id,
                    delta_credits,
                    note,
                    admin_id);
                // Plain UPDATE, not an upsert: the row is provably present
                // (materialized above) and locked (SELECT ... FOR UPDATE
                // above), so there is nothing left to conflict against.
                auto br = txn.exec_params(
                    "UPDATE wallet_balances SET credits = $2, updated_at = now() WHERE user_id = $1 RETURNING credits",
                    user_id,
                    new_total);
                return CreditResult{true, br[0]["credits"].template as<std::int64_t>(), std::string{}};
            });
        },
        [](std::string_view ss) {
            if (ss == "23514")
                throw InsufficientBalance{};
            // Defense in depth only — the pre-checks above make these two
            // unreachable except for an extremely narrow TOCTOU race (a user
            // deleted between the check and this write). Attribute to
            // user_id, the more common cause, rather than silently 500ing.
            if (ss == "23503")
                throw UnknownUser{};
            if (ss == "22P02")
                throw MalformedUserId{};
        });
}

}  // namespace Billing
