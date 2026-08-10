import { useState } from 'react';
import { keepPreviousData, useQuery } from '@tanstack/react-query';
import { CreditCard } from 'lucide-react';

import { DataTable, type Column } from '@/components/DataTable';
import { Button } from '@/components/ui/button';
import { Card, CardContent, CardDescription, CardHeader, CardTitle } from '@/components/ui/card';
import { Input } from '@/components/ui/input';
import { Label } from '@/components/ui/label';
import { useApiMutation } from '@/hooks/useApiMutation';
import { useErrorToast } from '@/hooks/useErrorToast';
import { api } from '@/lib/api/client';
import { qk } from '@/lib/api/queryKeys';
import type { BillingPackage, PublicWalletEntry } from '@/lib/api/types';
import { creditsForAmount, dollarsToCents, formatCents } from '@/lib/money';

const HISTORY_PER_PAGE = 20;

/**
 * User wallet / top-up page: current balance, package cards, a
 * custom-amount input, and paged ledger history. "Pay with PayPal" starts a
 * top-up (POST /billing/topup) and redirects the browser to PayPal's own
 * approve_url; PayPal redirects back to /billing/return, which captures the
 * order (see BillingReturn.tsx).
 *
 * WalletResponse carries `limit`/`offset` but no `total` — the ledger page
 * can't use usePagedQuery (which requires a total for page-count math), so
 * history paging here is a plain "fetched a full page ⇒ maybe more" cursor,
 * not a page-count footer.
 */

function KindBadge({ kind }: { kind: PublicWalletEntry['kind'] }) {
  const styles: Record<PublicWalletEntry['kind'], string> = {
    topup:
      'border-emerald-300 bg-emerald-50 text-emerald-700 dark:border-emerald-500/30 dark:bg-emerald-500/10 dark:text-emerald-300',
    spend:
      'border-indigo-300 bg-indigo-50 text-indigo-700 dark:border-indigo-500/30 dark:bg-indigo-500/10 dark:text-indigo-300',
    adjustment:
      'border-amber-300 bg-amber-50 text-amber-700 dark:border-amber-500/30 dark:bg-amber-500/10 dark:text-amber-300',
    refund:
      'border-orange-300 bg-orange-50 text-orange-700 dark:border-orange-500/30 dark:bg-orange-500/10 dark:text-orange-300',
  };
  return (
    <span
      className={`inline-flex items-center rounded border px-2 py-0.5 text-xs font-medium ${styles[kind] ?? ''}`}
    >
      {kind}
    </span>
  );
}

function fmtDate(iso: string): string {
  try {
    return new Date(iso).toLocaleString();
  } catch {
    return iso;
  }
}

