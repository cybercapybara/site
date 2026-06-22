import { useState } from 'react';
import { useForm } from 'react-hook-form';
import { zodResolver } from '@hookform/resolvers/zod';
import type { z } from 'zod';

import { Alert, AlertDescription } from '@/components/ui/alert';
import { Button } from '@/components/ui/button';
import { FormField } from '@/components/FormField';
import { Card, CardContent, CardDescription, CardHeader, CardTitle } from '@/components/ui/card';
import { api, apiErrorMessage } from '@/lib/api/client';
import { changeEmailSchema } from '@/lib/schemas/auth';

type FormValues = z.infer<typeof changeEmailSchema>;

export function ChangeEmailPage() {
  const [sent, setSent] = useState(false);
  const [serverError, setServerError] = useState<string | null>(null);
  const {
    register,
    handleSubmit,
    formState: { errors, isSubmitting },
  } = useForm<FormValues>({ resolver: zodResolver(changeEmailSchema) });

  const onSubmit = handleSubmit(async (values) => {
    setServerError(null);
    const { error } = await api.POST('/api/account/change-email-request', { body: values });
    if (error) {
      setServerError(apiErrorMessage(error));
      return;
    }
    setSent(true);
  });

  return (
    <div className="container mx-auto max-w-md py-12">
      <Card>
        <CardHeader>
          <CardTitle>Change your email</CardTitle>
          <CardDescription>
            We'll send a confirmation link to the new address. Your current email stays active
            until you click it.
          </CardDescription>
        </CardHeader>
        <CardContent>
          {sent ? (
            <Alert variant="success">
              <AlertDescription>
                Confirmation email queued. Check the new address for a link.
              </AlertDescription>
            </Alert>
          ) : (
            <form className="space-y-4" onSubmit={onSubmit}>
              {serverError && (
                <Alert variant="destructive">
                  <AlertDescription>{serverError}</AlertDescription>
                </Alert>
              )}
              <FormField
                id="new_email"
                type="email"
                label="New email"
                error={errors.new_email?.message}
                {...register('new_email')}
              />
              <FormField
                id="password"
                type="password"
                label="Confirm with current password"
                error={errors.password?.message}
                {...register('password')}
              />
              <Button type="submit" className="w-full" disabled={isSubmitting}>
                Send confirmation link
              </Button>
            </form>
          )}
        </CardContent>
      </Card>
    </div>
  );
}
