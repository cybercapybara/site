# Changelog

All notable changes to this project are documented here.
Format: [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).
Versioning: [SemVer](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

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
  types, tests, and migrations. The template now ships infrastructure-only
  (Health + Jobs); the worked CRUD pattern moved to `docs/EXAMPLES.md` so
  `src/` stays free of example noise when you fork.
- `ensure_test_seed()` helper (no longer needed without the demo schema).

### Security
- Default `auth.mode` is `none` for local development, but the service
  refuses to start in `jwt` mode without a `JWT_SECRET` set.
- OpenSSL linked explicitly for HMAC-SHA256 (JWT signature) and SHA-256
  (Idempotency-Key body hash); constant-time compare via `CRYPTO_memcmp`.

[Unreleased]: https://example.com/compare/master...HEAD
