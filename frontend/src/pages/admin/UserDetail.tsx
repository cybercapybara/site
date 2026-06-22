import { Link, useNavigate, useParams } from 'react-router-dom';
import { useQuery } from '@tanstack/react-query';

import { RoleSelect } from '@/components/RoleSelect';
import { Alert, AlertDescription } from '@/components/ui/alert';
import { Button } from '@/components/ui/button';
import { Card, CardContent, CardHeader, CardTitle } from '@/components/ui/card';
import { Input } from '@/components/ui/input';
import { Label } from '@/components/ui/label';
import { useApiMutation } from '@/hooks/useApiMutation';
import { useMe } from '@/hooks/useMe';
import { api } from '@/lib/api/client';
import { qk } from '@/lib/api/queryKeys';
import type { UserDetailResponse } from '@/lib/api/types';

export function AdminUserDetailPage() {
  const { id = '' } = useParams<{ id: string }>();
  const navigate = useNavigate();
  // Query-backed via the TanStack Query cache: the cache is empty for one
  // paint after a hard reload, which would briefly disable the
  // self-protection UI.
  const me = useMe().data ?? null;

  const userQ = useQuery({
    queryKey: qk.admin.user(id),
    queryFn: () => api.getJson<UserDetailResponse>('/api/admin/users/' + id),
  });

  const update = useApiMutation(
    (patch: Record<string, unknown>) =>
      api.patchJson<UserDetailResponse>('/api/admin/users/' + id, { body: patch }),
    { invalidate: [qk.admin.user(id), qk.admin.users()] },
  );

  const remove = useApiMutation(() => api.deleteJson('/api/admin/users/' + id), {
    invalidate: [qk.admin.users()],
    onSuccess: () => navigate('/admin/users'),
  });

  const error = update.error ?? remove.error;

  if (userQ.isLoading) return <p className="container py-12">Loading…</p>;
  if (userQ.error || !userQ.data)
    return <p className="container py-12 text-destructive">User not found.</p>;

  const user = userQ.data.data;
  const isSelf = me?.id === user.id;

  return (
    <div className="container mx-auto max-w-2xl py-12 space-y-6">
      <div className="flex items-center justify-between">
        <h1 className="text-2xl font-bold">{user.email}</h1>
        <Button variant="ghost" asChild>
          <Link to="/admin/users">← Back</Link>
        </Button>
      </div>
      {error && (
        <Alert variant="destructive">
          <AlertDescription>{error}</AlertDescription>
        </Alert>
      )}
      {update.isSuccess && !error && (
        <Alert variant="success">
          <AlertDescription>Changes saved.</AlertDescription>
        </Alert>
      )}
      <Card>
        <CardHeader>
          <CardTitle>Details</CardTitle>
        </CardHeader>
        <CardContent className="space-y-4">
          <form
            onSubmit={(e) => {
              e.preventDefault();
              const fd = new FormData(e.currentTarget);
              const patch: Record<string, unknown> = {};
              const newEmail = String(fd.get('email') || '');
              const newRoleId = Number(fd.get('role_id'));
              const newFirst = String(fd.get('first_name') || '');
              const newLast = String(fd.get('last_name') || '');
              if (newEmail && newEmail !== user.email) patch.email = newEmail;
              if (newRoleId && newRoleId !== user.role_id) patch.role_id = newRoleId;
              if (newFirst !== (user.first_name ?? '')) patch.first_name = newFirst;
              if (newLast !== (user.last_name ?? '')) patch.last_name = newLast;
              if (Object.keys(patch).length === 0) return;
              update.mutate(patch);
            }}
            className="space-y-3"
          >
            <div className="space-y-2">
              <Label htmlFor="email">Email</Label>
              <Input id="email" name="email" defaultValue={user.email} />
            </div>
            <div className="grid grid-cols-2 gap-3">
              <div className="space-y-2">
                <Label htmlFor="first_name">First name</Label>
                <Input id="first_name" name="first_name" defaultValue={user.first_name ?? ''} />
              </div>
              <div className="space-y-2">
                <Label htmlFor="last_name">Last name</Label>
                <Input id="last_name" name="last_name" defaultValue={user.last_name ?? ''} />
              </div>
            </div>
            <div className="space-y-2">
              <Label htmlFor="role_id">Role</Label>
              <RoleSelect
                id="role_id"
                name="role_id"
                defaultValue={user.role_id}
                disabled={isSelf}
              />
              {isSelf && (
                <p className="text-xs text-muted-foreground">
                  You cannot change the role of your own account.
                </p>
              )}
            </div>
            <Button type="submit" disabled={update.isPending}>
              {update.isPending ? 'Saving…' : 'Save changes'}
            </Button>
          </form>
        </CardContent>
      </Card>
      <Card>
        <CardHeader>
          <CardTitle className="text-destructive">Danger zone</CardTitle>
        </CardHeader>
        <CardContent>
          <Button
            variant="destructive"
            disabled={isSelf}
            onClick={() => {
              if (confirm(`Delete user ${user.email}? This cannot be undone.`)) remove.mutate();
            }}
          >
            Delete user
          </Button>
          {isSelf && (
            <p className="text-xs text-muted-foreground mt-2">
              You cannot delete your own account; ask another admin.
            </p>
          )}
        </CardContent>
      </Card>
    </div>
  );
}
