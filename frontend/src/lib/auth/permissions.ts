/**
 * Permission bit catalogue — THE frontend mirror of Domain::Permission::k*
 * in src/domain/Role.hpp on the backend. Single source: the Permission
 * constants and the admin Roles page checkbox list are both derived from
 * here. Add a row when the C++ side carves out a new bit.
 */

import type { User } from '../api/types';

export interface PermissionBit {
  bit: number;
  label: string;
  hint?: string;
}

export const PERMISSION_BITS: PermissionBit[] = [
  { bit: 0x01, label: 'General', hint: 'Baseline access for any signed-in user' },
  { bit: 0x02, label: 'Audit read', hint: 'Read the audit trail (GET /api/admin/audit)' },
  // Bits 0x04..0x80 are NOT carved out on the backend yet — only kGeneral
  // (0x01), kAuditRead (0x02) and kAdminister (0xff) exist in
  // Domain::Permission (src/domain/Role.hpp). Add a row here the moment you
  // define a new kPermission bit there; do not expose checkboxes for bits the
  // backend can't authorise.
];

/** Named bits — mirrors Domain::Permission::k* exactly. */
export const Permission = {
  None: 0x00,
  General: 0x01,
  /** Read the audit trail — Domain::Permission::kAuditRead. */
  AuditRead: 0x02,
  /** All bits — Domain::Permission::kAdminister. */
  Administer: 0xff,
} as const;

/** True when the user's role carries ALL requested permission bits. */
export function userCan(user: User | null | undefined, permission: number): boolean {
  if (!user || !user.role) return false;
  return (user.role.permissions & permission) === permission;
}

export function userIsAdmin(user: User | null | undefined): boolean {
  return userCan(user, Permission.Administer);
}
