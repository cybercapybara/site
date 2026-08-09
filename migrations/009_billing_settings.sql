-- Migration 009: billing_settings — single-row, admin-editable runtime
-- config for the billing module (credits-per-unit rate + custom top-up
-- bounds).
--
-- Applied in numeric order on boot. The MigrationRunner wraps this file in ONE
-- transaction under an advisory lock — do NOT add BEGIN/COMMIT. Idempotent DDL.
--
-- Why a table instead of reusing an existing mechanism: `grep -rn "settings"
-- src/repositories/ src/domain/ src/core/` (Task 6) found no existing
-- runtime-config store anywhere in this codebase — every admin-editable value
-- to date lives in config/config.json + env var overrides (Config::get<T>),
-- which requires a redeploy to change. Task 6's admin settings endpoint
-- (GET|PUT /api/v1/admin/billing/settings) needs a rate/bounds change to take
-- effect immediately for the next top-up, so it needs a real row to UPDATE.
--
-- Single-row guard: `id` is a CHECK(id = 1) SMALLINT PK — a second INSERT
-- always collides on the primary key, so UPDATE is structurally the only way
-- to change these values (same shape as any other single-row settings table;
-- this codebase's closest precedent, migration 004's admin permission
-- sentinel, is a one-time data migration rather than a live-edited row, so
-- this pattern is new here but standard practice elsewhere).
--
-- The seeded defaults (100 / 100 / 100000) are copied verbatim from
-- config/config.json's billing.credits_per_unit / min_amount_cents /
-- max_amount_cents — so a fresh database's admin-settings round-trip starts
-- from the exact same numbers BillingController::billing_limits() already
-- read from config before this migration existed. See the Task 6 report for
-- why this doesn't change any existing Task 4 test's observed behavior.
CREATE TABLE IF NOT EXISTS billing_settings (
    id               SMALLINT    PRIMARY KEY DEFAULT 1 CHECK (id = 1),
    credits_per_unit BIGINT      NOT NULL CHECK (credits_per_unit > 0),
    min_amount_cents BIGINT      NOT NULL CHECK (min_amount_cents > 0),
    max_amount_cents BIGINT      NOT NULL CHECK (max_amount_cents >= min_amount_cents),
    updated_at       TIMESTAMPTZ NOT NULL DEFAULT now()
);

INSERT INTO billing_settings (id, credits_per_unit, min_amount_cents, max_amount_cents)
VALUES (1, 100, 100, 100000)
ON CONFLICT (id) DO NOTHING;

DROP TRIGGER IF EXISTS billing_settings_touch_updated_at ON billing_settings;
CREATE TRIGGER billing_settings_touch_updated_at
    BEFORE UPDATE ON billing_settings
    FOR EACH ROW EXECUTE FUNCTION touch_updated_at();
