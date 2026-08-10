import { useMemo, useState } from 'react';
import { useQuery } from '@tanstack/react-query';
import {
  Area,
  AreaChart,
  CartesianGrid,
  ResponsiveContainer,
  Tooltip,
  XAxis,
  YAxis,
} from 'recharts';

import { DataTable, type Column } from '@/components/DataTable';
import { Card, CardContent, CardDescription, CardHeader, CardTitle } from '@/components/ui/card';
import { Skeleton } from '@/components/ui/skeleton';
import { useErrorToast } from '@/hooks/useErrorToast';
import { api, ApiClientError, apiErrorMessage } from '@/lib/api/client';
import { qk } from '@/lib/api/queryKeys';
import { formatAxisDollars, formatBucketTick, formatCompactUsd } from '@/lib/billingMetrics';
import { formatCents } from '@/lib/money';

/**
 * Admin billing "Overview" tab: KPI tiles, a revenue-over-time chart and the
 * top-packages/top-users breakdowns for GET /api/v1/admin/billing/metrics
 * (Task 3, docs/openapi.yaml's BillingMetricsResponse). Chart + tile design
 * follows the repo's `dataviz` skill: a single-series area chart (categorical
 * slot-1 blue — nothing to distinguish by identity here, so no legend) and
 * the stat-tile contract for the KPI row.
 *
 * SCHEMA NOTE: same situation as Billing.tsx's other tabs — schema.gen.ts
 * hasn't been regenerated since this endpoint landed in openapi.yaml (Task
 * 3), so the response is hand-typed here and fetched through the
 * string-path `getJson` overload rather than the typed-path one.
 *
 * MONEY: every *_cents field from the API stays an integer until the exact
 * point it is rendered — dividing by 100 anywhere upstream (e.g. to derive
 * revenue from summing `series[]`) would drift at window edges, which is
 * why the top-level revenue_cents/avg_payment_cents/etc. KPI values are
 * shown as the API returns them and `series[]` is used only for the chart.
 */

type Period = 'day' | 'week' | 'month';

interface MetricsSeriesPoint {
  bucket_start: string;
  revenue_cents: number;
  payments_count: number;
}
interface MetricsTopPackage {
  package_id: string;
  title: string;
  revenue_cents: number;
  payments_count: number;
}
interface MetricsTopUser {
  user_id: string;
  email: string;
  topup_credits: number;
  revenue_cents: number;
}
interface BillingMetrics {
  period: Period;
  revenue_cents: number;
  payments_count: number;
  avg_payment_cents: number;
  conversion: { created: number; captured: number; rate: number };
  refunds_cents: number;
  refunds_count: number;
  outstanding_credits: number;
  outstanding_value_cents: number;
  series: MetricsSeriesPoint[];
  top_packages: MetricsTopPackage[];
  top_users: MetricsTopUser[];
}
interface BillingMetricsResponse {
  data: BillingMetrics;
}

const PERIODS: { value: Period; label: string }[] = [
  { value: 'day', label: 'Day' },
  { value: 'week', label: 'Week' },
  { value: 'month', label: 'Month' },
];

const PERIOD_CAPTION: Record<Period, string> = {
  day: 'Last 24 hours, hourly buckets.',
  week: 'Last 7 days, daily buckets.',
  month: 'Last 30 days, daily buckets.',
};

function fmtDateTime(iso: string): string {
  try {
    return new Date(iso).toLocaleString();
  } catch {
    return iso;
  }
}

function PeriodToggle({ period, onChange }: { period: Period; onChange: (p: Period) => void }) {
  return (
    <div
      className="inline-flex rounded-md border border-border p-0.5"
      role="group"
      aria-label="Metrics period"
    >
      {PERIODS.map((p) => (
        <button
          key={p.value}
          type="button"
          aria-pressed={period === p.value}
          onClick={() => onChange(p.value)}
          className={`rounded px-3 py-1 text-sm font-medium transition-colors ${
            period === p.value
              ? 'bg-primary text-primary-foreground'
              : 'text-muted-foreground hover:text-foreground'
          }`}
        >
          {p.label}
        </button>
      ))}
    </div>
  );
}

function StatTile({ label, value, sub }: { label: string; value: string; sub?: string }) {
  return (
    <div className="rounded-md border border-border bg-card p-4">
      <p className="text-xs font-medium uppercase tracking-wide text-muted-foreground">{label}</p>
      <p className="mt-1 text-2xl font-semibold">{value}</p>
      {sub && <p className="mt-0.5 text-xs text-muted-foreground">{sub}</p>}
    </div>
  );
}

function StatTileSkeleton() {
  return (
    <div className="rounded-md border border-border bg-card p-4 space-y-2">
      <Skeleton className="h-3 w-16" />
      <Skeleton className="h-7 w-20" />
    </div>
  );
}

