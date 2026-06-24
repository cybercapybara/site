import { Alert, AlertDescription } from '@/components/ui/alert';
import { cn } from '@/lib/utils';

interface FormAlertProps {
  /** Error message (rendered destructive). Takes priority over `success`. */
  message?: string | null;
  /** Success message (rendered success) — shown only when there's no error. */
  success?: string | null;
}

/**
 * In-context form feedback that doesn't shove the fields around. Drop it as the
 * first child of a form. Instead of inserting an Alert into the flow on error
 * (which snaps every field down — the reported "layout jumps" jank), this slot
 * is always present and smoothly expands/collapses via a grid-rows transition.
 * `aria-live` announces the message to screen readers when it appears.
 */
export function FormAlert({ message, success }: FormAlertProps) {
  const isError = !!message;
  const text = message || success || '';
  const shown = !!text;
  return (
    <div
      role="status"
      aria-live={isError ? 'assertive' : 'polite'}
      className={cn(
        'grid transition-[grid-template-rows] duration-200 ease-out',
        shown ? 'grid-rows-[1fr]' : 'grid-rows-[0fr]',
      )}
    >
      <div className="overflow-hidden">
        {shown && (
          <Alert variant={isError ? 'destructive' : 'success'}>
            <AlertDescription>{text}</AlertDescription>
          </Alert>
        )}
      </div>
    </div>
  );
}
