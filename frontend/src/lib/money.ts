/**
 * Money helpers for the billing module.
 *
 * The backend deals in two distinct integer units and they must never be
 * mixed:
 *   - amount_cents — real-world currency (USD cents): Package.amount_cents,
 *     Payment.amount_cents, the topup amount_cents body field, and
 *     min/max_amount_cents bounds.
 *   - credits — the internal wallet unit (WalletResponse.data.balance,
 *     WalletEntry.delta_credits, Package.credits): a plain integer, NOT
 *     divided by 100.
 *
 * Every conversion between integer cents and a decimal dollar string lives
 * here, done ONLY at render/parse boundaries — nothing upstream should ever
 * do arithmetic on a formatted/float dollar amount.
 */

/** Integer cents → "12.34" for display. Never used for further arithmetic. */
export function formatCents(cents: number): string {
  return (cents / 100).toFixed(2);
}

/**
 * Parse a user-typed dollar string ("12.34") into integer cents (1234)
 * without float drift: the string is validated and split on the decimal
 * point rather than multiplied and rounded. Returns null for anything that
 * isn't a non-negative amount with at most 2 decimal places.
 */
export function dollarsToCents(input: string): number | null {
  const trimmed = input.trim();
  if (!/^\d+(\.\d{1,2})?$/.test(trimmed)) return null;
  const [whole, frac = ''] = trimmed.split('.');
  const cents = frac.padEnd(2, '0');
  return parseInt(whole, 10) * 100 + parseInt(cents, 10);
}

/**
 * Credits a given amount_cents buys at the given rate, mirroring the
 * server's own integer formula (amount_cents * credits_per_unit / 100,
 * truncated) — see BillingController::topup's doc comment in
 * docs/openapi.yaml. Display-only preview; the server always computes the
 * authoritative value.
 */
export function creditsForAmount(amountCents: number, creditsPerUnit: number): number {
  return Math.floor((amountCents * creditsPerUnit) / 100);
}
