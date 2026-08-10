import { useEffect, useRef, type ReactNode } from 'react';
import { Link, useSearchParams } from 'react-router-dom';
import { CheckCircle2, Clock } from 'lucide-react';

import { Button } from '@/components/ui/button';
import { Card, CardContent, CardDescription, CardHeader, CardTitle } from '@/components/ui/card';
import { useApiMutation } from '@/hooks/useApiMutation';
import { api } from '@/lib/api/client';

interface StatusView {
  icon: ReactNode;
  title: string;
  description: ReactNode;
}

const PENDING_ICON = <Clock className="h-5 w-5 text-amber-500" />;

/**
 * PayPal redirect target after an approved checkout (billing.paypal.return_url,
 * see docs/CONFIG.md — defaults to {app.base_url}/billing/return). PayPal
 * appends the order id as `?token=<order_id>`, not `order_id` — see the
 * PayPal Orders v2 docs; `PayerID` is also appended but unused here.
 *
 * Capture is documented as idempotent (docs/openapi.yaml's
 * POST /billing/capture: "capturing an already-captured order returns
 * credited=false with the unchanged balance instead of calling PayPal
 * again"), so firing it once on mount is safe.
 *
 * Whatever goes wrong here — a network error, PayPal still showing PENDING,
 * a stale token — the user must NEVER be told the money is lost: an
 * already-approved order either completes via this call or via the
 * webhook (see BillingController's webhook handler), so the fallback copy
 * always points at that safety net instead.
 */
export function BillingReturnPage() {
  const [params] = useSearchParams();
  const token = params.get('token');

  const capture = useApiMutation((orderId: string) =>
    api.postJson('/api/v1/billing/capture', { body: { order_id: orderId } }),
  );

  const fired = useRef(false);
  useEffect(() => {
    if (fired.current || !token) return;
    fired.current = true;
    capture.mutate(token);
  }, [token, capture]);

  let view: StatusView;
  if (!token) {
    view = {
      icon: PENDING_ICON,
      title: 'Missing payment reference',
      description:
        "This page is meant to be reached from a PayPal redirect and is missing the expected reference. If you completed a payment, your wallet balance will update as soon as PayPal confirms it.",
    };
  } else if (capture.isError) {
    view = {
      icon: PENDING_ICON,
      title: "Couldn't confirm the payment yet",
      description: (
        <>
          {capture.error ?? 'The payment could not be confirmed right now'} — if PayPal approved
          the payment, it will still be
          credited automatically once confirmed (we also verify every payment independently via
          PayPal's webhook). Nothing has been lost; check your balance again in a minute.
        </>
      ),
    };
  } else if (capture.data) {
    // status === 'captured' covers BOTH a fresh credit (credited: true) and
    // an idempotent replay of an already-credited order (credited: false,
    // status still reports 'captured') — either way the balance is current.
    // Checked via `capture.data` truthiness (not `isSuccess`) so this
    // narrows `data` without depending on useApiMutation's wrapped return
    // type preserving TanStack Query's discriminated union.
    const result = capture.data.data;
    view =
      result.status === 'captured'
        ? {
            icon: <CheckCircle2 className="h-5 w-5 text-emerald-500" />,
            title: 'Payment successful',
            description: (
              <>
                Your new balance is{' '}
                <span className="font-semibold text-foreground">
                  {result.balance.toLocaleString()} credits
                </span>
                .
              </>
            ),
          }
        : {
            icon: PENDING_ICON,
            title: 'Payment still processing',
            description: (
              <>
                PayPal reports this payment as <span className="font-mono">{result.status}</span>.
                If it completes, your wallet will be credited automatically — we verify every
                payment independently via PayPal's webhook, so nothing is lost even if this page
                doesn't wait for it. Check your balance again shortly.
              </>
            ),
          };
  } else {
    view = {
      icon: <Clock className="h-5 w-5 animate-pulse" />,
      title: 'Confirming your payment…',
      description: 'This only takes a moment.',
    };
  }

  return (
    <div className="container mx-auto max-w-lg py-8">
      <Card>
        <CardHeader>
          <CardTitle className="flex items-center gap-2">
            {view.icon}
            {view.title}
          </CardTitle>
          <CardDescription className="pt-1">{view.description}</CardDescription>
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
