-- Migration 007: billing (credit wallet + PayPal payments)
--
-- Applied in numeric order on boot. The MigrationRunner wraps this file in ONE
-- transaction under an advisory lock — do NOT add BEGIN/COMMIT. Idempotent DDL.
--
-- Money invariants:
--   * every amount is an INTEGER (cents / credits) — no floating point;
--   * wallet_entries is APPEND-ONLY and is the source of truth;
--   * wallet_balances is a cache written in the same transaction as its entry;
--   * payments.provider_capture_id UNIQUE is the structural guard against
--     double crediting when the return-flow capture races the webhook.

CREATE TABLE IF NOT EXISTS billing_packages (
    id           UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    title        TEXT        NOT NULL,
    amount_cents BIGINT      NOT NULL CHECK (amount_cents > 0),
    credits      BIGINT      NOT NULL CHECK (credits > 0),
    active       BOOLEAN     NOT NULL DEFAULT true,
    sort         INTEGER     NOT NULL DEFAULT 0,
    created_at   TIMESTAMPTZ NOT NULL DEFAULT now(),
    updated_at   TIMESTAMPTZ NOT NULL DEFAULT now()
);

CREATE TABLE IF NOT EXISTS payments (
    id                  UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    user_id             UUID        NOT NULL REFERENCES users(id) ON DELETE RESTRICT,
    provider            TEXT        NOT NULL DEFAULT 'paypal',
    provider_order_id   TEXT        NOT NULL UNIQUE,
    provider_capture_id TEXT        UNIQUE,           -- set once, guards double credit
    amount_cents        BIGINT      NOT NULL CHECK (amount_cents > 0),
    currency            CHAR(3)     NOT NULL DEFAULT 'USD',
    credits_expected    BIGINT      NOT NULL CHECK (credits_expected > 0),
    rate_snapshot       BIGINT      NOT NULL,          -- credits per 100 cents at creation
    package_id          UUID        REFERENCES billing_packages(id) ON DELETE SET NULL,
    status              VARCHAR(16) NOT NULL DEFAULT 'created',  -- created|approved|captured|failed|refunded
    failure_reason      TEXT,
    created_at          TIMESTAMPTZ NOT NULL DEFAULT now(),
    updated_at          TIMESTAMPTZ NOT NULL DEFAULT now()
);
CREATE INDEX IF NOT EXISTS idx_payments_user    ON payments (user_id, created_at DESC);
CREATE INDEX IF NOT EXISTS idx_payments_status  ON payments (status) WHERE status <> 'captured';

CREATE TABLE IF NOT EXISTS wallet_entries (
    id            UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    user_id       UUID        NOT NULL REFERENCES users(id) ON DELETE RESTRICT,
    delta_credits BIGINT      NOT NULL CHECK (delta_credits <> 0),
    kind          VARCHAR(16) NOT NULL,               -- topup|spend|adjustment|refund
    reference     TEXT        NOT NULL DEFAULT '',    -- payment id / service id
    note          TEXT        NOT NULL DEFAULT '',
    created_by    UUID        REFERENCES users(id) ON DELETE SET NULL,  -- admin, for adjustments
    created_at    TIMESTAMPTZ NOT NULL DEFAULT now()
);
CREATE INDEX IF NOT EXISTS idx_wallet_entries_user ON wallet_entries (user_id, created_at DESC);

CREATE TABLE IF NOT EXISTS wallet_balances (
    user_id    UUID PRIMARY KEY REFERENCES users(id) ON DELETE RESTRICT,
    credits    BIGINT      NOT NULL DEFAULT 0 CHECK (credits >= 0),
    updated_at TIMESTAMPTZ NOT NULL DEFAULT now()
);

DROP TRIGGER IF EXISTS billing_packages_touch_updated_at ON billing_packages;
CREATE TRIGGER billing_packages_touch_updated_at
    BEFORE UPDATE ON billing_packages
    FOR EACH ROW EXECUTE FUNCTION touch_updated_at();

DROP TRIGGER IF EXISTS payments_touch_updated_at ON payments;
CREATE TRIGGER payments_touch_updated_at
    BEFORE UPDATE ON payments
    FOR EACH ROW EXECUTE FUNCTION touch_updated_at();
