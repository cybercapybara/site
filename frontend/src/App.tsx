import { Routes, Route, Outlet } from 'react-router-dom';

import { Layout } from '@/components/Layout';
import { ProtectedRoute } from '@/components/ProtectedRoute';
import { NotFoundPage } from '@/pages/NotFound';
import { Permission } from '@/lib/auth/permissions';
import { routes, type RouteEntry, type RouteGuard } from '@/routes/manifest';

/**
 * Guard groups expressed as layout routes: the wrapper renders once and
 * children mount via <Outlet/>. A new admin page added to the manifest
 * under guard:'admin' can't accidentally skip the permission check.
 */
function RequireAuth() {
  return (
    <ProtectedRoute>
      <Outlet />
    </ProtectedRoute>
  );
}

function RequireConfirmed() {
  return (
    <ProtectedRoute requireConfirmed>
      <Outlet />
    </ProtectedRoute>
  );
}

function RequireAdmin() {
  return (
    <ProtectedRoute requirePermission={Permission.Administer} requireConfirmed>
      <Outlet />
    </ProtectedRoute>
  );
}

function routesFor(guard: RouteGuard): RouteEntry[] {
  return routes.filter((r) => r.guard === guard);
}

function renderRoute(r: RouteEntry) {
  return (
    <Route
      key={r.path}
      path={r.path}
      element={
        r.requirePermission !== undefined && r.guard !== 'admin' ? (
          <ProtectedRoute requirePermission={r.requirePermission} requireConfirmed>
            {r.element}
          </ProtectedRoute>
        ) : (
          r.element
        )
      }
    />
  );
}

export default function App() {
  return (
    <Routes>
      <Route element={<Layout />}>
        {/* Public pages */}
        {routesFor('public').map(renderRoute)}

        {/* Authenticated but possibly unconfirmed */}
        <Route element={<RequireAuth />}>{routesFor('auth').map(renderRoute)}</Route>

        {/* Authenticated + confirmed */}
        <Route element={<RequireConfirmed />}>{routesFor('confirmed').map(renderRoute)}</Route>

        {/* Admin — gated by Permission.Administer (0xff) */}
        <Route element={<RequireAdmin />}>{routesFor('admin').map(renderRoute)}</Route>

        <Route path="*" element={<NotFoundPage />} />
      </Route>
    </Routes>
  );
}
