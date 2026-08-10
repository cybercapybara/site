# Billing Emails + Admin Dashboard Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Transactional email on every billing status change (receipt, refund/chargeback, failed, admin adjustment) + an admin business dashboard on `/admin/billing` with day/week/month metrics.

**Architecture:** Billing emails render templates at enqueue time and ship via the existing `email.send` job (no new job kind); dispatched AFTER the money transaction commits, best-effort, de-duped by the crediting result so return-flow + webhook produce exactly one email. Dashboard = a new admin metrics endpoint (SQL aggregates) + a recharts Overview tab.

**Tech Stack:** C++20/Drogon header-only, pqxx, the existing Email/Jobs stack, GTest buckets, React+TS SPA, recharts (new dep).

**Spec:** `docs/superpowers/specs/2026-08-10-billing-emails-dashboard-design.md`

## Global Constraints

- **This repo only** (cybercapybara/site). Branch: `feat/billing-emails-dashboard` (exists, carries the spec). Plain conventional commits, **NO AI-attribution trailers**.
- **Builds and tests run ONLY in GitHub CI** (owner policy). No local docker/make/npm. Static verification + the two bash drift scripts. NO background/Monitor waits in subagents.
- `Endpoints.hpp` + `docs/openapi.yaml` change in the SAME commit as routes.
- Money is integers (cents/credits); format to dollars only at the render boundary.
- **Money-safety (non-negotiable):** no email enqueue inside `Database::execute_write`; every billing email is best-effort and NEVER throws into the money path; a mail failure must not affect a credit/refund.
- Idioms to match: `src/email/AccountEmails.hpp` (`Email::Templates::render_pair(name, ctx)`, `{{ key }}` templates with spaces), `Email::SendEmail::send(to, subject, text, html)` (`src/email/GenericEmail.hpp`), `templates/email/confirm.html|.txt` (visual style, inline CSS, no external assets), `API_REQUIRE_ADMIN` + `Core::billing_enabled()` guards, `with_repo_errors`, `frontend/src/pages/admin/Billing.tsx` tab structure, `frontend/src/lib/api/queryKeys.ts`.
- Autofix workflow handles clang-format + schema.gen.ts regen after a push; don't hand-fix those.
- Money core signatures (call verbatim): see `.superpowers/sdd/2026-08-09-billing-paypal/task-2-report.md` / `task-4-report.md` — `credit_capture` returns `{credited, balance, payment_id}`; `refund_capture` similar; `adjust(user_id, delta, note, admin_id)`.

---

### Task 1: BillingEmails module + templates + wire receipt/refund/failed

**Files:**
- Create: `src/email/BillingEmails.hpp`
- Create: `templates/email/billing_receipt.html`, `.txt`, `billing_refund.html`, `.txt`, `billing_failed.html`, `.txt`
- Modify: `src/api/BillingController.hpp` (capture path → receipt; webhook path → receipt/refund/failed), `src/billing/Wallet.hpp` ONLY if the credited/failed signal isn't already returned (it is — do not change the money txn)
- Test: `tests/integration/test_billing_emails.cpp`

**Interfaces:**
- Produces: `namespace Email::BillingEmails { void receipt(const Domain::User&, ...ctx); void refund(...); void failed(...); }` — each renders `render_pair` + `SendEmail::send`, best-effort, never throws. Exact ctx keys per the spec's table. Task 2 adds `adjustment(...)` to the same namespace.

- [ ] **Step 1: Write the templates** — mirror `confirm.html`/`confirm.txt` header/footer and inline style. `billing_receipt.html` shows a receipt block: `{{ package_title }}`, `{{ amount }} {{ currency }}`, `{{ credits }}` credits, new balance `{{ new_balance }}`, payment `{{ payment_id }}`, `{{ date }}`. `billing_refund.html`: `{{ kind_label }}` (Refund/Reversal), `{{ amount }} {{ currency }}`, `{{ credits_deducted }}` credits removed, `{{ new_balance }}`. `billing_failed.html`: MUST say "you were not charged", `{{ reason }}`. Each `.txt` is the plain-text twin. Use `{{ key }}` with spaces (matches the loader).

- [ ] **Step 2: Write `BillingEmails.hpp`** — mirror AccountEmails' render+send but via the best-effort ad-hoc path:

