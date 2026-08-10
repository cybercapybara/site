import { useState, type FormEvent } from 'react';
import { Link } from 'react-router-dom';
import { useQuery } from '@tanstack/react-query';

import { ConfirmDialog } from '@/components/ConfirmDialog';
import { DataTable, type Column } from '@/components/DataTable';
import { Modal } from '@/components/Modal';
import { PaginationFooter } from '@/components/PaginationFooter';
import { Button } from '@/components/ui/button';
import { Card, CardContent, CardHeader, CardTitle } from '@/components/ui/card';
import { Input } from '@/components/ui/input';
import { Label } from '@/components/ui/label';
import { useToast } from '@/components/ui/toaster';
import { useApiMutation } from '@/hooks/useApiMutation';
import { useErrorToast } from '@/hooks/useErrorToast';
import { usePagedQuery } from '@/hooks/usePagedQuery';
import { api } from '@/lib/api/client';
import { qk } from '@/lib/api/queryKeys';
import type { MessageResponse } from '@/lib/api/types';
import { dollarsToCents, formatCents } from '@/lib/money';
import { AdjustDialog } from '@/pages/admin/billing/AdjustDialog';
import { OverviewTab } from '@/pages/admin/billing/Overview';

const PER_PAGE = 20;

/**
 * Admin billing: the Overview metrics dashboard (default tab — see
 * pages/admin/billing/Overview.tsx), packages CRUD, the rate/bounds
 * settings, a payments table, and a manual wallet adjustment dialog (see
 * docs/superpowers/specs/2026-08-09-billing-paypal-design.md's Frontend
 * section).
 *
 * SCHEMA NOTE: docs/openapi.yaml has carried the admin billing schemas
 * (AdminPackage*, Payment, BillingSettings*, AdjustResponse) and paths
 * since Task 6 (commit 25737f9), but frontend/src/lib/api/schema.gen.ts
 * hasn't been regenerated since (last regen: ea1a31c, BEFORE Task 6). The
 * types below are hand-typed mirrors of those openapi.yaml schemas and the
 * calls use the api client's generic string-path overload rather than the
 * typed-path one. Once the autofix workflow (regenerate OpenAPI TS schema)
 * is dispatched, these can be swapped for `components['schemas'][...]`
 * aliases like every other page in this file tree.
 */
interface AdminPackage {
  id: string;
  title: string;
  amount_cents: number;
  credits: number;
  active: boolean;
  sort: number;
  created_at: string;
  updated_at: string;
}
interface AdminPackageListResponse {
  data: AdminPackage[];
}
interface AdminPackageResponse {
  data: AdminPackage;
}
interface AdminPayment {
  id: string;
  user_id: string;
  provider: string;
  provider_order_id: string;
  provider_capture_id: string | null;
  amount_cents: number;
  currency: string;
  credits_expected: number;
  rate_snapshot: number;
  package_id: string | null;
  status: 'created' | 'approved' | 'captured' | 'failed' | 'refunded';
  failure_reason: string | null;
  created_at: string;
  updated_at: string;
}
interface AdminPaymentListResponse {
  data: AdminPayment[];
  total: number;
  limit: number;
  offset: number;
}
interface AdminBillingSettings {
  credits_per_unit: number;
  min_amount_cents: number;
  max_amount_cents: number;
  updated_at: string;
}
interface AdminBillingSettingsResponse {
  data: AdminBillingSettings;
}
const PAYMENT_STATUS_STYLES: Record<AdminPayment['status'], string> = {
  created:
    'border-slate-300 bg-slate-50 text-slate-700 dark:border-slate-500/30 dark:bg-slate-500/10 dark:text-slate-300',
  approved:
    'border-indigo-300 bg-indigo-50 text-indigo-700 dark:border-indigo-500/30 dark:bg-indigo-500/10 dark:text-indigo-300',
  captured:
    'border-emerald-300 bg-emerald-50 text-emerald-700 dark:border-emerald-500/30 dark:bg-emerald-500/10 dark:text-emerald-300',
  failed:
    'border-red-300 bg-red-50 text-red-700 dark:border-red-500/30 dark:bg-red-500/10 dark:text-red-300',
  refunded:
    'border-amber-300 bg-amber-50 text-amber-700 dark:border-amber-500/30 dark:bg-amber-500/10 dark:text-amber-300',
};

