# PayPal Billing Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Users top up an internal credit wallet with PayPal; an admin controls the rate and sale packages. Taking money and crediting the wallet only — spending credits is a later wave.

**Architecture:** New `billing` module in this repo (cybercapybara/site), gated by `billing.enabled`. Append-only ledger (`wallet_entries`) is the source of truth, `wallet_balances` is a same-transaction cache. PayPal REST v2 Orders; both the return-flow capture and the webhook funnel into ONE idempotent crediting function guarded by a UNIQUE capture id.

**Tech Stack:** C++20/Drogon header-only `.hpp`, pqxx, libcurl + nlohmann::json (already used by `S3Storage`/`Mailer`), GTest buckets, React+TS SPA, Helm.

**Spec:** `docs/superpowers/specs/2026-08-09-billing-paypal-design.md`

## Global Constraints

- **This repository only** (cybercapybara/site). Nothing here is backported to the upstream template.
- Branch: `feat/billing-paypal` (exists, carries the spec). Plain conventional commits, **NO AI-attribution trailers**.
- **Builds and tests run ONLY in GitHub CI** (owner policy). No local `docker`, `make test*`, `npm`. Static verification + the two bash scripts (`./scripts/check-openapi-drift.sh`, `./scripts/check-routes-registered.sh`) is what an implementer runs locally. No background/Monitor waits.
- `Endpoints.hpp` + `docs/openapi.yaml` change in the SAME commit as routes (drift gate).
- Money is integers everywhere: `amount_cents`, `credits` are `BIGINT`/`int64_t`. No floating point in the money path — not even for display maths.
- Real PayPal credentials never land in git. Tests stub the client; CI makes no network calls to PayPal.
- Existing idioms to follow: `API_REQUIRE_ADMIN(req, callback)` / `API_REQUIRE_PRINCIPAL(req, callback, principal)` from `src/api/Guards.hpp`; `with_repo_errors(callback, "op", fn)` from `src/api/HandlerSupport.hpp`; `Response::ok` / `ErrorResponse::*` from `src/utils/ErrorResponse.hpp`; `Database::get().execute_write([](auto& txn){...})`; repositories extend `CrudBase`.
- Every new handler starts with the module guard, mirroring `PostsController`:
  ```cpp
  if (!Core::billing_enabled()) { callback(ErrorResponse::not_found("billing")); return; }
  ```

---

### Task 0: publish this repo's own images

**Why first:** the site currently runs the upstream template's images. Billing code lives only here, so nothing after this task can ever reach production until the repo builds itself.

