import { describe, expect, it } from 'vitest';

import type { User } from '../api/types';
import { Permission, PERMISSION_BITS, userCan, userIsAdmin } from './permissions';

function userWith(permissions: number): User {
  return {
    id: 'u1',
    email: 'a@b.c',
    full_name: 'A',
    confirmed: true,
    role_id: 1,
    role: { id: 1, name: 'r', permissions, is_default: false },
  } as User;
}

describe('permission bitmask (mirror of Domain::Permission)', () => {
  it('Administer is 0xff and every catalogue bit is within it', () => {
    // Administer == all bits set, independent of which individual bits are
    // carved out — so it's NOT the OR-fold of the (currently partial) catalogue.
    expect(Permission.Administer).toBe(0xff);
    for (const b of PERMISSION_BITS) {
      expect(Permission.Administer & b.bit).toBe(b.bit);
    }
  });

  it('userCan requires ALL requested bits', () => {
    expect(userCan(userWith(Permission.General), Permission.General)).toBe(true);
    expect(userCan(userWith(Permission.General), Permission.Administer)).toBe(false);
    expect(userCan(userWith(0x03), 0x02)).toBe(true);
    expect(userCan(userWith(0x03), 0x05)).toBe(false); // 0x04 missing
  });

  it('userIsAdmin only for the full mask', () => {
    expect(userIsAdmin(userWith(0xff))).toBe(true);
    expect(userIsAdmin(userWith(0xfe))).toBe(false);
    expect(userIsAdmin(null)).toBe(false);
  });

  it('no role / no user → no permissions', () => {
    expect(userCan(null, Permission.General)).toBe(false);
    expect(userCan({ ...userWith(1), role: undefined } as unknown as User, 1)).toBe(false);
  });
});
