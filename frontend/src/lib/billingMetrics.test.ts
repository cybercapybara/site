import { describe, expect, it } from 'vitest';

import { formatBucketTick, formatCompactUsd } from './billingMetrics';

/**
 * The admin billing Overview tab (pages/admin/billing/Overview.tsx) renders
 * a chart + tables and has no @testing-library in this project's stack (see
 * JoinFromInvite.test.ts) — so, mirroring money.test.ts, this tests the
 * load-bearing PURE formatting logic the page delegates to:
 *   - formatCompactUsd  → the stat-tile headline value (cents → $ string,
 *                         auto-compact above $1,000 per the dataviz skill's
 *                         stat-tile contract).
 *   - formatBucketTick  → the revenue chart's x-axis tick label, which must
 *                         stay UTC-deterministic regardless of the runtime's
 *                         locale/timezone (bucket_start is UTC per
 *                         openapi.yaml).
 */

describe('formatCompactUsd', () => {
  it('formats whole dollars exactly under the $1,000 compact threshold', () => {
    expect(formatCompactUsd(500)).toBe('$5.00');
  });

  it('formats zero', () => {
    expect(formatCompactUsd(0)).toBe('$0.00');
  });

  it('formats odd cents without rounding drift, still under threshold', () => {
    expect(formatCompactUsd(99999)).toBe('$999.99'); // $999.99, just under $1,000
  });

  it('formats a negative amount (e.g. a net-negative window) exactly', () => {
    expect(formatCompactUsd(-250)).toBe('$-2.50');
  });

  it('switches to compact notation at and above $1,000', () => {
    const result = formatCompactUsd(150000); // $1,500.00
    expect(result.startsWith('$')).toBe(true);
    expect(result).toMatch(/1\.5/);
    expect(result).toMatch(/K/i);
  });

  it('compacts large revenue windows to millions', () => {
    const result = formatCompactUsd(450_000_000); // $4,500,000.00
    expect(result.startsWith('$')).toBe(true);
    expect(result).toMatch(/4\.5/);
    expect(result).toMatch(/M/i);
  });
});

describe('formatBucketTick', () => {
  it('formats an hourly bucket (period=day) as UTC HH:00, ignoring local timezone', () => {
    expect(formatBucketTick('2026-08-10T14:30:00Z', 'day')).toBe('14:00');
  });

  it('zero-pads single-digit UTC hours', () => {
    expect(formatBucketTick('2026-08-10T05:00:00Z', 'day')).toBe('05:00');
  });

  it('formats a daily bucket (period=week) as "Mon D" in UTC', () => {
    expect(formatBucketTick('2026-08-05T00:00:00Z', 'week')).toBe('Aug 5');
  });

  it('formats a daily bucket (period=month) the same way as week', () => {
    expect(formatBucketTick('2026-01-31T00:00:00Z', 'month')).toBe('Jan 31');
  });

  it('falls back to the raw string for an unparseable bucket_start', () => {
    expect(formatBucketTick('not-a-date', 'day')).toBe('not-a-date');
  });
});
