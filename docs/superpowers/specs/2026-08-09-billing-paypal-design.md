# Billing module — PayPal top-ups and a credit wallet

**Date:** 2026-08-09
**Status:** approved design, ready for an implementation plan
**Scope:** THIS repository (cybercapybara/site) only. Billing is a product
feature of the site, not of the upstream template — it does not get
backported to cpp-rapid-rest-template.

Users buy internal credits with PayPal; an admin controls the exchange rate
and the sale packages. Spending those credits on services is explicitly a
LATER wave — this spec covers taking money and crediting the wallet only.

## Owner decisions (fixed)

- PayPal **REST v2** (Orders + Webhooks). Classic NVP/SOAP is not used —
  it is legacy, and SOAP would need an XML stack this template does not have.
- Pricing: **rate + packages**. Admin sets credits-per-unit and a list of
  sale packages; a user picks a package or enters a custom amount.
- Balance storage: **ledger of entries** (not a mutable `balance` column).
- Currency: charge in **USD**, wallet holds **integer credits**.
- Completion: **capture on return + webhook as the safety net**, both
  idempotent.
- Environment: **sandbox first**, live is a config switch.

## Module gating

`billing.enabled` (default **false**, env `BILLING_ENABLED`) — the same
pattern as `content.enabled` inherited from the template. Every route below
answers 404 when off. Migrations always apply (empty tables are harmless).

**Build/deploy consequence:** the site currently runs the template's public
images (`ghcr.io/moveeeax/cpp-rapid-rest-template*`). Billing code lives only
here, so this repo must publish its OWN images
(`ghcr.io/cybercapybara/site{,-worker,-frontend}`) before billing can ship —
the inherited `release.yml` already does the whole build→scan→promote dance,
it only needs `IMAGE_NAME` retargeted and a tag pushed. Treat that as task
zero of the implementation plan.

## Data model — migration `007_billing.sql`

**`wallet_entries`** — the ledger, append-only:
`id UUID`, `user_id UUID → users`, `delta_credits BIGINT` (signed, never 0),
`kind` (`topup` | `spend` | `adjustment` | `refund`), `reference TEXT`
(payment id / service id / admin note key), `note TEXT`, `created_by UUID`
NULL (admin for adjustments), `created_at TIMESTAMPTZ`.
Index on `(user_id, created_at DESC)`.

**`wallet_balances`** — derived cache: `user_id UUID PK`, `credits BIGINT NOT
NULL DEFAULT 0`, `updated_at`. Written in the SAME transaction as every
ledger insert. An integration test asserts the invariant
`SUM(wallet_entries.delta_credits) == wallet_balances.credits` per user after
a mixed workload — the cache is an optimization, the ledger is the truth.

**`payments`** — provider orders:
`id UUID`, `user_id UUID`, `provider TEXT` (`paypal`), `provider_order_id
TEXT UNIQUE NOT NULL`, `provider_capture_id TEXT UNIQUE NULL`,
`amount_cents BIGINT`, `currency CHAR(3)`, `credits_expected BIGINT`,
`rate_snapshot BIGINT` (credits per 100 cents at creation time),
`package_id UUID NULL → billing_packages`, `status`
(`created` | `approved` | `captured` | `failed` | `refunded`),
`failure_reason TEXT NULL`, `created_at`, `updated_at`.

> `provider_capture_id UNIQUE` is the structural guard against double
> crediting: the return-flow capture and the webhook race each other by
> design, and the loser hits a unique-violation instead of relying on
> application logic being careful.

**`billing_packages`** — admin-managed catalogue: `id UUID`, `title TEXT`,
`amount_cents BIGINT`, `credits BIGINT`, `active BOOL`, `sort INT`,
timestamps.

Rate and limits live in config (`billing.credits_per_unit`,
`billing.min_amount_cents`, `billing.max_amount_cents`) and are editable from
the admin UI; every change is written to the existing audit log.

## API

| Route | Auth | Behaviour |
|---|---|---|
| `GET /api/v1/billing/packages` | user | active packages + current rate + min/max |
| `GET /api/v1/billing/wallet` | user | own balance + paged ledger history |
| `POST /api/v1/billing/topup` | user | body: `{package_id}` OR `{amount_cents}`; server computes credits from the rate (never trusts a client-sent credit count), creates `payments(created)` + a PayPal order, returns the approve link |
| `POST /api/v1/billing/capture` | user | body `{order_id}`; captures via PayPal, credits the wallet, returns the new balance |
| `POST /api/v1/billing/paypal/webhook` | public | signature-verified; handles `PAYMENT.CAPTURE.COMPLETED` (credit) and `PAYMENT.CAPTURE.REFUNDED` (negative `refund` entry) |
| `GET /api/v1/admin/billing/payments` | admin | paged, filter by status/user |
| `GET/POST/PATCH/DELETE /api/v1/admin/billing/packages[/{id}]` | admin | package CRUD |
| `GET/PUT /api/v1/admin/billing/settings` | admin | rate + min/max |
| `POST /api/v1/admin/billing/users/{id}/adjust` | admin | manual `adjustment` entry (signed delta + mandatory note) |

