/**
 * Flat domain-type aliases over the generated OpenAPI tree.
 *
 * openapi-typescript emits indexed types (`components['schemas']['User']`)
 * into schema.gen.ts. Call sites want flat names (`User`, `Job`, …), so
 * this module re-exports thin aliases — change a shape in
 * docs/openapi.yaml, run `npm run gen:api`, and every page picks it up.
 *
 * Everything here is now generated: the me / user-detail / roles / invite
 * response envelopes earned named `components/schemas` entries in
 * docs/openapi.yaml, so there are no hand-written envelope types left to
 * drift against the backend.
 */
import type { components } from './schema.gen';

type Schemas = components['schemas'];

export type Role = Schemas['Role'];
export type User = Schemas['User'];
export type Job = Schemas['Job'];
export type AuditEntry = Schemas['AuditEntry'];
export type AuditListResponse = Schemas['AuditListResponse'];
export type UserListResponse = Schemas['UserListResponse'];
export type JobListResponse = Schemas['JobListResponse'];
export type DlqListResponse = Schemas['DlqListResponse'];
export type JobCreate = Schemas['JobCreate'];

/** GET /api/auth/me, POST /api/auth/login, POST /api/auth/refresh — { user }. */
export type MeResponse = Schemas['MeResponse'];
/** GET/PATCH /api/admin/users/{id}, POST /api/admin/users — { data: User }. */
export type UserDetailResponse = Schemas['UserDetailResponse'];
/** GET /api/admin/roles — { data: Role[] }. */
export type RolesResponse = Schemas['RolesResponse'];
/** POST/PATCH /api/admin/roles[/{id}] — { data: Role }. */
export type RoleDetailResponse = Schemas['RoleDetailResponse'];
/** POST /api/admin/invite — { data: User, message? }. */
export type InviteResponse = Schemas['InviteResponse'];
/** Generic { message } envelope (logout, delete, …). */
export type MessageResponse = Schemas['MessageResponse'];

/**
 * Billing (Task 8 wallet/PayPal top-up). credits_per_unit/min/max come back
 * on the SAME GET /billing/packages response as the package list — see
 * BillingPackageListResponse. WalletResponse.data.balance and every
 * delta_credits are integer "credits" (the internal wallet unit), never
 * divided by 100; amount_cents fields are real-world USD cents — see
 * frontend/src/lib/money.ts for the render-boundary conversion rules.
 *
 * NOTE: the admin billing schemas (AdminPackage*, Payment, BillingSettings*,
 * AdjustResponse) are intentionally NOT aliased here — docs/openapi.yaml has
 * carried them since Task 6 (commit 25737f9) but schema.gen.ts hasn't been
 * regenerated since Task 6/7 landed, so `components['schemas']` doesn't have
 * them yet. frontend/src/pages/admin/Billing.tsx hand-types those locally
 * until the autofix workflow (regenerate OpenAPI TS schema) is dispatched.
 */
export type BillingPackage = Schemas['BillingPackage'];
export type BillingPackageListResponse = Schemas['BillingPackageListResponse'];
export type PublicWalletEntry = Schemas['PublicWalletEntry'];
export type WalletResponse = Schemas['WalletResponse'];
export type TopupResponse = Schemas['TopupResponse'];
export type CaptureResponse = Schemas['CaptureResponse'];