interface ChartPoint {
  bucket_start: string;
  revenueDollars: number;
  payments_count: number;
}

function RevenueTooltip({
  active,
  payload,
}: {
  active?: boolean;
  payload?: { payload: ChartPoint }[];
}) {
  if (!active || !payload?.length) return null;
  const point = payload[0].payload;
  return (
    <div className="rounded-md border border-border bg-card px-3 py-2 text-xs shadow-sm">
      <p className="font-medium text-foreground">{fmtDateTime(point.bucket_start)}</p>
      <p className="mt-1 text-muted-foreground">
        Revenue:{' '}
        <span className="font-mono text-foreground">${point.revenueDollars.toFixed(2)}</span>
      </p>
      <p className="text-muted-foreground">
        Payments:{' '}
        <span className="font-mono text-foreground">{point.payments_count.toLocaleString()}</span>
      </p>
    </div>
  );
}

export function OverviewTab() {
  const [period, setPeriod] = useState<Period>('week');
  const [chartView, setChartView] = useState<'chart' | 'table'>('chart');

  const metricsQ = useQuery({
    queryKey: qk.admin.billing.metrics(period),
    queryFn: () =>
      api.getJson<BillingMetricsResponse>('/api/v1/admin/billing/metrics', {
        query: { period },
      }),
  });

  // A 404 here means billing.enabled=false — an expected admin-config state,
  // not a failure, so it must never fire the error toast (same gating as the
  // wallet balance indicator in Nav.tsx). Any OTHER error still toasts and
  // gets an inline message below.
  const billingDisabled = metricsQ.error instanceof ApiClientError && metricsQ.error.status === 404;
  useErrorToast(metricsQ.error && !billingDisabled ? apiErrorMessage(metricsQ.error) : null);

  const metrics = metricsQ.data?.data;

  const chartData = useMemo<ChartPoint[]>(
    () =>
      (metrics?.series ?? []).map((point) => ({
        bucket_start: point.bucket_start,
        // Cents → dollars at the render boundary only.
        revenueDollars: point.revenue_cents / 100,
        payments_count: point.payments_count,
      })),
    [metrics?.series],
  );

  const packageColumns: Column<MetricsTopPackage>[] = [
    { header: 'Package', className: 'font-medium', cell: (p) => p.title },
    {
      header: 'Payments',
      className: 'font-mono text-right',
      cell: (p) => p.payments_count.toLocaleString(),
    },
    {
      header: 'Revenue',
      className: 'font-mono text-right',
      cell: (p) => `$${formatCents(p.revenue_cents)}`,
    },
  ];
  const userColumns: Column<MetricsTopUser>[] = [
    { header: 'User', className: 'font-medium', cell: (u) => u.email },
    {
      header: 'Top-up credits',
      className: 'font-mono text-right',
      cell: (u) => u.topup_credits.toLocaleString(),
    },
    {
      header: 'Revenue',
      className: 'font-mono text-right',
      cell: (u) => `$${formatCents(u.revenue_cents)}`,
    },
  ];
  const seriesColumns: Column<ChartPoint>[] = [
    { header: 'Bucket', className: 'whitespace-nowrap', cell: (p) => fmtDateTime(p.bucket_start) },
    {
      header: 'Revenue',
      className: 'font-mono text-right',
      cell: (p) => `$${p.revenueDollars.toFixed(2)}`,
    },
    {
      header: 'Payments',
      className: 'font-mono text-right',
      cell: (p) => p.payments_count.toLocaleString(),
    },
  ];

  return (
    <div className="space-y-6">
      <div className="flex flex-wrap items-center justify-between gap-3">
        <p className="text-sm text-muted-foreground">{PERIOD_CAPTION[period]}</p>
        <PeriodToggle period={period} onChange={setPeriod} />
      </div>

      {billingDisabled && (
        <Card>
          <CardContent className="pt-6">
            <p className="text-sm text-muted-foreground">
              Billing is disabled (billing.enabled=false) — there are no metrics to show.
            </p>
          </CardContent>
        </Card>
      )}

      {!billingDisabled && metricsQ.error && (
        <Card>
          <CardContent className="pt-6">
            <p className="text-sm text-destructive">Could not load billing metrics.</p>
          </CardContent>
        </Card>
      )}

      {!billingDisabled && !metricsQ.error && (
        <>
          <div className="grid grid-cols-2 gap-4 sm:grid-cols-3 lg:grid-cols-6">
            {metrics ? (
              <>
                <StatTile label="Revenue" value={formatCompactUsd(metrics.revenue_cents)} />
                <StatTile label="Payments" value={metrics.payments_count.toLocaleString()} />
                <StatTile label="Avg payment" value={formatCompactUsd(metrics.avg_payment_cents)} />
                <StatTile
                  label="Conversion"
                  value={`${(metrics.conversion.rate * 100).toFixed(1)}%`}
                  sub={`${metrics.conversion.captured.toLocaleString()} of ${metrics.conversion.created.toLocaleString()} created`}
                />
                <StatTile
                  label="Outstanding liability"
                  value={`${metrics.outstanding_credits.toLocaleString()} credits`}
                  sub={`≈ $${formatCents(metrics.outstanding_value_cents)}`}
                />
                <StatTile
                  label="Refunds"
                  value={formatCompactUsd(metrics.refunds_cents)}
                  sub={`${metrics.refunds_count.toLocaleString()} refund${metrics.refunds_count === 1 ? '' : 's'}`}
                />
              </>
            ) : (
              Array.from({ length: 6 }).map((_, i) => <StatTileSkeleton key={i} />)
            )}
          </div>

          <Card>
            <CardHeader className="flex flex-row items-center justify-between space-y-0">
              <div>
                <CardTitle>Revenue over time</CardTitle>
                <CardDescription>
                  Captured payments, bucketed by {period === 'day' ? 'hour' : 'day'}.
                </CardDescription>
              </div>
              {chartData.length > 0 && (
                <button
                  type="button"
                  className="text-xs font-medium text-muted-foreground underline-offset-2 hover:text-foreground hover:underline"
                  onClick={() => setChartView(chartView === 'chart' ? 'table' : 'chart')}
                >
                  {chartView === 'chart' ? 'View as table' : 'View as chart'}
                </button>
              )}
            </CardHeader>
            <CardContent>
              {!metrics ? (
                <Skeleton className="h-[280px] w-full" />
              ) : chartData.length === 0 ? (
                <p className="text-sm text-muted-foreground">
                  No captured payments in this window.
                </p>
              ) : chartView === 'table' ? (
                <div className="overflow-x-auto">
                  <DataTable columns={seriesColumns} rows={chartData} rowKey={(p) => p.bucket_start} />
                </div>
              ) : (
                <ResponsiveContainer width="100%" height={280}>
                  <AreaChart data={chartData} margin={{ top: 8, right: 8, left: 8, bottom: 0 }}>
                    <CartesianGrid vertical={false} stroke="hsl(var(--border))" />
                    <XAxis
                      dataKey="bucket_start"
                      tickFormatter={(v: string) => formatBucketTick(v, period)}
                      stroke="hsl(var(--muted-foreground))"
                      tick={{ fontSize: 12 }}
                      tickLine={false}
                      axisLine={{ stroke: 'hsl(var(--border))' }}
                    />
                    <YAxis
                      tickFormatter={(v: number) => formatAxisDollars(v)}
                      stroke="hsl(var(--muted-foreground))"
                      tick={{ fontSize: 12 }}
                      tickLine={false}
                      axisLine={false}
                      width={64}
                    />
                    <Tooltip content={<RevenueTooltip />} cursor={{ stroke: 'hsl(var(--border))' }} />
                    <Area
                      type="monotone"
                      dataKey="revenueDollars"
                      stroke="var(--chart-revenue)"
                      strokeWidth={2}
                      fill="var(--chart-revenue)"
                      fillOpacity={0.1}
                      dot={false}
                      activeDot={{ r: 4, strokeWidth: 2, stroke: 'hsl(var(--card))' }}
                    />
                  </AreaChart>
                </ResponsiveContainer>
              )}
            </CardContent>
          </Card>

          <div className="grid gap-6 lg:grid-cols-2">
            <Card>
              <CardHeader>
                <CardTitle>Top packages</CardTitle>
                <CardDescription>By revenue among captured payments in-window.</CardDescription>
              </CardHeader>
              <CardContent className="overflow-x-auto">
                <DataTable
                  columns={packageColumns}
                  rows={metrics?.top_packages}
                  rowKey={(p) => p.package_id}
                  isLoading={metricsQ.isLoading}
                  emptyText="No package sales in this window."
                />
              </CardContent>
            </Card>
            <Card>
              <CardHeader>
                <CardTitle>Top users</CardTitle>
                <CardDescription>By top-up credits among captured payments in-window.</CardDescription>
              </CardHeader>
              <CardContent className="overflow-x-auto">
                <DataTable
                  columns={userColumns}
                  rows={metrics?.top_users}
                  rowKey={(u) => u.user_id}
                  isLoading={metricsQ.isLoading}
                  emptyText="No top-ups in this window."
                />
              </CardContent>
            </Card>
          </div>
        </>
      )}
    </div>
  );
}
