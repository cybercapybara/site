import { Link } from 'react-router-dom';
import { XCircle } from 'lucide-react';

import { Button } from '@/components/ui/button';
import { Card, CardContent, CardDescription, CardHeader, CardTitle } from '@/components/ui/card';

/**
 * PayPal redirect target when the buyer cancels checkout on PayPal's side
 * (billing.paypal.cancel_url, see docs/CONFIG.md — defaults to
 * {app.base_url}/billing/cancel). No API call happens here: an order that
 * was never approved was never captured, so there is nothing to reconcile —
 * the payment simply stays in `created`/`approved` and is never charged.
 */
export function BillingCancelPage() {
  return (
    <div className="container mx-auto max-w-lg py-8">
      <Card>
        <CardHeader>
          <CardTitle className="flex items-center gap-2">
            <XCircle className="h-5 w-5 text-muted-foreground" />
            Payment cancelled
          </CardTitle>
          <CardDescription className="pt-1">
            Nothing was charged. You can start a new top-up any time.
          </CardDescription>
        </CardHeader>
        <CardContent>
          <Button asChild>
            <Link to="/billing">Back to wallet</Link>
          </Button>
        </CardContent>
      </Card>
    </div>
  );
}
