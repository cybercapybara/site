import { useState } from 'react';
import { useForm } from 'react-hook-form';
import { zodResolver } from '@hookform/resolvers/zod';
import { Link, useParams } from 'react-router-dom';
import type { z } from 'zod';

import { Alert, AlertDescription } from '@/components/ui/alert';
import { Button } from '@/components/ui/button';
import { FormField } from '@/components/FormField';
import { Card, CardContent, CardHeader, CardTitle } from '@/components/ui/card';
import { api, apiErrorMessage } from '@/lib/api/client';
import { resetPasswordSchema } from '@/lib/schemas/auth';

type FormValues = z.infer<typeof resetPasswordSchema>;

export function ResetPasswordPage() {
  const { token = '' } = useParams<{ token: string }>();
  const [done, setDone] = useState(false);
  const [serverError, setServerError] = useState<string | null>(null);
  const {
    register,
    handleSubmit,
    formState: { errors, isSubmitting },
  } = useForm<FormValues>({ resolver: zodResolver(resetPasswordSchema) });

  const onSubmit = handleSubmit(async (values) => {
    setServerError(null);
    const { error } = await api.POST(
      '/api/account/reset-password/' + encodeURIComponent(token),
      { body: { new_password: values.new_password } },
    );
    if (error) {
      setServerError(apiErrorMessage(error, 'This reset link is invalid or has expired.'));
      return;
    }
    setDone(true);
  });

  return (
    <div className="container mx-auto max-w-md py-12">
      <Card>
        <CardHeader>
          <CardTitle>Set a new password</CardTitle>
        </CardHeader>
        <CardContent>
          {done ? (
            <div className="space-y-4">
              <Alert variant="success">
                <AlertDescription>Password updated. You can log in now.</AlertDescription>
              </Alert>
              <Button asChild className="w-full">
                <Link to="/login">Continue to log in</Link>
              </Button>
            </div>
          ) : (
            <form className="space-y-4" onSubmit={onSubmit}>
              {serverError && (
                <Alert variant="destructive">
                  <AlertDescription>{serverError}</AlertDescription>
                </Alert>
              )}
              <FormField
                id="new_password"
                type="password"
                label="New password"
                error={errors.new_password?.message}
                {...register('new_password')}
              />
              <FormField
                id="new_password_confirm"
                type="password"
                label="Confirm new password"
                error={errors.new_password_confirm?.message}
                {...register('new_password_confirm')}
              />
              <Button type="submit" className="w-full" disabled={isSubmitting}>
                Update password
              </Button>
            </form>
          )}
        </CardContent>
      </Card>
    </div>
  );
}
