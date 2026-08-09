-- Migration 008: durable refund idempotency marker + defensive re-declaration
-- of two 007 additions.
--
-- Applied in numeric order on boot. The MigrationRunner wraps this file in ONE
-- transaction under an advisory lock — do NOT add BEGIN/COMMIT. Idempotent DDL.
--
-- Why this exists instead of another edit to 007_billing.sql: the runner
-- tracks applied migrations by VERSION NUMBER ONLY (schema_migrations, no
-- checksum) — see MigrationRunner::list_pending. A database that already
-- recorded version 7 before 007_billing.sql was edited in place (Task 2 fix
-- round 1, adding CHECK(rate_snapshot > 0) and idx_wallet_entries_refund_ref)
-- will NEVER re-run that file, so it silently lacks both additions forever.
-- Every statement below is therefore written to be safe whether the target
-- database has the original 007 or the edited one — no more editing 007.

-- Partial unique index backing refund idempotency (007's comment explains the
-- shape) — IF NOT EXISTS makes repeating this a no-op on a database that
-- already has it.
CREATE UNIQUE INDEX IF NOT EXISTS idx_wallet_entries_refund_ref ON wallet_entries (reference) WHERE kind = 'refund';

-- rate_snapshot > 0. PostgreSQL has no `ADD CONSTRAINT IF NOT EXISTS`, so
-- guard on pg_constraint directly. The name matches Postgres's own default
-- for an unnamed single-column CHECK (<table>_<column>_check), so this
-- converges with 007's inline CHECK on a database that already has it —
-- the DO block is then a no-op there, and only actually adds the constraint
-- on a database still running the pre-edit 007.
DO $$
BEGIN
    IF NOT EXISTS (
        SELECT 1 FROM pg_constraint
        WHERE conrelid = 'payments'::regclass
          AND conname = 'payments_rate_snapshot_check'
    ) THEN
        ALTER TABLE payments ADD CONSTRAINT payments_rate_snapshot_check CHECK (rate_snapshot > 0);
    END IF;
END $$;

-- billing_refunds: the durable, per-refund-id idempotency marker for
-- Billing::refund_capture (src/billing/Wallet.hpp). A `wallet_entries` row
-- can no longer serve as that marker: a refund that converts to 0 credits at
-- the payment's rate, or one that would drive the wallet negative, is
-- deliberately SKIPPED at the ledger level — without a durable record of the
-- attempt, a redelivered webhook would silently re-run that same decision
-- from scratch every time (and, worse, could apply a delayed debit once the
-- balance happened to recover, double-processing a refund that was already
-- accounted for). Every refund_capture() call that isn't itself an
-- idempotent no-op writes exactly one row here, in the SAME transaction as
-- any wallet_entries/wallet_balances/payments change — outcome records which
-- of the three things happened.
CREATE TABLE IF NOT EXISTS billing_refunds (
    id                 UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    payment_id         UUID        NOT NULL REFERENCES payments(id),
    provider_refund_id TEXT        NOT NULL UNIQUE,
    amount_cents       BIGINT      NOT NULL CHECK (amount_cents > 0),
    credits_deducted   BIGINT      NOT NULL DEFAULT 0 CHECK (credits_deducted >= 0),
    outcome            VARCHAR(24) NOT NULL
        CHECK (outcome IN ('applied', 'skipped_insufficient', 'skipped_zero_credits')),
    note               TEXT        NOT NULL DEFAULT '',
    created_at         TIMESTAMPTZ NOT NULL DEFAULT now()
);
CREATE INDEX IF NOT EXISTS idx_billing_refunds_payment ON billing_refunds (payment_id);
