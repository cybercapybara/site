import { useState, type FormEvent } from 'react';

import { Modal } from '@/components/Modal';
import { Button } from '@/components/ui/button';
import { Card, CardContent, CardHeader, CardTitle } from '@/components/ui/card';
import { Input } from '@/components/ui/input';
import { Label } from '@/components/ui/label';
import { useToast } from '@/components/ui/toaster';
import { useApiMutation } from '@/hooks/useApiMutation';
import { useErrorToast } from '@/hooks/useErrorToast';
import { api } from '@/lib/api/client';
import { qk } from '@/lib/api/queryKeys';

interface AdminAdjustResponse {
  data: { balance: number; credited: boolean };
}

const UUID_RE = /^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/i;

/**
 * Manual wallet-adjustment dialog. Used from two places:
 *   - the Billing admin page, with a free-text User ID field, and
 *   - a user's detail page (/admin/users/:id), where `initialUserId`
 *     pre-fills and locks the target so an admin never has to copy a UUID
 *     out of the address bar.
 *
 * `notify` maps to the endpoint's optional notify flag: when checked, the
 * target user is emailed a best-effort adjustment notice carrying the note
 * as the reason. `note` is mandatory either way (it is the audit-log reason).
 */
export function AdjustDialog({
  onClose,
  initialUserId,
  userLabel,
}: {
  onClose: () => void;
  /** Pre-fill + lock the target user (from a user detail page). */
  initialUserId?: string;
  /** Human label (email/name) shown when the user is locked. */
  userLabel?: string;
}) {
  const toast = useToast();
  const locked = !!initialUserId;
  const [userId, setUserId] = useState(initialUserId ?? '');
  const [delta, setDelta] = useState('');
  const [note, setNote] = useState('');
  const [notify, setNotify] = useState(false);

  const adjust = useApiMutation(
    (vars: { userId: string; delta_credits: number; note: string; notify: boolean }) =>
      api.postJson<AdminAdjustResponse>(`/api/v1/admin/billing/users/${vars.userId}/adjust`, {
        body: { delta_credits: vars.delta_credits, note: vars.note, notify: vars.notify },
      }),
    {
      invalidate: [qk.admin.billing.payments()],
      onSuccess: (res) => {
        const suffix = notify ? ' — user notified' : '';
        toast.success(
          `Adjusted. New balance: ${res.data.balance.toLocaleString()} credits${suffix}.`,
        );
        onClose();
      },
    },
  );
  useErrorToast(adjust.error);

  const deltaNum = parseInt(delta, 10);
  const deltaValid = /^-?\d+$/.test(delta) && deltaNum !== 0;
  const uuidValid = UUID_RE.test(userId);
  const noteValid = note.trim().length > 0;

  const handleSubmit = (e: FormEvent) => {
    e.preventDefault();
    if (!deltaValid || !uuidValid || !noteValid) return;
    adjust.mutate({ userId, delta_credits: deltaNum, note: note.trim(), notify });
  };

  return (
    <Modal onClose={onClose} className="max-w-md">
      <Card>
        <CardHeader>
          <CardTitle>Adjust balance</CardTitle>
        </CardHeader>
        <CardContent>
          <form onSubmit={handleSubmit} className="space-y-4">
            {locked ? (
              <div className="space-y-1">
                <Label>User</Label>
                <p className="text-sm">{userLabel ?? userId}</p>
                <p className="font-mono text-xs text-muted-foreground">{userId}</p>
              </div>
            ) : (
              <div className="space-y-1">
                <Label htmlFor="adjust-user">User ID</Label>
                <Input
                  id="adjust-user"
                  className="font-mono text-xs"
                  placeholder="uuid — see /admin/users"
                  value={userId}
                  onChange={(e) => setUserId(e.target.value.trim())}
                />
                {!uuidValid && userId !== '' && (
                  <p className="text-xs text-destructive">Not a valid user id.</p>
                )}
              </div>
            )}
            <div className="space-y-1">
              <Label htmlFor="adjust-delta">Delta credits (signed; negative debits)</Label>
              <Input
                id="adjust-delta"
                inputMode="numeric"
                placeholder="e.g. 100 or -50"
                value={delta}
                onChange={(e) => setDelta(e.target.value)}
                autoFocus={locked}
              />
              {!deltaValid && delta !== '' && (
                <p className="text-xs text-destructive">
                  Enter a non-zero whole number (negative to debit).
                </p>
              )}
            </div>
            <div className="space-y-1">
              <Label htmlFor="adjust-note">Note (reason)</Label>
              <Input
                id="adjust-note"
                required
                maxLength={2000}
                placeholder="why — stored in the audit log"
                value={note}
                onChange={(e) => setNote(e.target.value)}
              />
            </div>
            <label className="flex items-center gap-2 text-sm text-muted-foreground">
              <input
                type="checkbox"
                className="h-4 w-4"
                checked={notify}
                onChange={(e) => setNotify(e.target.checked)}
              />
              Email the user this adjustment (reason = note)
            </label>
            <div className="flex gap-2">
              <Button
                type="submit"
                disabled={adjust.isPending || !deltaValid || !uuidValid || !noteValid}
              >
                {adjust.isPending ? 'Adjusting…' : 'Adjust'}
              </Button>
              <Button type="button" variant="ghost" onClick={onClose}>
                Cancel
              </Button>
            </div>
          </form>
        </CardContent>
      </Card>
    </Modal>
  );
}