```cpp
namespace Email::BillingEmails {
inline void send_rendered(const std::string& tmpl, const std::string& subject,
                          const Domain::User& user, nlohmann::json ctx) {
    try {
        ctx["app_name"] = /* Config app.name */;
        ctx["user"]["full_name"] = user.full_name;               // match template var shape
        auto r = Email::Templates::render_pair(tmpl, ctx);
        Email::SendEmail::send(user.email, subject, r.text, r.html);  // enqueues email.send, best-effort
    } catch (const std::exception& e) {
        spdlog::warn("BillingEmails: {} for {} failed: {}", tmpl,
                     Utils::Strings::mask_email(user.email), e.what());
    }
}
inline void receipt(const Domain::User& u, /* amount_cents, currency, credits, new_balance, payment_id, date, package_title */) { ... send_rendered("billing_receipt", "Your top-up receipt", u, ctx); }
inline void refund(const Domain::User& u, /* kind_label, amount_cents, currency, credits_deducted, new_balance, payment_id, date */) { ... }
inline void failed(const Domain::User& u, /* amount_cents, currency, reason, date */) { ... }
}
```

Money formatting (cents → "12.34") stays integer-string based; reuse the frontend's approach conceptually but here just format in C++ (`amount_cents/100` and `amount_cents%100` with zero-pad — NO double).

- [ ] **Step 3: Wire the dispatch points** (in `BillingController.hpp`, AFTER the wallet result returns, OUTSIDE any txn):
  - Capture endpoint + webhook capture-completed: after `credit_capture`, `if (result.credited) { load user via UserRepository::find(payment.user_id); BillingEmails::receipt(user, ...); }` — the `credited` flag guarantees one email across return-flow + webhook.
  - Webhook refund/reversal: after `refund_capture`, send `refund` only when a debit actually happened (the result signals applied vs no-op/skipped — check the exact field in task-2-report; send on applied only).
  - Failed path (amount/currency mismatch marks `failed`): send `failed` once at that transition.
  - User load failure → log + skip, never throw.

- [ ] **Step 4: Integration tests** (`tests/integration/test_billing_emails.cpp`) — the mail path can be observed without SMTP: assert via the Jobs queue (an `email.send` job was enqueued) or a test Mailer seam (check how existing account-email tests observe delivery — `grep -rn 'email.send\|MailerSpy\|enqueued' tests/`). Cases: a real capture enqueues exactly ONE receipt job; a duplicate capture/webhook for the same payment enqueues NO second receipt; a refund enqueues a refund email; an amount-mismatch enqueues a failed email; with `mail.enabled=false` nothing is enqueued. Do NOT assert SMTP.

- [ ] **Step 5: Commit** — `feat(billing): receipt, refund and failed-payment emails`

---

### Task 2: admin adjustment email + notify flag

**Files:**
- Modify: `src/api/AdminBillingController.hpp` (`adjustWallet`: parse optional `notify`, send email), `src/email/BillingEmails.hpp` (add `adjustment(...)`)
- Create: `templates/email/billing_adjustment.html`, `.txt`
- Modify: `docs/openapi.yaml` (adjust request gains `notify`)
- Test: extend `tests/integration/test_admin_billing_api.cpp`

**Interfaces:**
- Consumes: `Billing::adjust` (Task-2 money core), `Email::BillingEmails`.

- [ ] **Step 1: `adjustment` template + helper** — `billing_adjustment.html/.txt`: `{{ delta_credits }}` shown signed (+N / −N), `{{ reason }}`, `{{ new_balance }}`, `{{ date }}`. `BillingEmails::adjustment(user, delta, reason, new_balance)`.
- [ ] **Step 2: `adjustWallet`** — parse optional `notify` (bool, default false) from the body (`note` stays mandatory). After `Billing::adjust` succeeds AND the audit row is written, `if (notify) { load user; BillingEmails::adjustment(...); }`. Order: adjust → audit → email (email best-effort, after).
- [ ] **Step 3: openapi** — add `notify` to the adjust request schema; run both drift scripts.
- [ ] **Step 4: Tests** — adjust with `notify=true` enqueues an adjustment email carrying the reason; `notify=false`/omitted enqueues none; the mandatory-note behavior is unchanged (empty note still 400).
- [ ] **Step 5: Commit** — `feat(billing): optional user notification on admin wallet adjustment`

---

### Task 3: admin metrics endpoint

**Files:**
- Create: `src/repositories/BillingMetricsRepository.hpp` (or methods on BillingRepository)
- Modify: `src/api/AdminBillingController.hpp` (new `metrics` handler), `src/api/Endpoints.hpp`, `docs/openapi.yaml`
- Test: `tests/integration/test_billing_metrics.cpp`

**Interfaces:**
- Produces: `GET /api/v1/admin/billing/metrics?period=day|week|month` (admin + module guarded) returning the JSON in the spec (revenue_cents, payments_count, avg_payment_cents, conversion{created,captured,rate}, refunds_cents/count, outstanding_credits, outstanding_value_cents, series[], top_packages[], top_users[]).

