# Content Module Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Backport the posts/uploads/sitemap feature set from the production fork `moveeeax/tarassov.me` into the template, gated by `content.enabled` (default off), with articles served as raw Markdown at `/posts/{slug}`.

**Architecture:** Port battle-tested fork code (domain, repository, controllers, admin SPA pages) with tarassov-specifics stripped; add a new slim public controller for Markdown delivery + sitemap; wire the template's gates (Endpoints.hpp ↔ openapi.yaml drift, public_paths, coverage floor).

**Tech Stack:** C++20/Drogon (header-only .hpp convention), pqxx, GTest (unit/integration/e2e buckets), React+TS SPA, drogon ADD_METHOD_TO routing.

**Spec:** `docs/superpowers/specs/2026-08-08-content-module-design.md`

## Global Constraints

- Branch: `feat/content-module` (exists, carries the spec). Plain conventional commits, NO AI-attribution trailers (owner's standing rule).
- **Fork source checkout:** `/private/tmp/claude-501/-Users-moveeeax-Public-github-cpp-rapid-rest-template/19700f83-6daa-4c9f-b311-b4a323345000/scratchpad/tarassov.me` (referred to as `$FORK` below). If missing: `git clone --depth 1 https://github.com/moveeeax/tarassov.me.git` to that path.
- Public article URL is `/posts/{slug}` — the fork uses `/blog/{1}`; every ported occurrence of the public path MUST become `/posts/...`. Admin CRUD routes stay `/api/v1/posts...` exactly as in the fork.
- Every route of this module is guarded per-request: when `content.enabled` is false the handler replies `ErrorResponse::not_found("content")` (registration via ADD_METHOD_TO is static — routes cannot be conditionally registered).
- `Endpoints.hpp` and `docs/openapi.yaml` must change in the SAME commit as controller route changes (CI `openapi-drift` gate).
- Header-only convention: all new C++ code in `.hpp` under `src/`, matching the fork's structure (it followed the same convention).
- Tests run in Docker: `make test-quick` for unit+integration iteration, `make test` for the full suite incl. e2e. Coverage floor 54% must hold.
- Follow existing comment density/idiom; keep the fork's comments where code is ported verbatim.

---

### Task 1: config flag + migration

**Files:**
- Modify: `config/config.json` (add `content` section after `jobs`)
- Modify: `src/core/Core.hpp` (config surface — see Step 2)
- Create: `migrations/006_add_posts.sql`
- Test: `tests/unit/test_config.cpp` (or the existing config test file — find with `grep -rl 'jobs.enabled' tests/unit/`)

**Interfaces:**
- Produces: `Core::content_enabled()` — static accessor reading `content.enabled` config key / `CONTENT_ENABLED` env, default `false`. Tasks 3–5 call exactly `Core::content_enabled()`.
- Produces: table `posts` with columns `id, slug (citext unique), title, summary, body, status, topic, tags, published_at, created_at, updated_at` — Task 2's repository relies on these names.

- [ ] **Step 1: Write the failing unit test** — in the existing config-defaults test file add:

```cpp
TEST(ConfigDefaults, ContentDisabledByDefault) {
    // minimal_config() carries no "content" section — the flag must default off.
    auto cfg = TestHelpers::parse_minimal_config();  // match the file's existing fixture helper
    EXPECT_FALSE(cfg.get<bool>("content.enabled", "CONTENT_ENABLED", false));
}
```

Adapt fixture calls to the file's existing pattern (read the file first; if config tests live under a different structure, place the test consistently). Run: `make test-quick` → expect FAIL only if helper names are wrong; the assertion itself passes trivially once the accessor exists — the REAL failing target is Step 3's accessor compile-time absence if referenced. Keep the test minimal.

- [ ] **Step 2: Add config + accessor.** In `config/config.json` after the `jobs` block:

```json
  "content": {
    "enabled": "${CONTENT_ENABLED:-false}"
  },
```

In `src/core/Core.hpp`, next to the jobs gating (near line ~609, pattern `cfg.get<bool>("jobs.enabled", "JOBS_ENABLED", false)`), add a public static accessor following the file's existing style:

```cpp
    /// Content module (posts/uploads/sitemap) master switch. Routes are
    /// statically registered, so handlers consult this per-request and 404
    /// when the module is off.
    static bool content_enabled() {
        return config().get<bool>("content.enabled", "CONTENT_ENABLED", false);
    }
```

(Adapt `config()` to however Core exposes its Config instance — read the surrounding accessors and copy their access pattern exactly.)

- [ ] **Step 3: Write the migration** — `migrations/006_add_posts.sql`, the fork's 006+007 squashed to final schema (leetcode backfills dropped):

```sql
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
```

- [ ] **Step 4: Run unit + integration buckets** — `make test-quick`. Expected: all green (MigrationsTest picks up 006 automatically and stays idempotent-green).

- [ ] **Step 5: Commit**

```bash
git add config/config.json src/core/Core.hpp migrations/006_add_posts.sql tests/unit/<file>
git commit -m "feat(content): add content.enabled flag and posts migration"
```

---

### Task 2: Post domain + repository

**Files:**
- Create: `src/domain/Post.hpp` (port `$FORK/src/domain/Post.hpp`)
- Create: `src/repositories/PostRepository.hpp` (port `$FORK/src/repositories/PostRepository.hpp`)
- Modify: `src/utils/Strings.hpp` — diff against `$FORK/src/utils/Strings.hpp`, take ONLY helpers the two files above reference (e.g. slugify/xml-escape); skip anything unreferenced.
- Test: `tests/unit/test_post_repository.cpp` (new; port relevant cases from `$FORK/tests` if present — check `ls $FORK/tests/unit | grep -i post` — else write fresh)

**Interfaces:**
- Consumes: `posts` table schema from Task 1.
- Produces: `Domain::Post` (fields mirroring the table incl. `topic`, `tags` split/joined in domain layer) and `Repositories::PostRepository` with the fork's method set (create/update/delete/get by id/slug, list admin, list published, used by Tasks 3–4). Copy signatures EXACTLY from the fork — later tasks port controllers that call them verbatim.

- [ ] **Step 1: Port the two files.** Copy from `$FORK`, then strip tarassov-specifics: grep each file for `leetcode|tarassov` and remove those branches/comments (the topic/tags mechanics stay — they're in the squashed migration). Keep fork comments otherwise.
- [ ] **Step 2: Write failing unit tests** — minimum set (adapt to the repo's unit-test DB fixture pattern; look at an existing repository unit/integration test for the harness):

```cpp
TEST_F(PostRepositoryTest, CreateGeneratesUniqueSlugFromTitle)   // "Hello World!" → "hello-world"; second insert → "hello-world-2" (match fork's dedup scheme — read its slug code first)
TEST_F(PostRepositoryTest, ListPublishedExcludesDrafts)
TEST_F(PostRepositoryTest, PublishSetsPublishedAtOnce)           // re-publishing must not move published_at
TEST_F(PostRepositoryTest, TagsRoundTripCommaJoined)             // {"k8s","cpp"} → stored "k8s,cpp" → parsed back
```

If the repo's repository tests live in the integration bucket (they need Postgres), put these there instead — follow where `UserRepository` tests live. Run: `make test-quick` → new tests FAIL/compile-error before wiring, PASS after.
- [ ] **Step 3: Make them pass** (fix port glitches; no behavior changes vs fork unless a test exposes a real bug — then note it in the report).
- [ ] **Step 4: Commit** — `feat(content): port Post domain and PostRepository`

---

### Task 3: PostsController (admin CRUD + preview tokens + public JSON)

**Files:**
- Create: `src/api/PostsController.hpp` (port `$FORK/src/api/PostsController.hpp`)
- Modify: `src/api/Endpoints.hpp` (add the routes — copy the fork's entries)
- Modify: `docs/openapi.yaml` (add paths — copy from `$FORK/docs/openapi.yaml` the `/api/v1/posts*` + `/api/v1/public/posts*` path objects, adjusting only names the strip removed)
- Modify: `src/api/Api.hpp` if controller registration lists controllers explicitly (check how JobsController is included/registered and mirror it)
- Test: `tests/integration/test_posts_api.cpp` (new)

**Interfaces:**
- Consumes: `PostRepository` (Task 2 signatures), `Core::content_enabled()` (Task 1).
- Produces: routes `POST/GET /api/v1/posts`, `GET/PATCH/DELETE /api/v1/posts/{1}`, `POST /api/v1/posts/{1}/preview-token`, `GET /api/v1/public/posts`, `GET /api/v1/public/posts/{1}`. Preview-token flow exactly as fork. Task 4 reuses the same guard helper; Task 6's SPA calls these routes.

- [ ] **Step 1: Port the controller.** At the TOP of every handler body insert the module guard (this exact pattern, matching template error helpers):

```cpp
        if (!Core::content_enabled()) {
            callback(ErrorResponse::not_found("content"));
            return;
        }
```

Strip `leetcode|tarassov` remnants. Admin routes keep their fork auth guards (`API_REQUIRE_ADMIN` or the fork's equivalent — keep identical semantics using this template's macros; diff the fork's guard macro names against `src/api/JobsController.hpp` and use the template's).
- [ ] **Step 2: Endpoints.hpp + openapi.yaml in the same edit session.** Copy fork entries; verify with the repo's drift check (find the exact command: `grep -rn 'openapi' Makefile | head` — run that target). Expected: drift check green.
- [ ] **Step 3: Write failing integration tests** — cases:

```cpp
TEST_F(PostsApiTest, AdminCrudRoundtrip)            // create→get→patch→delete as admin
TEST_F(PostsApiTest, NonAdminGets403OnCreate)
TEST_F(PostsApiTest, PublicListShowsOnlyPublished)
TEST_F(PostsApiTest, PreviewTokenRevealsDraft)      // draft invisible publicly; with token — visible (copy fork test if exists)
TEST_F(PostsApiTest, AllRoutes404WhenContentDisabled) // fixture boots with content.enabled=false
```

Follow the existing integration-bucket controller-test fixture (direct controller invocation — see how existing api tests drive handlers). Run, make pass.
- [ ] **Step 4: Commit** — `feat(content): port PostsController with admin CRUD, preview tokens, public JSON`

---

### Task 4: Markdown delivery + sitemap controller

**Files:**
- Create: `src/api/ContentPagesController.hpp` (NEW slim controller — derive from `$FORK/src/api/PublicPagesController.hpp` by DELETING all HTML/PageTemplates rendering; no PageTemplates.hpp port)
- Modify: `src/api/Endpoints.hpp`, `docs/openapi.yaml` (routes `/posts/{slug}`, `/sitemap.xml`)
- Modify: `config/config.json` — `api.public_paths` gains `,/posts/*,/sitemap.xml,/api/v1/public/posts,/api/v1/public/posts/*`
- Test: e2e cases in `tests/e2e/test_http_e2e.cpp` + integration `tests/integration/test_content_pages.cpp`

**Interfaces:**
- Consumes: `PostRepository::find_published_by_slug` / list-published (Task 2 exact names), `Core::content_enabled()`.
- Produces: `GET /posts/{1}` → 200 `text/markdown; charset=utf-8`, body = post's raw `body` field prefixed by a `# {title}\n\n` heading line; 404 (module off, unknown slug, draft) → `text/markdown` body `# 404\n\nNot found.\n`. `GET /sitemap.xml` → `application/xml`: `<urlset>` with `app.base_url` root `<url>` + one `<url>` per published post (`{base_url}/posts/{slug}`, `<lastmod>` = published_at date ISO-8601, XML-escaped via the Strings helper from Task 2).

- [ ] **Step 1: Write the controller** — structure:

```cpp
class ContentPagesController : public drogon::HttpController<ContentPagesController> {
  public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(ContentPagesController::post_markdown, "/posts/{1}", drogon::Get);
    ADD_METHOD_TO(ContentPagesController::sitemap, "/sitemap.xml", drogon::Get);
    METHOD_LIST_END
    // handlers: guard → repository → build response with explicit
    // setContentTypeString("text/markdown; charset=utf-8") — Drogon has no
    // markdown CT enum. Sitemap: CT_APPLICATION_XML... (check fork's sitemap
    // handler for the exact Drogon calls and reuse them).
};
```

Port the fork's sitemap-building loop verbatim (minus HTML page URLs it added), swapping `/blog/` → `/posts/`.
- [ ] **Step 2: public_paths + Endpoints + openapi + drift check** (same commit).
- [ ] **Step 3: Failing tests, then green.** Integration: markdown body exact-match for a seeded published post; 404 for draft. E2E additions (in the existing HttpE2E suite):

```cpp
TEST(HttpE2E, PostMarkdownServedOverWire)   // seed via admin API → GET /posts/{slug} → 200, Content-Type text/markdown, body starts "# "
TEST(HttpE2E, SitemapListsPublishedPost)    // GET /sitemap.xml → 200, contains <loc>.../posts/{slug}</loc>
```

NOTE: the e2e environment boots with a config the suite controls (`tests/e2e/test_http_e2e.cpp` SetUp builds cfg json) — set `cfg["content"]["enabled"] = true` there; ALSO extend its `api.public_paths` the same way as config.json. Run full `make test`.
- [ ] **Step 4: Commit** — `feat(content): serve posts as markdown and generate sitemap.xml`

---

### Task 5: UploadController

**Files:**
- Create: `src/api/UploadController.hpp` (port `$FORK/src/api/UploadController.hpp`)
- Modify: `src/api/Endpoints.hpp`, `docs/openapi.yaml`
- Test: `tests/integration/test_uploads_api.cpp`

**Interfaces:**
- Consumes: existing `src/storage/Storage.hpp` backend (already in template — but FIRST diff it against `$FORK/src/storage/Storage.hpp`; if the fork extended it, port the delta too and say so in the report), `Core::content_enabled()` guard.
- Produces: `POST/GET /api/v1/admin/uploads`, `DELETE /api/v1/admin/uploads/{1}` (admin-only, guarded by content flag like Task 3).

- [ ] **Step 1: Diff Storage.hpp fork-vs-template; port delta if any.**
- [ ] **Step 2: Port controller with the content guard + admin guard; Endpoints/openapi; drift check.**
- [ ] **Step 3: Failing integration tests → green:** upload roundtrip against the local storage backend (`storage.backend=local` in the test config — no MinIO dependency; the storage layer abstracts it), list contains the key, delete removes, 404-when-disabled.
- [ ] **Step 4: Commit** — `feat(content): port admin uploads API`

---

### Task 6: admin SPA — Posts & Media tiles

**Files:**
- Create: `frontend/src/pages/admin/Posts.tsx`, `frontend/src/pages/admin/Media.tsx`, `frontend/src/components/Modal.tsx`, `frontend/src/components/TokenConfirmCard.tsx` (all ported from `$FORK/frontend/src/...`)
- Modify: `frontend/src/pages/admin/Dashboard.tsx` (two new Cards in the existing `grid gap-4 sm:grid-cols-2 lg:grid-cols-3` — copy an existing Card block's exact structure, titles "Posts" and "Media")
- Modify: the SPA router (find it: `grep -rn 'admin/jobs\|Jobs' frontend/src/App.tsx frontend/src/main.tsx frontend/src/router* 2>/dev/null` — mirror how admin/Jobs route is declared) — add `/admin/posts`, `/admin/media`
- Modify: `frontend/src/lib/api/schema.gen.ts` — REGENERATE, don't hand-edit: find the generator (`grep -rn 'schema.gen' frontend/package.json`) and run it against the updated `docs/openapi.yaml`
- Test: `frontend` bucket via the CI-equivalent command (`grep -n 'frontend' .github/workflows/ci.yml` shows the steps — typically npm ci/test/build)

**Interfaces:**
- Consumes: routes from Tasks 3 & 5 (the ported pages already call them — verify paths survived the port unchanged: admin CRUD `/api/v1/posts...`, uploads `/api/v1/admin/uploads...`).
- Produces: two dashboard tiles navigating to `/admin/posts` and `/admin/media`.

- [ ] **Step 1: Port the four files; strip `tarassov|leetcode|topic-cloud` UI remnants** (keep topic/tags plain inputs — they're generic now).
- [ ] **Step 2: Router + Dashboard tiles + schema regen.**
- [ ] **Step 3: Run frontend lint/tests/build exactly as ci.yml's frontend job does.** Expected: green, build succeeds.
- [ ] **Step 4: Commit** — `feat(content): admin Posts and Media tiles in the SPA dashboard`

---

### Task 7: full-suite verification + PR

- [ ] **Step 1:** `make test` (all three buckets) — green; coverage floor holds (54%). If coverage dips, add the cheapest meaningful unit tests in the new module — do NOT lower the floor.
- [ ] **Step 2:** push branch, `gh pr create` — title `feat: content module — posts with markdown delivery, sitemap, admin uploads`; body references the spec, states the flag default (off), notes squashed migration provenance and `/blog/→/posts/` rename.
- [ ] **Step 3:** CI green (use the poll-loop pattern, not `gh run watch`) → merge per repo convention (squash).
