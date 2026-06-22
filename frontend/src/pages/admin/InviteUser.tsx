import { useForm } from 'react-hook-form';
import { zodResolver } from '@hookform/resolvers/zod';
import { useNavigate } from 'react-router-dom';

import { RoleSelect } from '@/components/RoleSelect';
import { Alert, AlertDescription } from '@/components/ui/alert';
import { Button } from '@/components/ui/button';
import { Card, CardContent, CardDescription, CardHeader, CardTitle } from '@/components/ui/card';
import { Input } from '@/components/ui/input';
import { Label } from '@/components/ui/label';
import { useApiMutation } from '@/hooks/useApiMutation';
import { api } from '@/lib/api/client';
import { qk } from '@/lib/api/queryKeys';
import type { InviteResponse } from '@/lib/api/types';
import { inviteUserSchema, type InviteUserValues } from '@/lib/schemas/admin';

export function AdminInviteUserPage() {
  const navigate = useNavigate();
  const {
    register,
    handleSubmit,
    formState: { errors, isSubmitting },
  } = useForm<InviteUserValues>({ resolver: zodResolver(inviteUserSchema) });

  // useApiMutation owns the error banner + invalidation: a fresh invite
  // creates a user row, so the admin users list must be re-fetched (any
  // paged variant is prefix-matched by qk.admin.users()).
  const invite = useApiMutation(
    (values: InviteUserValues) =>
      api.postJson<InviteResponse>('/api/admin/invite', {
        body: {
          email: values.email,
          first_name: values.first_name || undefined,
          last_name: values.last_name || undefined,
          role_id: values.role_id ? Number(values.role_id) : undefined,
        },
      }),
    {
      invalidate: [qk.admin.users()],
      onSuccess: () => navigate('/admin/users'),
    },
  );

  const onSubmit = handleSubmit((values) => invite.mutate(values));

  return (
    <div className="container mx-auto max-w-md py-12">
      <Card>
        <CardHeader>
          <CardTitle>Invite a user</CardTitle>
          <CardDescription>
            They'll get an email with a link to set their own password.
          </CardDescription>
        </CardHeader>
        <CardContent>
          <form className="space-y-4" onSubmit={onSubmit}>
            {invite.error && (
              <Alert variant="destructive">
                <AlertDescription>{invite.error}</AlertDescription>
              </Alert>
            )}
            <div className="space-y-2">
              <Label htmlFor="email">Email</Label>
              <Input id="email" type="email" {...register('email')} />
              {errors.email && (
                <p className="text-sm text-destructive">{errors.email.message}</p>
              )}
            </div>
            <div className="grid grid-cols-2 gap-3">
              <div className="space-y-2">
                <Label htmlFor="first_name">First name</Label>
                <Input id="first_name" {...register('first_name')} />
                {errors.first_name && (
                  <p className="text-sm text-destructive">{errors.first_name.message}</p>
                )}
              </div>
              <div className="space-y-2">
                <Label htmlFor="last_name">Last name</Label>
                <Input id="last_name" {...register('last_name')} />
                {errors.last_name && (
                  <p className="text-sm text-destructive">{errors.last_name.message}</p>
                )}
              </div>
            </div>
            <div className="space-y-2">
              <Label htmlFor="role_id">Role</Label>
              <RoleSelect id="role_id" includeDefaultOption {...register('role_id')} />
            </div>
            <Button type="submit" className="w-full" disabled={isSubmitting || invite.isPending}>
              Send invitation
            </Button>
          </form>
        </CardContent>
      </Card>
    </div>
  );
}