`api.public_paths` gains only the webhook path. The webhook is exempt from
CSRF (no browser session involved) and rides the strict rate-limit tier.

## Crediting rules (the money-critical part)

1. Credit application is ONE database transaction: insert `wallet_entries`,
   upsert `wallet_balances`, move `payments.status` to `captured`, store
   `provider_capture_id`.
2. Both entry points (capture endpoint, webhook) call the same internal
   function. It is idempotent: if `provider_capture_id` already exists, it
   returns the existing state without touching the ledger.
3. Amounts and rates are integers throughout (cents, credits). No floating
   point anywhere in the money path.
4. The credited amount is `payments.credits_expected`, frozen at order
   creation — a rate change mid-payment cannot alter an in-flight order.
5. PayPal amount mismatch (captured amount ≠ ordered amount) → do NOT credit;
   mark `failed` with the reason and log at error level.

## Configuration and secrets

```
billing:
  enabled: false
  provider: paypal
  currency: USD
  credits_per_unit: 100        # credits per 1.00 USD
  min_amount_cents: 100
  max_amount_cents: 100000
  paypal:
    environment: sandbox       # sandbox | live
    client_id: ""              # env PAYPAL_CLIENT_ID
    client_secret: ""          # env PAYPAL_CLIENT_SECRET (Secret, never in git)
    webhook_id: ""             # env PAYPAL_WEBHOOK_ID (needed for signature verify)
    return_url: ""             # defaults to {app.base_url}/billing/return
    cancel_url: ""             # defaults to {app.base_url}/billing/cancel
```

Helm: `billing.*` values → configmap, the two secrets → the chart Secret and
`PAYPAL_CLIENT_SECRET` / `PAYPAL_WEBHOOK_ID` env (mirroring how
`MAIL_SMTP_PASSWORD` and `S3_SECRET_KEY` are wired).

## PayPal client

New `src/billing/PayPalClient.hpp` (header-only, libcurl + nlohmann::json,
same shape as `S3Storage`):
- OAuth2 token fetch with in-process caching until expiry (minus a margin).
- `create_order(amount_cents, currency, reference, return_url, cancel_url)`.
- `capture_order(order_id)`.
- `verify_webhook_signature(headers, raw_body, webhook_id)` via PayPal's
  verification endpoint — the raw body must be verified BEFORE parsing it as
  trusted input.
- Every call has connect/total timeouts and maps non-2xx to a typed error;
  network failures never crash a handler (all wrapped in `with_repo_errors`).

## Frontend

- User page `/billing`: current balance, package cards, custom-amount input
  (validated against min/max), "Pay with PayPal" → redirect; `/billing/return`
  calls capture and shows the new balance; `/billing/cancel` explains nothing
  was charged; ledger history table.
- Admin: a **Billing tile** in the `/admin` dashboard grid (consistent with
  Posts/Media) leading to packages CRUD, rate/limits form, payments table and
  the manual-adjustment dialog.

## Testing

- Unit: credits calculation (rate, package, rounding, min/max boundaries),
  ledger/balance invariant helpers.
- Integration (PayPal calls stubbed — no network in CI): topup creates a
  `created` payment with a frozen rate; capture credits exactly once; a second
  capture is a no-op; webhook for an already-captured order is a no-op;
  webhook-first (user closed the tab) credits correctly; refund produces a
  negative entry; amount mismatch refuses to credit; non-admin cannot reach
  admin routes; `billing.enabled=false` → every route 404.
- E2E: wallet and packages endpoints over real HTTP with an authenticated
  session; webhook path reachable without auth but rejected without a valid
  signature.

## Non-goals (later waves)

Spending credits on services, subscriptions/recurring payments, invoices and
tax handling, multi-currency wallets, payout/withdrawal, other providers
(the provider column exists so a second one can be added without a migration).

## Delivery

Branch `feat/billing-paypal` in THIS repo, spec + implementation, PR through
the inherited CI gates. Merging upstream template updates stays possible —
billing lives in new files plus small, clearly marked hooks. Sandbox credentials are supplied by the owner at the wiring step; no
real credentials ever land in git. Plain conventional commits, no
AI-attribution trailers.
