-- Migration 006: add_posts
--
-- Migrations are applied in numeric order on app boot (or via
-- RUN_MIGRATIONS_ONLY=1). The MigrationRunner wraps this file in ONE
-- transaction (under an advisory lock) together with the schema_migrations
-- bookkeeping. Do NOT add BEGIN/COMMIT. Prefer idempotent DDL.

-- Content-module posts. Authored via the admin API; public endpoints only
-- expose rows with status = 'published'. Applied unconditionally — with
-- content.enabled=false the table simply stays empty.
CREATE TABLE IF NOT EXISTS posts (
    id           UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    slug         CITEXT UNIQUE NOT NULL,            -- URL key (citext from migration 001)
    title        TEXT        NOT NULL,
    summary      TEXT        NOT NULL DEFAULT '',   -- list/teaser blurb
    body         TEXT        NOT NULL DEFAULT '',   -- Markdown source
    status       VARCHAR(16) NOT NULL DEFAULT 'draft',  -- draft | published
    topic        TEXT        NOT NULL DEFAULT '',   -- section label above the title
    tags         TEXT        NOT NULL DEFAULT '',   -- comma-joined keyword tags
    published_at TIMESTAMPTZ,                       -- set when first published
    created_at   TIMESTAMPTZ NOT NULL DEFAULT now(),
    updated_at   TIMESTAMPTZ NOT NULL DEFAULT now()
);

-- Public listing: newest published first. Partial index keeps drafts out.
CREATE INDEX IF NOT EXISTS idx_posts_published
    ON posts (published_at DESC) WHERE status = 'published';

DROP TRIGGER IF EXISTS posts_touch_updated_at ON posts;
CREATE TRIGGER posts_touch_updated_at
    BEFORE UPDATE ON posts
    FOR EACH ROW EXECUTE FUNCTION touch_updated_at();
