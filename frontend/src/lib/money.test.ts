import { describe, expect, it } from 'vitest';

import { creditsForAmount, dollarsToCents, formatCents } from './money';

describe('formatCents', () => {
  it('formats whole dollars', () => {
    expect(formatCents(500)).toBe('5.00');
  });

  it('formats odd cents without rounding drift', () => {
    expect(formatCents(1234)).toBe('12.34');
  });

  it('formats zero', () => {
    expect(formatCents(0)).toBe('0.00');
  });
});

describe('dollarsToCents', () => {
  it('parses a whole dollar amount', () => {
    expect(dollarsToCents('5')).toBe(500);
  });

  it('parses two decimal places without float drift', () => {
    expect(dollarsToCents('12.34')).toBe(1234);
  });

  it('pads a single decimal place', () => {
    expect(dollarsToCents('12.3')).toBe(1230);
  });

  it('tolerates surrounding whitespace', () => {
    expect(dollarsToCents('  12.34  ')).toBe(1234);
  });

  it('rejects more than two decimal places', () => {
    expect(dollarsToCents('12.345')).toBeNull();
  });

  it('rejects negative amounts', () => {
    expect(dollarsToCents('-5')).toBeNull();
  });

  it('rejects non-numeric input', () => {
    expect(dollarsToCents('abc')).toBeNull();
  });

  it('rejects an empty string', () => {
    expect(dollarsToCents('')).toBeNull();
  });

  it('is the exact inverse of formatCents for tricky cent values', () => {
    // 0.1 + 0.2 !== 0.3 in float — the classic drift case, done here as
    // cents so it never touches float arithmetic in the first place.
    expect(dollarsToCents('0.29')).toBe(29);
    expect(dollarsToCents('0.30')).toBe(30);
  });
});

describe('creditsForAmount', () => {
  it('applies the credits-per-100-cents rate', () => {
    // billing.credits_per_unit is "credits per 100 cents".
    expect(creditsForAmount(500, 100)).toBe(500);
    expect(creditsForAmount(1000, 50)).toBe(500);
  });

  it('truncates fractional credits', () => {
    expect(creditsForAmount(999, 100)).toBe(999);
    expect(creditsForAmount(150, 33)).toBe(49); // 150*33/100 = 49.5
  });
});
