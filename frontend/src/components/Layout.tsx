import { Outlet } from 'react-router-dom';

import { Nav } from './Nav';
import { useMe } from '@/hooks/useMe';

/**
 * Top-level shell. Calling useMe here means every page below has a
 * fresh principal in the TanStack Query cache on first paint.
 */
export function Layout() {
  useMe();
  return (
    <div className="min-h-screen flex flex-col">
      <Nav />
      <main className="flex-1">
        <Outlet />
      </main>
    </div>
  );
}