function PaymentStatusBadge({ status }: { status: AdminPayment['status'] }) {
  return (
    <span
      className={`inline-flex items-center rounded border px-2 py-0.5 text-xs font-medium ${PAYMENT_STATUS_STYLES[status] ?? ''}`}
    >
      {status}
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

export function AdminBillingPage() {
  const [tab, setTab] = useState<'overview' | 'packages' | 'payments' | 'settings'>('overview');
  const [adjusting, setAdjusting] = useState(false);

  return (
    <div className="container mx-auto py-8 space-y-6">
      <div className="flex items-center justify-between">
        <div>
          <h1 className="text-3xl font-bold">Billing</h1>
          <p className="text-sm text-muted-foreground">
            Top-up packages, rate/bounds, payments and manual wallet adjustments.
          </p>
        </div>
        <div className="flex gap-2">
          <Button asChild variant="ghost">
            <Link to="/admin">← Admin</Link>
          </Button>
          <Button variant="outline" onClick={() => setAdjusting(true)}>
            Adjust balance
          </Button>
        </div>
      </div>

      <div className="flex gap-2 border-b">
        {(['overview', 'packages', 'payments', 'settings'] as const).map((t) => (
          <button
            key={t}
            className={`px-4 py-2 text-sm font-medium border-b-2 -mb-px capitalize ${
              tab === t
                ? 'border-primary text-foreground'
                : 'border-transparent text-muted-foreground hover:text-foreground'
            }`}
            onClick={() => setTab(t)}
          >
            {t}
          </button>
        ))}
      </div>

      {tab === 'overview' && <OverviewTab />}
      {tab === 'packages' && <PackagesTab />}
      {tab === 'payments' && <PaymentsTab />}
      {tab === 'settings' && <SettingsTab />}

      {adjusting && <AdjustDialog onClose={() => setAdjusting(false)} />}
    </div>
  );
}

// ── Packages ─────────────────────────────────────────────────────────────

interface PackageForm {
  title: string;
  amount: string; // dollars, e.g. "5.00" — converted to amount_cents on submit
  credits: string;
  active: boolean;
  sort: string;
}

function PackagesTab() {
  const [creating, setCreating] = useState(false);
  const [editing, setEditing] = useState<AdminPackage | null>(null);
  const [deleting, setDeleting] = useState<AdminPackage | null>(null);

  const { data, isLoading, error } = useQuery({
    queryKey: qk.admin.billing.packages(),
    queryFn: () => api.getJson<AdminPackageListResponse>('/api/v1/admin/billing/packages'),
  });

  const create = useApiMutation(
    (form: PackageForm) =>
      api.postJson<AdminPackageResponse>('/api/v1/admin/billing/packages', {
        body: packageBody(form),
      }),
    { invalidate: [qk.admin.billing.packages()], onSuccess: () => setCreating(false) },
  );
  const update = useApiMutation(
    (vars: { id: string; form: PackageForm }) =>
      api.patchJson<AdminPackageResponse>(`/api/v1/admin/billing/packages/${vars.id}`, {
        body: packageBody(vars.form),
      }),
    { invalidate: [qk.admin.billing.packages()], onSuccess: () => setEditing(null) },
  );
  const remove = useApiMutation(
    (id: string) => api.deleteJson<MessageResponse>(`/api/v1/admin/billing/packages/${id}`),
    { invalidate: [qk.admin.billing.packages()], onSuccess: () => setDeleting(null) },
  );
  useErrorToast(create.error ?? update.error ?? remove.error);

  const columns: Column<AdminPackage>[] = [
    { header: 'Title', className: 'font-medium', cell: (p) => p.title },
    { header: 'Price', className: 'font-mono', cell: (p) => `$${formatCents(p.amount_cents)}` },
    { header: 'Credits', className: 'font-mono', cell: (p) => p.credits.toLocaleString() },
    {
      header: 'Active',
      cell: (p) => (
        <span className={p.active ? 'text-green-600' : 'text-muted-foreground'}>
          {p.active ? 'yes' : 'no'}
        </span>
      ),
    },
    { header: 'Sort', className: 'font-mono', cell: (p) => p.sort },
    {
      header: '',
      className: 'text-right space-x-1',
      cell: (p) => (
        <>
          <Button size="sm" variant="ghost" onClick={() => setEditing(p)}>
            Edit
          </Button>
          <Button size="sm" variant="ghost" onClick={() => setDeleting(p)}>
            <span className="text-destructive">Delete</span>
          </Button>
        </>
      ),
    },
  ];

  return (
    <div className="space-y-4">
      <div className="flex justify-end">
        <Button onClick={() => setCreating(true)}>New package</Button>
      </div>
      <Card>
        <CardContent className="overflow-x-auto pt-6">
          <DataTable
            columns={columns}
            rows={data?.data}
            rowKey={(p) => p.id}
            isLoading={isLoading}
            error={error}
            emptyText="No packages yet."
          />
        </CardContent>
      </Card>

      {creating && (
        <Modal onClose={() => setCreating(false)}>
          <PackageFormCard
            title="New package"
            submitting={create.isPending}
            initial={{ title: '', amount: '', credits: '', active: true, sort: '0' }}
            onSubmit={(form) => create.mutate(form)}
            onCancel={() => setCreating(false)}
          />
        </Modal>
      )}
      {editing && (
        <Modal onClose={() => setEditing(null)}>
          <PackageFormCard
            title={`Edit: ${editing.title}`}
            submitting={update.isPending}
            initial={{
              title: editing.title,
              amount: formatCents(editing.amount_cents),
              credits: String(editing.credits),
              active: editing.active,
              sort: String(editing.sort),
            }}
            onSubmit={(form) => update.mutate({ id: editing.id, form })}
            onCancel={() => setEditing(null)}
          />
        </Modal>
      )}
      {deleting && (
        <ConfirmDialog
          title="Delete package"
          description={`Delete "${deleting.title}"? This cannot be undone.`}
          confirmLabel="Delete package"
          destructive
          busy={remove.isPending}
          onConfirm={() => remove.mutate(deleting.id)}
          onClose={() => setDeleting(null)}
        />
      )}
    </div>
  );
}

function packageBody(form: PackageForm) {
  const amount_cents = dollarsToCents(form.amount) ?? 0;
  return {
    title: form.title.trim(),
    amount_cents,
    credits: parseInt(form.credits, 10) || 0,
    active: form.active,
    sort: parseInt(form.sort, 10) || 0,
  };
}

function PackageFormCard({
  title,
  initial,
  submitting,
  onSubmit,
  onCancel,
}: {
  title: string;
  initial: PackageForm;
  submitting: boolean;
  onSubmit: (form: PackageForm) => void;
  onCancel: () => void;
}) {
  const [form, setForm] = useState(initial);
  const amountCents = dollarsToCents(form.amount);
  const amountValid = amountCents !== null && amountCents > 0;
  const creditsValid = /^\d+$/.test(form.credits) && parseInt(form.credits, 10) > 0;

  const handleSubmit = (e: FormEvent) => {
    e.preventDefault();
    if (!amountValid || !creditsValid) return;
    onSubmit(form);
  };

  return (
    <Card>
      <CardHeader>
        <CardTitle>{title}</CardTitle>
      </CardHeader>
      <CardContent>
        <form onSubmit={handleSubmit} className="space-y-4">
          <div className="space-y-1">
            <Label htmlFor="pkg-title">Title</Label>
            <Input
              id="pkg-title"
              value={form.title}
              onChange={(e) => setForm({ ...form, title: e.target.value })}
              required
              maxLength={200}
            />
          </div>
          <div className="grid grid-cols-2 gap-4">
            <div className="space-y-1">
              <Label htmlFor="pkg-amount">Price (USD)</Label>
              <Input
                id="pkg-amount"
                inputMode="decimal"
                placeholder="5.00"
                value={form.amount}
                onChange={(e) => setForm({ ...form, amount: e.target.value })}
              />
              {!amountValid && form.amount !== '' && (
                <p className="text-xs text-destructive">Enter a dollar amount like 5.00.</p>
              )}
            </div>
            <div className="space-y-1">
              <Label htmlFor="pkg-credits">Credits</Label>
              <Input
                id="pkg-credits"
                inputMode="numeric"
                value={form.credits}
                onChange={(e) => setForm({ ...form, credits: e.target.value })}
              />
              {!creditsValid && form.credits !== '' && (
                <p className="text-xs text-destructive">Enter a positive whole number.</p>
              )}
            </div>
          </div>
          <div className="grid grid-cols-2 gap-4">
            <div className="space-y-1">
              <Label htmlFor="pkg-sort">Sort order</Label>
              <Input
                id="pkg-sort"
                inputMode="numeric"
                value={form.sort}
                onChange={(e) => setForm({ ...form, sort: e.target.value })}
              />
            </div>
            <label className="flex items-center gap-2 pt-6 text-sm">
              <input
                type="checkbox"
                checked={form.active}
                onChange={(e) => setForm({ ...form, active: e.target.checked })}
              />
              Active
            </label>
          </div>
          <div className="flex gap-2">
            <Button type="submit" disabled={submitting || !amountValid || !creditsValid}>
              {submitting ? 'Saving…' : 'Save'}
            </Button>
            <Button type="button" variant="ghost" onClick={onCancel}>
              Cancel
            </Button>
          </div>
        </form>
      </CardContent>
    </Card>
  );
}

// ── Payments ─────────────────────────────────────────────────────────────

function PaymentsTab() {
  const [status, setStatus] = useState<'' | AdminPayment['status']>('');

  const { data, isLoading, error, isPlaceholderData, page, setPage, totalPages } = usePagedQuery({
    queryKey: qk.admin.billing.payments(status),
    queryFn: ({ limit, offset }) =>
      api.getJson<AdminPaymentListResponse>('/api/v1/admin/billing/payments', {
        query: { limit, offset, ...(status ? { status } : {}) },
      }),
    perPage: PER_PAGE,
  });

  const columns: Column<AdminPayment>[] = [
    { header: 'ID', className: 'font-mono text-xs', cell: (p) => `${p.id.slice(0, 8)}…` },
    { header: 'User', className: 'font-mono text-xs', cell: (p) => `${p.user_id.slice(0, 8)}…` },
    { header: 'Amount', className: 'font-mono', cell: (p) => `$${formatCents(p.amount_cents)}` },
    {
      header: 'Credits',
      className: 'font-mono',
      cell: (p) => p.credits_expected.toLocaleString(),
    },
    { header: 'Status', cell: (p) => <PaymentStatusBadge status={p.status} /> },
    { header: 'Created', className: 'whitespace-nowrap text-xs', cell: (p) => fmtDate(p.created_at) },
  ];

  return (
    <div className="space-y-4">
      <div className="flex items-center gap-2">
        <select
          className="h-10 rounded-md border border-input bg-background px-3 text-sm"
          value={status}
          onChange={(e) => {
            setStatus(e.target.value as typeof status);
            setPage(1);
          }}
        >
          <option value="">all statuses</option>
          <option value="created">created</option>
          <option value="approved">approved</option>
          <option value="captured">captured</option>
          <option value="failed">failed</option>
          <option value="refunded">refunded</option>
        </select>
        {data && <span className="text-sm text-muted-foreground">{data.total} total</span>}
      </div>

      <Card>
        <CardContent className="overflow-x-auto pt-6">
          <DataTable
            columns={columns}
            rows={data?.data}
            rowKey={(p) => p.id}
            isLoading={isLoading}
            error={error}
            emptyText="No payments yet."
            isPlaceholder={isPlaceholderData}
          />
          {data && (
            <PaginationFooter
              page={page}
              totalPages={totalPages}
              isPlaceholderData={isPlaceholderData}
              onPageChange={setPage}
            />
          )}
        </CardContent>
      </Card>
    </div>
  );
}

// ── Settings ─────────────────────────────────────────────────────────────

function SettingsTab() {
  const toast = useToast();
  const { data, isLoading, error } = useQuery({
    queryKey: qk.admin.billing.settings(),
    queryFn: () => api.getJson<AdminBillingSettingsResponse>('/api/v1/admin/billing/settings'),
  });

  const save = useApiMutation(
    (body: { credits_per_unit: number; min_amount_cents: number; max_amount_cents: number }) =>
      api.putJson<AdminBillingSettingsResponse>('/api/v1/admin/billing/settings', { body }),
    {
      invalidate: [qk.admin.billing.settings()],
      onSuccess: () => toast.success('Settings saved.'),
    },
  );
  useErrorToast(save.error);

  if (isLoading) return <p className="text-sm text-muted-foreground">Loading…</p>;
  if (error || !data)
    return <p className="text-sm text-destructive">Could not load billing settings.</p>;

  return (
    <Card>
      <CardHeader>
        <CardTitle>Rate &amp; bounds</CardTitle>
      </CardHeader>
      <CardContent>
        <SettingsForm
          initial={data.data}
          submitting={save.isPending}
          onSubmit={(body) => save.mutate(body)}
        />
      </CardContent>
    </Card>
  );
}

function SettingsForm({
  initial,
  submitting,
  onSubmit,
}: {
  initial: AdminBillingSettings;
  submitting: boolean;
  onSubmit: (body: {
    credits_per_unit: number;
    min_amount_cents: number;
    max_amount_cents: number;
  }) => void;
}) {
  const [creditsPerUnit, setCreditsPerUnit] = useState(String(initial.credits_per_unit));
  const [minAmount, setMinAmount] = useState(formatCents(initial.min_amount_cents));
  const [maxAmount, setMaxAmount] = useState(formatCents(initial.max_amount_cents));

  const creditsValid = /^\d+$/.test(creditsPerUnit) && parseInt(creditsPerUnit, 10) > 0;
  const minCents = dollarsToCents(minAmount);
  const maxCents = dollarsToCents(maxAmount);
  const boundsValid = minCents !== null && maxCents !== null && minCents > 0 && maxCents >= minCents;

  const handleSubmit = (e: FormEvent) => {
    e.preventDefault();
    if (!creditsValid || !boundsValid || minCents === null || maxCents === null) return;
    onSubmit({
      credits_per_unit: parseInt(creditsPerUnit, 10),
      min_amount_cents: minCents,
      max_amount_cents: maxCents,
    });
  };

  return (
    <form onSubmit={handleSubmit} className="max-w-sm space-y-4">
      <div className="space-y-1">
        <Label htmlFor="settings-rate">Credits per 100 cents ($1.00)</Label>
        <Input
          id="settings-rate"
          inputMode="numeric"
          value={creditsPerUnit}
          onChange={(e) => setCreditsPerUnit(e.target.value)}
        />
      </div>
      <div className="space-y-1">
        <Label htmlFor="settings-min">Minimum top-up (USD)</Label>
        <Input
          id="settings-min"
          inputMode="decimal"
          value={minAmount}
          onChange={(e) => setMinAmount(e.target.value)}
        />
      </div>
      <div className="space-y-1">
        <Label htmlFor="settings-max">Maximum top-up (USD)</Label>
        <Input
          id="settings-max"
          inputMode="decimal"
          value={maxAmount}
          onChange={(e) => setMaxAmount(e.target.value)}
        />
      </div>
      {!boundsValid && (
        <p className="text-xs text-destructive">
          Enter valid dollar amounts with max ≥ min.
        </p>
      )}
      <Button type="submit" disabled={submitting || !creditsValid || !boundsValid}>
        {submitting ? 'Saving…' : 'Save settings'}
      </Button>
    </form>
  );
}

