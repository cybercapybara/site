import { useQuery } from '@tanstack/react-query';
import { Link } from 'react-router-dom';
import { Wallet } from 'lucide-react';

import { Button } from '@/components/ui/button';
import { Card, CardContent, CardDescription, CardHeader, CardTitle } from '@/components/ui/card';
import { useMe } from '@/hooks/useMe';
import { api } from '@/lib/api/client';
import { qk } from '@/lib/api/queryKeys';

export function ProfilePage() {
  const user = useMe().data ?? null;

  // Balance-only fetch of the own-wallet endpoint. Billing may be disabled
  // (billing.enabled=false), in which case this 404s — that's expected, not
  // an error to surface, so the card below only renders on isSuccess.
  const walletQ = useQuery({
    queryKey: qk.billing.wallet(),
    queryFn: () => api.getJson('/api/v1/billing/wallet', { query: { limit: 1 } }),
    enabled: !!user,
  });

  if (!user) return null;
  return (
    <div className="container mx-auto max-w-2xl py-8 space-y-6">
      <Card>
        <CardHeader>
          <CardTitle>Your account</CardTitle>
          <CardDescription>{user.email}</CardDescription>
        </CardHeader>
        <CardContent className="space-y-1 text-sm">
          <div>
            <span className="text-muted-foreground">Name: </span>
            {user.full_name || '(not set)'}
          </div>
          <div>
            <span className="text-muted-foreground">Role: </span>
            {user.role?.name ?? user.role_id}
          </div>
          <div>
            <span className="text-muted-foreground">Confirmed: </span>
            {user.confirmed ? 'yes' : 'no'}
          </div>
        </CardContent>
      </Card>
      {walletQ.isSuccess && (
        <Card>
          <CardHeader>
            <CardTitle className="flex items-center gap-2">
              <Wallet className="h-4 w-4" />
              Wallet
            </CardTitle>
            <CardDescription>
              {walletQ.data.data.balance.toLocaleString()} credits available
            </CardDescription>
          </CardHeader>
          <CardContent>
            <Button asChild>
              <Link to="/billing">
                {walletQ.data.data.balance > 0 ? 'Open wallet' : 'Buy credits'}
              </Link>
            </Button>
          </CardContent>
        </Card>
      )}
      <div className="grid gap-3 sm:grid-cols-3">
        <Button variant="outline" asChild>
          <Link to="/account/change-password">Change password</Link>
        </Button>
        <Button variant="outline" asChild>
          <Link to="/account/change-email">Change email</Link>
        </Button>
      </div>
    </div>
  );
}
