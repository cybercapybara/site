import { useState } from 'react';
import { Link, useNavigate } from 'react-router-dom';
import { LogOut, Moon, Sun } from 'lucide-react';

import { Button } from '@/components/ui/button';
import { BRAND } from '@/lib/brand';
import { useLogout } from '@/hooks/useAuthMutations';
import { useMe } from '@/hooks/useMe';
import { userCan } from '@/lib/auth/permissions';
import { routes, guardPermission, type RouteEntry } from '@/routes/manifest';

export function Nav() {
  const me = useMe();
  const user = me.data ?? null;
  const logout = useLogout();
  const navigate = useNavigate();

  // Minimal theme toggle: the .dark class drives Tailwind's dark: variants; the
  // initial class is set pre-paint by the inline script in index.html.
  const [dark, setDark] = useState(
    () => typeof document !== 'undefined' && document.documentElement.classList.contains('dark'),
  );
  const toggleTheme = () => {
    const next = !dark;
    setDark(next);
    document.documentElement.classList.toggle('dark', next);
    try {
      localStorage.setItem('theme', next ? 'dark' : 'light');
    } catch {
      /* ignore */
    }
  };

  // Show the logged-out auth buttons (Log in / Register) only once /me has
  // RESOLVED to "no session" — me.isSuccess && !user. Gating on isSuccess
  // (instead of just !isError) avoids the first-paint flash of "Log in /
  // Register" while /me is still pending, and still never claims the user
  // is logged out on a 5xx/network error (isSuccess stays false then).
  const showAuthButtons = me.isSuccess && !user;

  // Nav links come straight from the routes manifest — every route that
  // declares a navLabel, filtered by what this user is allowed to see.
  // One source of truth for routes and nav kills the route↔nav drift.
  const navLinks: RouteEntry[] = routes.filter(
    (r) => r.navLabel && userCan(user, guardPermission(r)),
  );

  return (
    <nav className="border-b">
      <div className="container mx-auto flex h-14 items-center justify-between">
        <div className="flex items-center gap-6">
          <Link to="/" className="font-semibold">
            {BRAND}
          </Link>
          <div className="flex items-center gap-4 text-sm">
            {navLinks.map((r) => {
              const Icon = r.navIcon;
              return (
                <Link
                  key={r.path}
                  to={r.path}
                  className="flex items-center gap-1 text-muted-foreground hover:text-foreground"
                >
                  {Icon && <Icon className="h-3.5 w-3.5" />}
                  {r.navLabel}
                </Link>
              );
            })}
          </div>
        </div>
        <div className="flex items-center gap-3 text-sm">
          <Button size="sm" variant="ghost" onClick={toggleTheme} aria-label="Toggle theme">
            {dark ? <Sun className="h-4 w-4" /> : <Moon className="h-4 w-4" />}
          </Button>
          {user && (
            <>
              <Link to="/account" className="text-muted-foreground hover:text-foreground">
                {user.full_name || user.email}
              </Link>
              <Button
                size="sm"
                variant="ghost"
                onClick={async () => {
                  await logout.mutateAsync();
                  navigate('/login');
                }}
              >
                <LogOut className="h-4 w-4" />
              </Button>
            </>
          )}
          {showAuthButtons && (
            <>
              <Button size="sm" variant="ghost" asChild>
                <Link to="/login">Log in</Link>
              </Button>
              <Button size="sm" asChild>
                <Link to="/register">Register</Link>
              </Button>
            </>
          )}
        </div>
      </div>
    </nav>
  );
}