**Files:**
- Modify: `.github/workflows/release.yml` (line ~32, `IMAGE_NAME`)
- Modify: `README.md` (image references, if any name the template's registry)

**Interfaces:**
- Produces: images `ghcr.io/cybercapybara/site{,-worker,-frontend}:<version>` — every later deploy step uses these.

- [ ] **Step 1: Retarget the image namespace**

```yaml
  IMAGE_NAME: ghcr.io/cybercapybara/site
```

Check the rest of the file for `moveeeax`/`cpp-rapid-rest-template` occurrences in prose comments and update them to match (the login step already uses `${{ github.actor }}` + `GITHUB_TOKEN`, which works for the org).

- [ ] **Step 2: Commit**

```bash
git add .github/workflows/release.yml README.md
git commit -m "ci(release): publish site images under ghcr.io/cybercapybara/site"
```

- [ ] **Step 3: Tag a baseline release and confirm it lands**

```bash
git tag v0.1.0 && git push origin v0.1.0
gh run list --workflow release.yml --limit 1   # poll until completed/success
```

Then verify all three packages exist (`gh api /orgs/cybercapybara/packages?package_type=container --jq '.[].name'`) and **make them public** — org container packages default to private and Kubernetes pulls anonymously. If the API refuses (GitHub restricts visibility changes to the UI for container packages), report back: the owner does it with three clicks.

- [ ] **Step 4: Point the cluster at the new images**

Edit `~/Public/cybercapybara/cluster/bootstrap/site/{api,worker,frontend}-values.yaml`: `image.repository` → `ghcr.io/cybercapybara/site{,-worker,-frontend}`, `tag: "0.1.0"`. Then `helm upgrade` all three releases (`--wait`) and confirm pods are Running and `https://cybercapybara.kz/healthz` returns `ok`. This proves the self-built images work BEFORE any billing code exists — if something breaks here it is the image pipeline, not billing.

---

### Task 1: config flag + migration

**Files:**
- Modify: `config/config.json` (a `billing` block after `content`)
- Modify: `src/core/Core.hpp` (accessor next to `content_enabled()`)
- Create: `migrations/007_billing.sql`
- Test: add a case to the existing config test file (find it: `grep -rl 'content.enabled\|CONTENT_ENABLED' tests/unit/`)

**Interfaces:**
- Produces: `Core::billing_enabled()` — free inline function in `namespace Core` (match how `content_enabled()` is written), reads `billing.enabled` / `BILLING_ENABLED`, default `false`.
- Produces: tables `wallet_entries`, `wallet_balances`, `payments`, `billing_packages` — Task 2's repositories depend on these exact column names.

- [ ] **Step 1: Write the migration** — `migrations/007_billing.sql`:

```sql
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
```

- [ ] **Step 2: Config block** — in `config/config.json`, after the `content` block:

```json
  "billing": {
    "enabled": "${BILLING_ENABLED:-false}",
    "provider": "paypal",
    "currency": "USD",
    "credits_per_unit": 100,
    "min_amount_cents": 100,
    "max_amount_cents": 100000,
    "paypal": {
      "environment": "${PAYPAL_ENV:-sandbox}",
      "client_id": "${PAYPAL_CLIENT_ID:-}",
      "client_secret": "${PAYPAL_CLIENT_SECRET:-}",
      "webhook_id": "${PAYPAL_WEBHOOK_ID:-}",
      "return_url": "${PAYPAL_RETURN_URL:-}",
      "cancel_url": "${PAYPAL_CANCEL_URL:-}"
    }
  },
```

- [ ] **Step 3: Accessor** — in `src/core/Core.hpp`, beside `content_enabled()`, copying its exact shape (including the `Config::is_initialized()` guard):

```cpp
    /// Billing module (wallet top-ups) master switch. Routes are statically
    /// registered, so handlers consult this per-request and 404 when off.
    inline bool billing_enabled() {
        if (!Config::is_initialized())
            return false;
        return Config::get().get<bool>("billing.enabled", "BILLING_ENABLED", false);
    }
```

- [ ] **Step 4: Unit test** — mirror the existing `ContentDisabledByDefault` case in the same file and with the same fixture:

```cpp
TEST_F(ConfigTest, BillingDisabledByDefault) {
    EXPECT_FALSE(Config::get().get<bool>("billing.enabled", "BILLING_ENABLED", false));
}
```

- [ ] **Step 5: Commit** — `feat(billing): add billing.enabled flag and wallet/payments migration`

---

### Task 2: domain, repositories and the crediting service

This is the money core. Everything else calls into it.

**Files:**
- Create: `src/domain/Billing.hpp` (`Domain::Package`, `Domain::Payment`, `Domain::WalletEntry`, enums as string constants)
- Create: `src/repositories/BillingRepository.hpp` (packages + payments CRUD via `CrudBase` where it fits)
- Create: `src/billing/Wallet.hpp` (the crediting service — the ONLY place that writes the ledger)
- Test: `tests/integration/test_wallet.cpp`

**Interfaces:**
- Produces (Tasks 4–6 call exactly these):
  ```cpp
  namespace Billing {
  struct CreditResult { bool credited; std::int64_t balance; std::string payment_id; };

  // Idempotent. If capture_id already recorded → returns credited=false with the
  // existing balance and touches nothing. Otherwise, in ONE transaction:
  // insert wallet_entries, upsert wallet_balances, set payments.status='captured'
  // and payments.provider_capture_id.
  CreditResult credit_capture(const std::string& provider_order_id,
                              const std::string& provider_capture_id,
                              std::int64_t captured_amount_cents);

  // Negative 'refund' entry, idempotent on (payment, capture) pair.
  CreditResult refund_capture(const std::string& provider_capture_id,
                              std::int64_t refunded_amount_cents);

  // Admin manual entry; delta may be negative. Always audited by the caller.
  CreditResult adjust(const std::string& user_id, std::int64_t delta_credits,
                      const std::string& note, const std::string& admin_id);

  std::int64_t balance_of(const std::string& user_id);
  std::vector<Domain::WalletEntry> history(const std::string& user_id, int limit, int offset);
  }  // namespace Billing
  ```
- `credit_capture` MUST verify `captured_amount_cents == payments.amount_cents`; on mismatch it sets `status='failed'`, writes `failure_reason`, logs at error level and returns `credited=false` WITHOUT touching the ledger.

- [ ] **Step 1: Write the failing integration tests first** — `tests/integration/test_wallet.cpp`, using the `TestHelpers::CoreBackedTest` fixture pattern (copy the setup from `tests/integration/test_posts_api.cpp`, including config overrides with `billing.enabled=true`):

```cpp
TEST_F(WalletTest, CreditCaptureCreditsOnceAndUpdatesBalance)      // credit → balance == credits_expected, one ledger row
TEST_F(WalletTest, CreditCaptureIsIdempotentOnCaptureId)           // same capture id twice → one row, credited=false the 2nd time
TEST_F(WalletTest, CreditCaptureRefusesAmountMismatch)             // captured != ordered → no ledger row, payment failed, reason set
TEST_F(WalletTest, RefundWritesNegativeEntry)                      // balance drops by the same amount, kind='refund'
TEST_F(WalletTest, AdjustWritesAuditedEntryAndMovesBalance)        // signed delta, note + created_by persisted
TEST_F(WalletTest, LedgerSumEqualsCachedBalanceAfterMixedTraffic)  // THE invariant: SUM(delta) == wallet_balances.credits
```

- [ ] **Step 2: Implement domain + repository + `Wallet.hpp`.** Key implementation notes:
  - All writes go through a single `Database::get().execute_write([&](auto& txn){ ... })` — the ledger insert, the balance upsert and the payment update are one atomic unit.
  - Balance upsert: `INSERT INTO wallet_balances (user_id, credits) VALUES ($1,$2) ON CONFLICT (user_id) DO UPDATE SET credits = wallet_balances.credits + EXCLUDED.credits, updated_at = now()`.
  - Idempotency: attempt `UPDATE payments SET provider_capture_id=$1, status='captured' WHERE provider_order_id=$2 AND provider_capture_id IS NULL RETURNING id, user_id, credits_expected, amount_cents`. Zero rows returned means "already captured (or unknown order)" → re-read and return the existing state instead of crediting. A concurrent duplicate loses the race on the UNIQUE index and lands in the same branch.
  - Never let a negative balance happen from a refund of a spent wallet: the `CHECK (credits >= 0)` will reject it — catch that specific failure, mark the payment `refunded` anyway and log an error for manual reconciliation (a refund must never be silently dropped).
- [ ] **Step 3: Static compile-risk pass** (signatures vs `CrudBase`, `Database`, `ErrorResponse`), then commit — `feat(billing): wallet ledger, payments repository and idempotent crediting`

---

### Task 3: PayPal REST client

**Files:**
- Create: `src/billing/PayPalClient.hpp`
- Test: `tests/unit/test_paypal_client.cpp` (pure parsing/URL tests — NO network)

**Interfaces:**
- Produces:
  ```cpp
  namespace Billing {
  struct PayPalOrder { std::string order_id; std::string approve_url; };
  struct PayPalCapture { std::string capture_id; std::int64_t amount_cents; std::string currency; };

  class PayPalClient {
  public:
      static PayPalClient& get();                       // configured from Config at Core init
      PayPalOrder create_order(std::int64_t amount_cents, const std::string& currency,
                               const std::string& reference,
                               const std::string& return_url, const std::string& cancel_url);
      PayPalCapture capture_order(const std::string& order_id);
      bool verify_webhook_signature(const std::map<std::string, std::string>& headers,
                                    const std::string& raw_body);
      // exposed for tests — pure, no I/O:
      static PayPalCapture parse_capture_response(const std::string& json_body);
      static std::string   base_url(const std::string& environment);  // sandbox|live
  };
  }  // namespace Billing
  ```
- Throws `std::runtime_error` on transport/non-2xx; callers wrap in `with_repo_errors`.

- [ ] **Step 1: Failing unit tests** (no network — feed canned PayPal JSON):

```cpp
TEST(PayPalClient, BaseUrlSelectsSandboxAndLive)                 // sandbox → api-m.sandbox.paypal.com, live → api-m.paypal.com, unknown → sandbox
TEST(PayPalClient, ParseCaptureExtractsIdAndIntegerCents)        // "12.34" USD → 1234 cents, no float rounding drift
TEST(PayPalClient, ParseCaptureRejectsMalformedBody)             // throws, doesn't return a zero-amount capture
```

The cents parser must convert the decimal string WITHOUT going through `double` (split on '.', pad/truncate to 2 digits) — a `stod` round-trip is exactly how money bugs start.

- [ ] **Step 2: Implement**, mirroring `S3Storage`'s libcurl usage in `src/storage/Storage.hpp` (same timeouts, same error mapping, same header handling). OAuth2: `POST /v1/oauth2/token` with basic auth, cache the token in-process until `expires_in - 60s`. Webhook verification: `POST /v1/notifications/verify-webhook-signature` with the raw body and the `paypal-*` headers, returning `verification_status == "SUCCESS"`.
- [ ] **Step 3: Commit** — `feat(billing): PayPal REST v2 client (orders, capture, webhook verification)`

---

### Task 4: user-facing billing API

**Files:**
- Create: `src/api/BillingController.hpp`
- Modify: `src/api/Api.hpp`, `src/api/Endpoints.hpp`, `docs/openapi.yaml`
- Test: `tests/integration/test_billing_api.cpp`

**Interfaces:**
- Consumes: `Billing::credit_capture`, `Billing::balance_of`, `Billing::history` (Task 2), `PayPalClient` (Task 3), `Core::billing_enabled()` (Task 1).
- Produces: `GET /api/v1/billing/packages`, `GET /api/v1/billing/wallet`, `POST /api/v1/billing/topup`, `POST /api/v1/billing/capture`.

- [ ] **Step 1: Failing integration tests** (PayPal stubbed — inject a fake client or seed the payment row directly, whichever the implementation allows; no network):

```cpp
TEST_F(BillingApiTest, TopupWithPackageFreezesRateAndCredits)   // payment row: credits_expected == package.credits, rate_snapshot == current rate
TEST_F(BillingApiTest, TopupWithCustomAmountRespectsMinMax)     // below min / above max → 400 with a clear message
TEST_F(BillingApiTest, TopupIgnoresClientSuppliedCredits)       // body carrying "credits": 999999 must not influence the row
TEST_F(BillingApiTest, CaptureCreditsWalletOnce)                // balance moves once; second call returns the same balance
TEST_F(BillingApiTest, WalletShowsOwnBalanceAndHistoryOnly)     // user A cannot see user B's wallet (no id parameter is accepted at all)
TEST_F(BillingApiTest, AllRoutes404WhenBillingDisabled)
```

- [ ] **Step 2: Implement the controller.** Every handler: module guard → `API_REQUIRE_PRINCIPAL(req, callback, principal)` → `with_repo_errors`. `topup` computes credits server-side: package → `package.credits`; custom amount → `amount_cents * credits_per_unit / 100` with integer maths, rejecting anything outside `[min,max]`. The response returns the approve URL only; the client never sees or supplies credit counts.
- [ ] **Step 3: Endpoints + openapi in the same commit; run both drift scripts.**
- [ ] **Step 4: Commit** — `feat(billing): user top-up, capture and wallet endpoints`

---

### Task 5: PayPal webhook

**Files:**
- Modify: `src/api/BillingController.hpp` (webhook handler), `src/api/Endpoints.hpp`, `docs/openapi.yaml`
- Modify: `src/utils/Strings.hpp` (`kDefaultPublicPathsCsv` gains `/api/v1/billing/paypal/webhook`) and `config/config.json` `api.public_paths` — **both**, the config file overrides the default (a lesson from the content module: adding only one leaves production 401ing).
- Modify: `src/utils/Strings.hpp` `kDefaultProtectedPathsCsv` — the webhook joins the strict rate-limit tier.
- Test: extend `tests/integration/test_billing_api.cpp`

**Interfaces:**
- Consumes: `PayPalClient::verify_webhook_signature`, `Billing::credit_capture`, `Billing::refund_capture`.
- Produces: `POST /api/v1/billing/paypal/webhook` — public, CSRF-exempt, always answers 200 to PayPal once the signature is valid (even for events it ignores), 401 on a bad signature.

- [ ] **Step 1: Failing tests**

```cpp
TEST_F(BillingApiTest, WebhookRejectsInvalidSignature)            // 401, nothing credited
TEST_F(BillingApiTest, WebhookCreditsWhenUserNeverReturned)       // capture-completed event alone credits the wallet
TEST_F(BillingApiTest, WebhookAfterCaptureIsNoop)                 // already captured → 200, exactly one ledger row
TEST_F(BillingApiTest, WebhookRefundWritesNegativeEntry)
TEST_F(BillingApiTest, WebhookIgnoresUnrelatedEventTypes)         // 200, no ledger change
```

- [ ] **Step 2: Implement.** Verify the signature against the RAW body BEFORE parsing it as trusted JSON. Do not require an authenticated principal. Log every received event id at info level (dedupe/debug trail).
- [ ] **Step 3: CSRF exemption check** — confirm how the CSRF middleware decides (read `src/api/Middleware.hpp`); if it keys off public paths, nothing extra is needed; otherwise exempt the webhook path explicitly and say so in the report.
- [ ] **Step 4: Drift scripts, commit** — `feat(billing): PayPal webhook with signature verification`

---

### Task 6: admin billing API

**Files:**
- Create: `src/api/AdminBillingController.hpp`
- Modify: `src/api/Api.hpp`, `src/api/Endpoints.hpp`, `docs/openapi.yaml`
- Test: `tests/integration/test_admin_billing_api.cpp`

**Interfaces:**
- Produces: `GET /api/v1/admin/billing/payments`, packages CRUD under `/api/v1/admin/billing/packages[/{id}]`, `GET|PUT /api/v1/admin/billing/settings`, `POST /api/v1/admin/billing/users/{id}/adjust`.
- Settings PUT persists rate/min/max; storage mechanism: a single-row `billing_settings` table is NOT in the migration — use the existing runtime-config mechanism if one exists (`grep -rn "settings" src/repositories/`), otherwise add the table in THIS task's own migration `008_billing_settings.sql` and say so in the report.

- [ ] **Step 1: Failing tests**

```cpp
TEST_F(AdminBillingTest, NonAdminGets403OnEveryRoute)
TEST_F(AdminBillingTest, PackageCrudRoundtrip)
TEST_F(AdminBillingTest, SettingsUpdateChangesComputedCredits)   // change rate → a new topup computes with the NEW rate, an in-flight payment keeps its snapshot
TEST_F(AdminBillingTest, AdjustRequiresNoteAndWritesAudit)       // empty note → 400; success → wallet_entries.created_by == admin, audit_log row exists
TEST_F(AdminBillingTest, PaymentsListFiltersByStatusAndUser)
```

- [ ] **Step 2: Implement** — `API_REQUIRE_ADMIN` on every handler plus the module guard; every mutation writes to the existing audit log (see how `AdminController` does it).
- [ ] **Step 3: Drift scripts, commit** — `feat(billing): admin packages, settings, payments and manual adjustments`

---

### Task 7: deployment wiring

**Files:**
- Modify: `helm/cpp-api/values.yaml` (a `billing` block), `helm/cpp-api/templates/configmap.yaml`, `helm/cpp-api/templates/secret.yaml`, `helm/cpp-api/templates/deployment.yaml`
- Modify: `docker/docker-compose.yml` (app service env), `docs/CONFIG.md`

**Interfaces:**
- Produces: `BILLING_ENABLED`, `PAYPAL_ENV`, `PAYPAL_CLIENT_ID`, `PAYPAL_RETURN_URL`, `PAYPAL_CANCEL_URL` as plain env; `PAYPAL_CLIENT_SECRET` and `PAYPAL_WEBHOOK_ID` from the chart Secret — mirroring exactly how `MAIL_SMTP_PASSWORD` and `S3_SECRET_KEY` are wired (read those three files first and copy the pattern; the content module's storage wiring is the closest example).

- [ ] **Step 1: Wire values → configmap → secret → deployment env.**
- [ ] **Step 2: Verify rendering** — `helm template t helm/cpp-api --set billing.enabled=true --set billing.paypal.clientSecret=x --set billing.paypal.webhookId=y` renders the env vars and the Secret keys; `--set billing.enabled=false` (default) must not render the secret entries.
- [ ] **Step 3: Document** the new keys in `docs/CONFIG.md`, including the note that enabling billing in a deployment that overrides `API_PUBLIC_PATHS` requires adding the webhook path.
- [ ] **Step 4: Commit** — `feat(billing): chart and compose wiring for the billing module`

---

### Task 8: SPA — user wallet page and admin tile

**Files:**
- Create: `frontend/src/pages/Billing.tsx` (balance, packages, custom amount, history), `frontend/src/pages/BillingReturn.tsx`, `frontend/src/pages/BillingCancel.tsx`, `frontend/src/pages/admin/Billing.tsx`
- Modify: `frontend/src/routes/manifest.tsx` (routes `/billing`, `/billing/return`, `/billing/cancel`, `/admin/billing`; admin entry WITHOUT `navLabel` — tiles only, matching the Posts/Media decision), `frontend/src/pages/admin/Dashboard.tsx` (a **Billing tile** in the existing grid), `frontend/src/lib/api/queryKeys.ts`
- **Do NOT touch** `frontend/src/lib/api/schema.gen.ts` — it is generated; the autofix workflow regenerates it in CI after the openapi change.

**Interfaces:**
- Consumes: every route from Tasks 4 and 6 (verify each call site against `src/api/Endpoints.hpp` and list them in the report as a table).

- [ ] **Step 1: Build the pages**, following `frontend/src/pages/admin/Posts.tsx` for data-fetching idioms (`usePagedQuery`, `useApiMutation`, `useErrorToast`) and `Jobs.tsx` for table/badge patterns. Money display: format from integer cents/credits (`(cents/100).toFixed(2)` at the RENDER boundary only — never for arithmetic).
- [ ] **Step 2: Return-flow page** calls `POST /api/v1/billing/capture` with the `token` query parameter PayPal appends, shows the new balance on success, and — on failure — tells the user the payment may still complete via the webhook (never "your money is lost").
- [ ] **Step 3: Commit** — `feat(billing): wallet page, PayPal return flow and admin billing tile`

---

### Task 9: PR, CI, sandbox verification, deploy

- [ ] **Step 1:** push, `gh pr create` referencing the spec; note in the body that billing ships **disabled by default** and that this repo now builds its own images (Task 0).
- [ ] **Step 2:** CI green (poll loop, not `gh run watch`). If `lint-format`/`frontend` fail on formatting or the generated schema, dispatch the repo's `autofix` workflow against the branch and re-check.
- [ ] **Step 3:** merge (squash), tag the next version, wait for the release run, deploy to fsn1 (`helm upgrade` all three releases).
- [ ] **Step 4: Sandbox smoke on the live site** — ask the owner for sandbox `PAYPAL_CLIENT_ID` / `PAYPAL_CLIENT_SECRET` / `PAYPAL_WEBHOOK_ID`, put the two secrets into the site values (chmod 600, never in git), enable `billing.enabled=true`, then run one real sandbox purchase end to end: package → PayPal approve → return → capture → balance moved by exactly the package credits → exactly one `wallet_entries` row → webhook arrival is a no-op. Verify the webhook is reachable (`POST /api/v1/billing/paypal/webhook` returns 401 without a signature, not 404/401-from-auth).
- [ ] **Step 5:** report the sandbox results and what remains for going live (webhook registered in the PayPal app, live credentials, `PAYPAL_ENV=live`).
