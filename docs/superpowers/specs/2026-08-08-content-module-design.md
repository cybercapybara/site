# Content module — posts, markdown delivery, sitemap, uploads (backport)

**Date:** 2026-08-08
**Source:** backport from the production fork `moveeeax/tarassov.me` (its code
grew out of this template and is idiomatic to it), trimmed by owner decisions.

## Owner decisions (fixed)

- Target: **this template** (forks pick it up later; cybercapybara/site next).
- Scope: admin + API only. Articles are delivered **as raw Markdown**; no SSR
  HTML pages in this wave (fork's `PageTemplates`, `templates/pages/*.html`
  and public React blog pages are explicitly NOT ported).
- Public article URL: **`/posts/{slug}`** (not `/blog/...`).
- Module is gated by config flag, off by default.
- Admin SPA: Posts and Media appear **as tiles in the existing `/admin`
  dashboard grid** — not as detached routes.
- Contact-form feature: not ported (out of scope).

## Config

- New `content` section: `content.enabled` (default **false**), env
  `CONTENT_ENABLED` — same pattern as `jobs.enabled`. The flag gates
  registration/behaviour of every route below; with the flag off they all
  return 404 — **except `/sitemap.xml`**, which is a site-level artifact and
  always serves, degrading to a root-only sitemap (no `<url>` entries) rather
  than 404ing. The guard skips the posts query entirely in that case (not
  just the response), which is what keeps this 500-safe on a deploy that
  hasn't run the posts migration yet. Implemented and covered by
  `tests/integration/test_content_pages.cpp`.
- Migration `006_add_posts.sql` — single clean migration with the FINAL
  schema (fork's 006 posts + 007 topic tags squashed; leetcode backfills
  008/009 dropped). Always applied; an empty table is harmless.

## API surface

| Route | Auth | Behaviour |
|---|---|---|
| `POST /api/v1/posts` | admin | create (fork's CRUD as-is, content stored as Markdown) |
| `GET /api/v1/posts` | admin | list incl. drafts |
| `GET /api/v1/posts/{id}` | admin | read |
| `PATCH /api/v1/posts/{id}` | admin | update |
| `DELETE /api/v1/posts/{id}` | admin | delete |
| `POST /api/v1/posts/{id}/preview-token` | admin | fork's preview-token flow |
| `GET /api/v1/public/posts` | public | JSON list of published posts (meta: slug, title, dates, tags) |
| `GET /api/v1/public/posts/{slug}` | public | JSON meta of one post (fork behaviour) |
| `GET /posts/{slug}` | public | **raw Markdown**, `text/markdown; charset=utf-8`; 404 body is plain markdown text |
| `GET /sitemap.xml` | public | static root entries + `/posts/{slug}` with `lastmod` from posts |
| `POST /api/v1/admin/uploads` | admin | upload to S3 via existing storage backend |
| `GET /api/v1/admin/uploads` | admin | list |
| `DELETE /api/v1/admin/uploads/{key}` | admin | delete |

- `api.public_paths` gains: `/posts/*`, `/sitemap.xml`,
  `/api/v1/public/posts`, `/api/v1/public/posts/*`.
- `Endpoints.hpp` and `docs/openapi.yaml` updated in the same commit as the
  controllers — the repo's drift gate enforces this.

## Code to port (from the fork, genericized)

- `src/api/PostsController.hpp`, `src/repositories/PostRepository.hpp`,
  `src/domain/Post.hpp` — as-is minus tarassov-specific topic handling.
- `src/api/UploadController.hpp` — as-is (storage backend already exists in
  the template).
- New slim public controller for `/posts/{slug}` (markdown) + `/sitemap.xml`
  — derived from fork's `PublicPagesController` with the HTML rendering
  removed.
- `src/utils/Strings.hpp` slug/helpers diff — take the needed helpers.
- Frontend: `pages/admin/Posts.tsx`, `pages/admin/Media.tsx`,
  `components/Modal.tsx`, `components/TokenConfirmCard.tsx`, router/API-schema
  diffs; dashboard grid gains Posts and Media tiles.

## Testing

- Unit: PostRepository (slug generation, publish/draft filtering), sitemap
  builder (lastmod, escaping).
- Integration: admin CRUD + preview-token round-trip; uploads against MinIO
  sidecar if present, else skipped.
- E2E (real HTTP): `/posts/{slug}` returns `text/markdown` with exact body;
  `/sitemap.xml` valid XML listing the published post; public JSON list;
  **with `content.enabled=false` every route above returns 404**; drafts are
  invisible publicly but reachable via preview token.
- Frontend: existing frontend test setup covers new components at its usual
  level (no new frameworks).

## Non-goals

SSR HTML pages, public React blog UI, contact form, RSS, tag pages,
upstream-sync mechanism. All possible later waves.

## Delivery

One branch off master, spec + implementation, PR through the normal gates
(coverage floor 54% must hold — the new module ships with its tests).
Plain conventional commits, no AI-attribution trailers.
