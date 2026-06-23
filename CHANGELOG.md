# Changelog

All notable changes to this project are documented here.
Format: [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).
Versioning: [SemVer](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

## [1.2.0] — 2026-06-23

Security hardening, saturation observability, and template/DX fixes. No
breaking API changes — all new config keys have defaults / env fallbacks, so
existing config files load unchanged.

### Added
- **Saturation metrics**: `db_pool_active_connections` + `db_pool_size`
  (labeled by pool — the answer to "is the pool about to time out on
  acquire?") and `jobs_queue_depth` (the waiting queue — a *leading* indicator,
  unlike the lagging `jobs_dlq_depth`). New Prometheus alerts
  `DbPoolSaturationHigh` and `JobsQueueBacklog` with RUNBOOK entries
  (`#dbpool`, `#queuebacklog`) and the matching SLO rows.
- **Opt-in CSRF** double-submit guard (`security.csrf.*`, off by default):
  server middleware verifying a token header against a non-HttpOnly cookie on
  cookie-authenticated mutations, with the SPA client wired to echo it.
- **Stricter rate-limit tier** config for the auth surface
  (`rate_limit.protected_requests` / `protected_window_sec` /
  `protected_paths`).
- `make` front doors: `make new-resource`, `make new-job`, `make init`.
- Coverage gate: `make coverage` now fails under `COVERAGE_MIN`% line coverage
  (default 40, a regression floor — override per-run).
- `scripts/init-project.sh` gains an optional `[domain]` argument to rebrand
  the host alongside the project name.

### Changed
- `with_repo_errors` is decoupled from the User/Role domain: repository
  exceptions now derive from generic `Repositories::NotFoundError` /
  `ConflictError` bases (`repositories/RepoErrors.hpp`), so the shared
  controller plumbing no longer includes the demo repositories and a forked
  domain's own exceptions map to the right status automatically.
- `new-resource.sh` scaffolds the repository on `CrudBase` (the base whose
  whole purpose is `find` / `list` / `count`) instead of hand-rolling them.
- The scaffolders are now discoverable where forkers read — README "first
  steps", `docs/INDEX.md`, and the Make-targets table — and `new-job.sh` is
  executable.
- `init-project.sh` verifies the rename itself (an independent broad scan that
  exits non-zero listing any survivor) instead of printing a grep for you to
  run.

### Fixed
- **Single-source version**: `CMakeLists.txt project() VERSION` was left at
  `1.0.0` through the 1.1.0 release, so the binary mis-reported its version;
  it now tracks the release.
- Dead scaffolding instructions removed: the generated test + `new-resource.sh`
  trailer + `docs/CONVENTIONS.md` no longer point at a non-existent
  "INTEGRATION_FILTER / 5 places" bucket registration (buckets are classified
  by directory).
- `init-project.sh` no longer leaves the template name behind in the vcpkg
  manifest (`vcpkg.json` was excluded by an over-broad `./vcpkg*` filter),
  `.env.*` variants, and helm `NOTES.txt` — nor the author's `security@` /
  demo host in a fork.

### Security
- **The public auth/account surface is now rate-limited.** `api.public_paths`
  exempted login / register / refresh / password-reset from *both* auth and the
  limiter, leaving them open to credential brute-force and mail-bombing. A
  second, stricter per-IP tier (separate `rl:auth:` namespace) re-arms them; the
  production profile enables it by default (10 req/60 s). Health/metrics/static
  stay unthrottled. **Behavior change on upgrade:** a burst against the auth
  endpoints now returns 429 — tune via `RATE_LIMIT_PROTECTED_REQUESTS`.
- **Baseline HTTP security headers** on every response — `X-Content-Type-Options:
  nosniff`, `X-Frame-Options: DENY`, `Referrer-Policy: no-referrer`, a
  locked-down CSP, and opt-in HSTS (`security.hsts`, on in the production
  profile) — set both in the app and at the nginx edge for the SPA.

## [1.1.0] — 2026-06-23

Pre-release hardening + a public demo. No breaking API changes.

### Added
- Public demo environment at `*.demo.tarassov.me` — `helm/cpp-env/values-demo.yaml`
  + `scripts/deploy-demo.sh` (external-dns + cert-manager, Mailpit/Jaeger UIs),
  with a periodic reset CronJob that wipes and reseeds the data.
- `THIRD_PARTY_NOTICES.md` (dependency licenses + flask-base attribution),
  `docs/TESTING.md` (what the suite covers and doesn't), `docs/BENCHMARKS.md`
  (how to measure latency/footprint), `CODE_OF_CONDUCT.md`, and GitHub issue
  templates.
- Working dark-mode toggle and a real favicon/brand in the SPA.

### Changed / Fixed
- **Security:** constant-time bearer-token compare; the production auth-guard is
  now actually armed (`APP_ENV` wired through config + Helm); the cpp-api chart
  defaults to `auth.mode=jwt` so a bare install can't ship a public API.
- **Fork experience:** `make quickstart` / `up` build the fork's own code instead
  of pulling the upstream image; neutral registry default; CODEOWNERS and
  Prometheus runbook links no longer hardcode the author's identity;
  `init-project.sh` rebrands the GHCR namespace.
- **Ops:** the backup CronJob verifies the dump (`gunzip -t` + size guard) instead
  of silently shipping a truncated one, with failure/staleness alerts;
  `seccompProfile` on all pods; single-replica PodDisruptionBudgets no longer
  wedge node drains; `make coverage` runs every test bucket.
- **CI:** Trivy fails on HIGH as well as CRITICAL; all GitHub Actions pinned to
  SHAs; the tag-release image job promotes the natively-built per-arch images
  into the `:vX.Y.Z` manifest instead of a QEMU rebuild (which segfaulted
  emulating amd64).
- **Docs:** corrected the README inventory, a non-existent module in an ADR, a
  migration-skeleton trap, dead clone/CI links, and a CHANGELOG contradiction.

## [1.0.0] — 2026-06-10

First tagged release. Highlights of the pre-release hardening pass:

- HTTP end-to-end test layer (real Drogon server + client) covering the
  middleware chain: auth gate, cookie sessions on the wire, refresh
  rotation/revocation, idempotency replay, permission bitmask, tracing.
- Frontend session refresh (401 → /api/auth/refresh → retry) + first
  frontend unit tests; committed package-lock.json for reproducible builds.
- Single-source version (CMake → version.hpp), production config profile
  with `make prod-check`, Prometheus alert rules, Renovate, prebuilt
  builder-image cache (`make warm-cache`).
- Test-bucket filters by explicit suite names; deflaked tamper/expiry
  tests; precompiled headers.

### Added
- Account / Auth / Admin feature surface modeled after flask-base
  (see `docs/PATTERNS-FROM-FLASK-BASE.md` for the file-level mapping):
  - `migrations/001_users_and_roles.sql` — users + roles + permission
    bitmask. Bits match flask-base's `Permission` class
    (`GENERAL=0x01`, `ADMINISTER=0xff`).
  - `src/domain/{User,Role}.hpp` + `src/repositories/*` — typed DTO +
    SQL-only repositories with `DuplicateEmail` / `UserNotFound`
    typed exceptions.
  - `src/security/Password.hpp` — argon2id password hashing via
    libsodium (replaces flask-base's PBKDF2).
  - `src/security/Tokens.hpp` — HMAC-signed timed link tokens with
    per-purpose key derivation (replaces flask-base's
    `URLSafeTimedSerializer`).
  - `src/security/Auth.hpp` — JWT in HttpOnly cookies (`__Host-access`
    + `__Host-refresh`), refresh-token JTI revocation in Redis,
    `current_user_can(req, perm)` / `require_admin(req)` helpers.
  - `src/api/AuthController.hpp` — `/api/auth/{register,login,logout,refresh,me}`.
  - `src/api/AccountController.hpp` —
    `/api/account/{confirm-resend,confirm/{token},reset-password-request,
    reset-password/{token},change-email-request,change-email/{token},
    change-password}`.
  - `src/api/AdminController.hpp` — `/api/admin/{users,users/{id},
    invite,roles}` with self-protection guards.
  - `src/email/Mailer.hpp` — SMTP outbound via libcurl.
  - `src/email/Templates.hpp` + `templates/email/*.{html,txt}` — inja
    rendering for confirm / reset_password / change_email / invite.
  - `src/email/AccountEmails.hpp` — token-issuing senders shared by
    Auth + Account + Admin controllers.
  - CLI ops: `--setup-dev`, `--create-admin EMAIL [PASS]`,
    `--seed-fake [N]`. flask-base parity: `manage.py setup_general /
    add_fake_data`.
  - Mailpit dev sidecar (`docker-compose.yml`) on :8025 (UI) / :1025
    (SMTP). No TLS, no auth — local-only catcher.
- React SPA under `frontend/`:
  - Vite + React 18 + TypeScript + Tailwind + shadcn/ui (Radix
    primitives) + TanStack Query + react-hook-form + zod + Zustand.
  - openapi-fetch typed client driven by `npm run gen:api` against
    `docs/openapi.yaml`. Stub committed so a fresh clone compiles.
  - Pages: Home, About, Login, Register, CheckEmail, ConfirmEmail,
    Unconfirmed, Profile, ChangePassword, ChangeEmail, RequestReset,
    ResetPassword, Admin (Dashboard, Users, UserDetail, InviteUser).
  - `<ProtectedRoute>` with `requireConfirmed` and
    `requirePermission={Permission.Administer}` guards; `<Layout>`
    runs `useMe()` once so every page has a fresh principal.
  - `frontend/Dockerfile` (multi-stage Node→nginx) +
    `frontend/nginx.conf` proxy `/api/* -> app:8080`. New
    docker-compose service exposed on host :3001.
- Make targets: `frontend-{install,dev,build,lint,format,typecheck,
  test,gen-api,up,image}`.
- CI: `frontend` job (typecheck + lint + production build) +
  `openapi-drift`, `clang-tidy`, `sanitizers` jobs (paritet with
  GitLab CI).
- ADR-0005 documenting the SPA split.
- `docs/PATTERNS-FROM-FLASK-BASE.md` — authoritative list of what we
  lift from flask-base, what we change, and what we don't lift.
- `--run-migrations` CLI flag on the app binary (mirrors
  `RUN_MIGRATIONS_ONLY=1`; convenient for native dev / `make migrate-local`).
- New Make targets: `test-watch`, `migrate-local`, `ci-local`, `helm-lint`,
  `new-endpoint`, `new-migration`, `tail-trace TID=…`, `env-check`.
- `scripts/env-check.sh` — reports `${VAR}` placeholders in
  `config/config.json` that are unset and have no default (multi-token
  aware, handles `database.primary` correctly).
- `scripts/new-endpoint.sh --with-test` and `--patch-openapi` flags —
  scaffold a `tests/api/test_<name>.cpp` smoke test and patch
  `docs/openapi.yaml` in place instead of just printing a stub.
- `scripts/init-project.sh` idempotency guard — refuses to re-run with a
  different name unless `--force` is passed.
- `.editorconfig` — whitespace/EOL rules for editors that don't read
  `.clang-format`.
- `envrc.sample` — direnv template covering `VCPKG_ROOT`,
  `VCPKG_BINARY_SOURCES`, `TEST_PG_HOST`, `DATABASE_PASSWORD`.
- `docs/INDEX.md` — one-line navigator across docs / ADRs / scripts /
  configs.
- CMake: CTest labels (`unit` / `integration`) so `ctest -L unit` works
  without re-deriving the gtest filter; `ENABLE_WERROR` option for CI.
- GitHub Actions parity with GitLab: `openapi-drift`, `clang-tidy`, and
  `sanitizers` jobs.
- Repository pattern scaffolding in `docs/EXAMPLES.md`: typed DTOs with
  `nlohmann::to_json`, repositories that own all SQL and raise typed
  `DuplicateKey` / `UserNotFound` exceptions, thin controllers that
  translate those to HTTP status codes.
- `LOG_FORMAT=json` (default `text`) — emits one JSON record per line with
  JSON-escaped message via a custom spdlog flag; includes `service`,
  `thread`, `level`, ISO8601 timestamp for Loki/ELK/Datadog pipelines.
- CLI ops flags on the app binary:
  - `--print-routes` prints the endpoint table and exits (no subsystems).
  - `--dump-config` resolves config (JSON + env) and prints it as JSON.
  - `--verify-migrations` reports pending migrations, exits 1 if any pending.
- `MigrationRunner::list_pending()` — read-only migration diff helper.
- `Jobs::init_blocking_client()` for in-process tests so BRPOP has a socket
  timeout budget that matches its block timeout.
- `docker-compose.yml` `test-redis` service in the `test` profile — isolates
  the test suite from the dev-stack worker that would otherwise BRPOP
  test-submitted jobs off the shared queue.
- `DATABASE_REPLICA_URLS` passthrough in the worker service so the worker
  can route reads to the replica under `up-everything`.
- `.github/workflows/ci.yml` (full build + test + clang-format + shellcheck)
  and `.github/workflows/release.yml` (multi-arch GHCR push on `v*` tags).
- `migrations/README.md` starter doc — naming, `--verify-migrations`, ops.
- Env-var interpolation (`${VAR}` / `${VAR:-default}`) in config JSON values.
- `Config::require<T>()` — throws on missing required config.
- JWT (HS256) auth middleware with exp/nbf/iss/aud validation + RBAC helpers.
- Static bearer-token mode for dev convenience (`auth.mode=bearer`).
- Redis-backed fixed-window rate limiter with per-IP / per-user scope.
- Idempotency-Key middleware for POST/PUT/PATCH/DELETE.
- Dead-letter queue for jobs (`jobs:dlq:*`) with GET `/api/jobs/dlq` and
  POST `/api/jobs/dlq/{id}/requeue`.
- Generic retry-with-backoff utility (`Retry::run`) with pqxx / redis-plus-plus
  transient-error classifiers; applied to `Database::execute_read/write`.
- W3C Trace Context parsing and `X-Request-Id` / `traceparent` response
  headers; per-request trace-id attached to request attributes.
- Validation helpers (`Api::Validation::Errors`, `require`, `string_length`,
  `regex_match`, `int_range`, `one_of`, `email`, `uuid`).
- Graceful shutdown: readiness flips to 503 on SIGTERM; Drogon drain after
  configurable pre-stop delay; worker bounded drain.
- CMake option `ENABLE_SANITIZERS` (ASan + UBSan).
- `.clang-tidy` baseline + CI lint job.
- Sanitizer CI job (unit subset).
- Helm: `preStop` hook, `terminationGracePeriodSeconds`, external-secrets
  ExternalSecret skeleton, PrometheusRule with baseline SLO alerts.
- OpenAPI 3.1 spec under `docs/openapi.yaml`.
- Governance: `CODEOWNERS`, `SECURITY.md`, `CONTRIBUTING.md`, MR/PR templates.

### Changed
- `scripts/check-openapi-drift.sh` now compares `(method, path)` tuples,
  not just paths — catches verb-only changes (`GET → POST` on the same
  route) the previous diff missed.
- CMake test glob uses `CONFIGURE_DEPENDS` so a freshly-added
  `tests/<bucket>/test_*.cpp` is picked up on the next build without a
  manual `cmake --preset dev` reconfigure.
- Added stricter compile warnings by default: `-Wshadow`,
  `-Wnon-virtual-dtor`, `-Wold-style-cast`, `-Wcast-align`,
  `-Woverloaded-virtual`, `-Wnull-dereference`, `-Wdouble-promotion`,
  `-Wformat=2`. Debug build also gets `-fno-omit-frame-pointer`.
- `.devcontainer/devcontainer.json` `postCreateCommand` now calls
  `make compile-commands` instead of duplicating the cmake invocation.
- `Jobs::fail()` after `max_retries` now transitions jobs to status `dead`
  and pushes to the DLQ instead of a terminal `failed` state.
- `config/*.json` no longer contain plaintext passwords — use `${VAR}`
  placeholders.

### Removed
- Demo `users` / `posts` / `events` controllers, their repositories, domain
  types, tests, and migrations. The real auth / account / admin / audit domain
  stays; only the throwaway CRUD demo was removed, and the worked CRUD pattern
  moved to `docs/EXAMPLES.md` so `src/` stays free of example noise when you fork.
- `ensure_test_seed()` helper (no longer needed without the demo schema).

### Security
- Default `auth.mode` is `none` for local development, but the service
  refuses to start in `jwt` mode without a `JWT_SECRET` set.
- OpenSSL linked explicitly for HMAC-SHA256 (JWT signature) and SHA-256
  (Idempotency-Key body hash); constant-time compare via `CRYPTO_memcmp`.

[Unreleased]: https://gitlab.com/tarassov.me/cpp-rapid-rest-template/-/compare/v1.2.0...master
[1.2.0]: https://gitlab.com/tarassov.me/cpp-rapid-rest-template/-/compare/v1.1.0...v1.2.0
[1.1.0]: https://gitlab.com/tarassov.me/cpp-rapid-rest-template/-/compare/v1.0.0...v1.1.0
