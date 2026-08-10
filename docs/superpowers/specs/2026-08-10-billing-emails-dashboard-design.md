# Billing emails + admin business dashboard

**Date:** 2026-08-10
**Repo:** cybercapybara/site ONLY (product feature, not backported to the template).
**Builds on:** the billing feature (spec 2026-08-09-billing-paypal-design.md),
already live on cybercapybara.kz with sandbox PayPal.

Two features: (A) transactional email on every billing status change, and
(B) an admin business dashboard on `/admin/billing` with day/week/month metrics.

## Owner decisions (fixed)

- Emails on: **credit (receipt), refund/chargeback, payment error, admin
  adjustment**. Receipt is an **HTML email** (no PDF).
- Admin manual adjustment: the reason field stays **mandatory** and is stored
  in the ledger `note` + audit log (already so); ADD a **"notify user"**
  checkbox — when set, the adjustment email (carrying the reason) is sent.
- Dashboard metrics: **revenue + payment count, outstanding credit liability +
  refunds, average payment + conversion, top packages + top users**, over
  **day / week / month**. Charts via **recharts**.

---

## Part A — billing emails

### Delivery mechanism

Reuse the existing mail stack. Billing context (amount, credits, new balance,
payment id) is richer than what a worker could re-derive from a user id, so
templates are **rendered at enqueue time** in C++ (the code paths already hold
the numbers) and shipped through `Email::SendEmail::send(to, subject, text,
html)` — the ad-hoc `email.send` job (retry/backoff/DLQ, worker already
subscribes to `email.send`; NO new job kind or WORKER_TYPES change).

New `src/email/BillingEmails.hpp` (mirrors `AccountEmails.hpp` shape):
- Loads the template pair from `templates/email/`, does `{{KEY}}` substitution
  via the existing template loader (same as AccountEmails), sets subject, calls
  `Email::SendEmail::send`.
- One `dispatch_*` helper per event; every one is **best-effort and never
  throws** (a mail failure must never affect the money path).
- All values pre-formatted by the caller (dollars from integer cents at the
  boundary only; credits as integers).

### Template pairs (`templates/email/`, .html + .txt each)

| Template | Trigger | Context keys |
|---|---|---|
| `billing_receipt` | a real credit applied | app_name, package_title (or "Custom top-up"), amount, currency, credits, new_balance, payment_id, date, rate |
| `billing_refund` | REFUNDED or REVERSED debit applied | app_name, kind_label ("Refund"/"Reversal"), amount, currency, credits_deducted, new_balance, payment_id, date |
| `billing_failed` | payment set to `failed` | app_name, amount, currency, reason (human), date — copy MUST say "no money was charged / you were not charged" |
| `billing_adjustment` | admin adjust with notify=true | app_name, delta_credits (signed, shown +/-), reason, new_balance, date |

Match the visual style of the existing `confirm.html` etc. (same header/footer,
inline CSS, no external assets — CSP-safe).

### Wiring (the money-safety rules)

- **Enqueue AFTER the wallet transaction commits**, from the controller/service
  layer, never inside `execute_write`.
- **De-dupe by the credit result**: `Billing::credit_capture` returns
  `credited=true` only on a real credit (idempotent no-op → `false`). Send the
  receipt only when `credited==true`, so the return-flow capture and the webhook
  for one payment produce EXACTLY ONE receipt. Same rule for
  `refund_capture` (send only when a debit actually happened, not on a
  duplicate-refund-id no-op or a skipped-insufficient outcome — for those, log,
  no email).
- **`billing_failed`**: sent where `credit_capture` marks a payment `failed`
  (amount/currency mismatch). Because this can be reached from both the capture
  endpoint and the webhook, guard the same way (send once — the status
  transition to `failed` happens once, keyed on the guarded UPDATE).
- **`billing_adjustment`**: `AdminBillingController::adjustWallet` gains an
  optional `notify` bool in the request body (default false); when true and the
  adjust succeeded, enqueue the adjustment email with the reason. The reason is
  the already-mandatory `note`.
- The user's email + name come from `UserRepository::find(user_id)`; if the user
  can't be loaded, log and skip (never throw).

### Email address / user lookup

Billing paths currently carry `user_id` (a UUID), not the email. Each dispatch
loads the user via `UserRepository` to get the address and name. This is one
extra read on a non-hot path (after a payment), acceptable.

---

## Part B — admin business dashboard

### Metrics endpoint

`GET /api/v1/admin/billing/metrics?period=day|week|month` — admin-only (same
`Core::billing_enabled()` + `API_REQUIRE_ADMIN` guards as the other admin
billing routes). `period` selects the window and the time-series granularity:

| period | window | series buckets |
|---|---|---|
| day | last 24h | hourly |
| week | last 7 days | daily |
| month | last 30 days | daily |

Response (all money as integer cents, credits as integers):

```json
{
  "period": "week",
  "revenue_cents": 123400,
  "payments_count": 42,
  "avg_payment_cents": 2938,
  "conversion": { "created": 60, "captured": 42, "rate": 0.70 },
  "refunds_cents": 5000,
  "refunds_count": 2,
  "outstanding_credits": 87000,
  "outstanding_value_cents": 87000,      // credits × current rate → money-equivalent liability
  "series": [ { "bucket": "2026-08-04", "revenue_cents": 20000, "payments": 7 }, ... ],
  "top_packages": [ { "id": "...", "title": "Starter", "count": 20, "revenue_cents": 20000 }, ... ],
  "top_users": [ { "user_id": "...", "email": "a@b.c", "topup_credits": 11000, "payments": 2 }, ... ]
}
```

- Aggregations are SQL over `payments` (captured rows for revenue/count/avg,
  all rows for conversion), `billing_refunds` (refunds), `wallet_balances`
  (SUM(credits) = outstanding liability), `billing_settings` (current rate for
  the money-equivalent), joined to `billing_packages`/`users` for the top lists.
  No materialization — indexed scans at this scale.
- `top_packages`/`top_users` capped at 5. `top_users` exposes email (admin
  context, already visible in the payments tab).
- New `BillingMetricsRepository` (or methods on `BillingRepository`); one
  method per block, each a single parameterized aggregate query. Time buckets
  via `date_trunc` with a `generate_series` left join so empty buckets render as
  zero (no gaps in the chart).

### Dashboard UI

New **Overview** tab on `/admin/billing` (alongside packages/payments/settings),
made the default tab. Layout:
- A **day / week / month** segmented toggle at the top.
- **KPI tiles** row: Revenue, Payments, Avg payment, Conversion %, Outstanding
  liability (credits + money-equiv), Refunds.
- A **revenue-over-time** area/bar chart (recharts) from `series`.
- Two tables: **Top packages** and **Top users**.

recharts is a new frontend dependency (add to `frontend/package.json`); it is
tree-shaken into the bundle and needs no external/CDN assets, so it stays
CSP-safe. Money formatted from integer cents at the render boundary only.

### Follow the dataviz guidance

The dashboard's charts and stat tiles MUST be built per the `dataviz` skill
(color, stat-tile, chart-type rules) — this is exactly its remit.

---

## Non-goals (later)

CSV export, arbitrary date ranges, PDF receipts, live/push dashboard updates,
per-currency breakdown (single-currency USD for now), spending credits on
services (separate wave).

## Delivery

Two branches off master (emails, then dashboard) OR one `feat/billing-emails-dashboard`
— implementer's call in the plan. Ships with billing already enabled in prod.
Emails are gated by the same `mail.enabled`/`billing.enabled` posture (no mail
when either is off). Normal CI gates. Plain conventional commits, no
AI-attribution trailers.