export function BillingPage() {
  const packagesQ = useQuery({
    queryKey: qk.billing.packages(),
    queryFn: () => api.getJson('/api/v1/billing/packages'),
  });

  const [historyPage, setHistoryPage] = useState(1);
  const historyOffset = (historyPage - 1) * HISTORY_PER_PAGE;
  const walletQ = useQuery({
    queryKey: qk.billing.wallet(historyPage),
    queryFn: () =>
      api.getJson('/api/v1/billing/wallet', {
        query: { limit: HISTORY_PER_PAGE, offset: historyOffset },
      }),
    placeholderData: keepPreviousData,
  });

  const [customAmount, setCustomAmount] = useState('');

  const topup = useApiMutation(
    (vars: { package_id?: string; amount_cents?: number }) =>
      api.postJson('/api/v1/billing/topup', { body: vars }),
    {
      // The whole point of a successful call is to leave the page for
      // PayPal, so there's nothing left here to invalidate.
      onSuccess: (res) => {
        window.location.href = res.data.approve_url;
      },
    },
  );
  useErrorToast(topup.error);

  const bounds = packagesQ.data;
  const cents = dollarsToCents(customAmount);
  const customValid =
    cents !== null &&
    bounds !== undefined &&
    cents >= bounds.min_amount_cents &&
    cents <= bounds.max_amount_cents;
  const customCreditsPreview =
    cents !== null && bounds ? creditsForAmount(cents, bounds.credits_per_unit) : null;
  const customError =
    customAmount.trim() !== '' && !customValid && bounds
      ? cents === null
        ? 'Enter a dollar amount like 12.34.'
        : `Amount must be between $${formatCents(bounds.min_amount_cents)} and $${formatCents(bounds.max_amount_cents)}.`
      : null;

  const historyColumns: Column<PublicWalletEntry>[] = [
    { header: 'Date', className: 'whitespace-nowrap text-xs', cell: (e) => fmtDate(e.created_at) },
    { header: 'Type', cell: (e) => <KindBadge kind={e.kind} /> },
    {
      header: 'Credits',
      className: 'font-mono text-right',
      cell: (e) => (
        <span className={e.delta_credits < 0 ? 'text-destructive' : 'text-emerald-600'}>
          {e.delta_credits > 0 ? '+' : ''}
          {e.delta_credits.toLocaleString()}
        </span>
      ),
    },
    { header: 'Note', className: 'text-muted-foreground', cell: (e) => e.note || '—' },
  ];

  const history: PublicWalletEntry[] | undefined = walletQ.data?.data.history;
  const hasNextHistoryPage = (history?.length ?? 0) >= HISTORY_PER_PAGE;

  return (
    <div className="container mx-auto max-w-3xl py-8 space-y-6">
      <div>
        <h1 className="text-3xl font-bold">Wallet</h1>
        <p className="text-sm text-muted-foreground">
          Top up credits via PayPal and review your ledger history.
        </p>
      </div>

      <Card>
        <CardHeader>
          <CardTitle>Balance</CardTitle>
        </CardHeader>
        <CardContent>
          <p className="text-4xl font-bold">
            {walletQ.isLoading
              ? '…'
              : (walletQ.data?.data.balance.toLocaleString() ?? '—')}{' '}
            <span className="text-base font-normal text-muted-foreground">credits</span>
          </p>
        </CardContent>
      </Card>

      <Card>
        <CardHeader>
          <CardTitle>Top up</CardTitle>
          <CardDescription>
            Choose a package or enter a custom amount, then pay with PayPal.
          </CardDescription>
        </CardHeader>
        <CardContent className="space-y-6">
          {packagesQ.isLoading && <p className="text-sm text-muted-foreground">Loading…</p>}
          {packagesQ.error && (
            <p className="text-sm text-destructive">Could not load top-up packages.</p>
          )}
          {packagesQ.data && (
            <div className="grid gap-3 sm:grid-cols-2">
              {packagesQ.data.data.map((pkg: BillingPackage) => (
                <div
                  key={pkg.id}
                  className="flex items-center justify-between rounded-md border border-border p-4"
                >
                  <div>
                    <p className="font-medium">{pkg.title}</p>
                    <p className="text-sm text-muted-foreground">
                      ${formatCents(pkg.amount_cents)} → {pkg.credits.toLocaleString()} credits
                    </p>
                  </div>
                  <Button
                    size="sm"
                    disabled={topup.isPending}
                    onClick={() => topup.mutate({ package_id: pkg.id })}
                  >
                    <CreditCard className="h-3.5 w-3.5 mr-1" />
                    Pay
                  </Button>
                </div>
              ))}
              {packagesQ.data.data.length === 0 && (
                <p className="text-sm text-muted-foreground sm:col-span-2">
                  No packages configured — use a custom amount below.
                </p>
              )}
            </div>
          )}

          {packagesQ.data && (
            <div className="space-y-2 border-t pt-4">
              <Label htmlFor="custom-amount">Custom amount (USD)</Label>
              <div className="flex items-start gap-2">
                <div className="flex-1 space-y-1">
                  <Input
                    id="custom-amount"
                    inputMode="decimal"
                    placeholder={`${formatCents(packagesQ.data.min_amount_cents)} – ${formatCents(packagesQ.data.max_amount_cents)}`}
                    value={customAmount}
                    onChange={(e) => setCustomAmount(e.target.value)}
                  />
                  {customError && <p className="text-xs text-destructive">{customError}</p>}
                  {customValid && customCreditsPreview !== null && (
                    <p className="text-xs text-muted-foreground">
                      ≈ {customCreditsPreview.toLocaleString()} credits
                    </p>
                  )}
                </div>
                <Button
                  disabled={!customValid || topup.isPending}
                  onClick={() => cents !== null && topup.mutate({ amount_cents: cents })}
                >
                  <CreditCard className="h-3.5 w-3.5 mr-1" />
                  Pay
                </Button>
              </div>
            </div>
          )}
        </CardContent>
      </Card>

      <Card>
        <CardHeader>
          <CardTitle>History</CardTitle>
        </CardHeader>
        <CardContent className="overflow-x-auto">
          <DataTable
            columns={historyColumns}
            rows={history}
            rowKey={(e) => e.id}
            isLoading={walletQ.isLoading}
            error={walletQ.error}
            emptyText="No ledger activity yet."
            isPlaceholder={walletQ.isPlaceholderData}
          />
          {(historyPage > 1 || hasNextHistoryPage) && (
            <div className="mt-4 flex items-center justify-between">
              <p className="text-sm text-muted-foreground">Page {historyPage}</p>
              <div className="space-x-2">
                <Button
                  variant="outline"
                  size="sm"
                  disabled={historyPage <= 1}
                  onClick={() => setHistoryPage((p) => p - 1)}
                >
                  Previous
                </Button>
                <Button
                  variant="outline"
                  size="sm"
                  disabled={!hasNextHistoryPage || walletQ.isPlaceholderData}
                  onClick={() => setHistoryPage((p) => p + 1)}
                >
                  Next
                </Button>
              </div>
            </div>
          )}
        </CardContent>
      </Card>
    </div>
  );
}