- [ ] **Step 1: Repository aggregates** — one parameterized query per block against the real schema:
  - window from `period`: day→`now()-interval '24 hours'` hourly, week→`'7 days'` daily, month→`'30 days'` daily.
  - revenue/count/avg: `payments WHERE status='captured' AND created_at >= window`.
  - conversion: `count(*) FILTER (WHERE status='captured')` vs `count(*)` over `payments` in-window.
  - refunds: `billing_refunds` sum/count in-window (outcome='applied' only).
  - outstanding: `SUM(credits) FROM wallet_balances` (all-time liability, not windowed); money-equiv = `outstanding × (100 / current rate)` cents... — compute exactly: money_cents = credits * 100 / credits_per_unit (integer). Read the current rate from `billing_settings`.
  - series: `date_trunc(bucket, created_at)` grouped, LEFT JOINed to `generate_series(window_start, now(), bucket_interval)` so empty buckets are 0.
  - top_packages: join `payments`(captured)→`billing_packages`, group, order by revenue, limit 5.
  - top_users: group captured `payments` by user_id, join `users` for email, order by topup credits, limit 5.
- [ ] **Step 2: Handler** — module guard → `API_REQUIRE_ADMIN` → `with_repo_errors`; validate `period` ∈ {day,week,month} (default week; unknown → 400). Assemble the JSON.
- [ ] **Step 3: Endpoints + openapi (same commit), both drift scripts pass.**
- [ ] **Step 4: Failing integration tests then green** — seed a mix of captured/created/failed payments across timestamps + a refund + balances; assert revenue/count/avg/conversion/refunds/outstanding math; assert the series has no gaps (zero buckets present); non-admin → 403; billing disabled → 404; bad period → 400.
- [ ] **Step 5: Commit** — `feat(billing): admin metrics endpoint (revenue, liability, conversion, top lists)`

---

### Task 4: dashboard Overview tab (recharts)

**Files:**
- Modify: `frontend/package.json` (+recharts), `frontend/src/pages/admin/Billing.tsx` (Overview tab, default), `frontend/src/lib/api/queryKeys.ts` (metrics key)
- Create: `frontend/src/pages/admin/billing/Overview.tsx` (or inline in Billing.tsx if the file stays reasonable)
- Do NOT touch `schema.gen.ts`.

**Interfaces:**
- Consumes: `GET /api/v1/admin/billing/metrics` (Task 3).

- [ ] **Step 1: LOAD the `dataviz` skill FIRST** and follow it for the chart + stat-tiles (palette, chart-type, tile spec). This is mandatory for the design.
- [ ] **Step 2: Add recharts** to `package.json` dependencies (a real version, e.g. `^2.x`); the autofix/CI `npm ci` installs it. Confirm it's pure-JS (no native/CDN) so CSP holds.
- [ ] **Step 3: Overview tab** — day/week/month segmented toggle (drives the `period` query param + query key); KPI tile row (Revenue, Payments, Avg, Conversion %, Outstanding liability with credits + money-equiv, Refunds), a recharts area/bar chart from `series` (money from cents at render only), Top-packages and Top-users tables (reuse `DataTable`). Make Overview the default tab. Match `Billing.tsx`/`Jobs.tsx` idioms (usePagedQuery/useApiQuery/useErrorToast — but a 404 when billing disabled must not error-toast; gate like the wallet card did).
- [ ] **Step 4: Commit** — `feat(billing): admin Overview dashboard with revenue chart and KPI tiles`

---

### Task 5: PR, CI, deploy, verify

- [ ] **Step 1:** push the branch; open/mark-ready the PR referencing the spec. If lint-format/frontend-schema fail, dispatch the `autofix` workflow and re-check (poll loop, not `gh run watch`).
- [ ] **Step 2:** CI fully green → merge (squash), tag the next version (v0.3.0), wait for release, deploy all three releases to fsn1 (`helm upgrade`, tag bump).
- [ ] **Step 3: Verify live** — dashboard: `GET /admin/billing` Overview renders with the real sandbox data (3 captured payments, outstanding 12000 credits) and the day/week/month toggle works. Emails: trigger one real receipt by running a fresh sandbox top-up→capture (or an admin adjust with notify=true against a test user whose address you control) and confirm the worker enqueued+delivered the email (check worker logs / the Jobs admin tile). Do NOT spam real users.
- [ ] **Step 4:** report: dashboard URL + a screenshot-equivalent description of the numbers, and confirmation an email was delivered end to end. Note any deferred items.
