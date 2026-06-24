/**
 * @file Endpoints.hpp
 * @brief Endpoint registry — the single source of truth for routes.
 * @details `docs/openapi.yaml` is checked against this list in CI via
 *          scripts/check-openapi-drift.sh; `--print-routes` prints it.
 *          Add a line here for every new ADD_METHOD_TO.
 */

#pragma once

#include <string>
#include <vector>

namespace Api {

/**
 * @brief Endpoint metadata: method, path, description
 */
struct EndpointInfo {
    std::string method;
    std::string path;
    std::string description;
};

/**
 * @brief Single source of truth for all registered API endpoints
 */
inline const std::vector<EndpointInfo>& get_endpoints() {
    static const std::vector<EndpointInfo> endpoints = {
        {"GET", "/", "Endpoint discovery"},
        {"GET", "/healthz", "Liveness probe"},
        {"GET", "/ready", "Readiness probe"},
        {"GET", "/health", "Detailed health check"},
        {"POST", "/api/auth/register", "Register a new user"},
        {"POST", "/api/auth/login", "Log in (issues access + refresh cookies)"},
        {"POST", "/api/auth/logout", "Log out (clears cookies + revokes refresh)"},
        {"POST", "/api/auth/refresh", "Rotate access + refresh cookies"},
        {"GET", "/api/auth/me", "Get the authenticated user"},
        {"POST", "/api/account/confirm-resend", "Resend email confirmation link"},
        {"POST", "/api/account/confirm/{token}", "Confirm an account from token"},
        {"POST", "/api/account/reset-password-request", "Request a password-reset email"},
        {"POST", "/api/account/reset-password/{token}", "Reset password using a token"},
        {"POST", "/api/account/change-email-request", "Request a confirm-email link for a new address"},
        {"POST", "/api/account/change-email/{token}", "Apply a pending email change from token"},
        {"POST", "/api/account/join-from-invite/{token}", "Set password and confirm account from an invite token"},
        {"POST", "/api/account/change-password", "Change password while logged in"},
        {"GET", "/api/account/api-keys", "List your API keys"},
        {"POST", "/api/account/api-keys", "Create an API key (secret shown once)"},
        {"DELETE", "/api/account/api-keys/{id}", "Revoke an API key"},
        {"GET", "/api/admin/users", "Admin: list users"},
        {"POST", "/api/admin/users", "Admin: create a user"},
        {"POST", "/api/admin/invite", "Admin: invite a user via email"},
        {"GET", "/api/admin/users/{id}", "Admin: user detail"},
        {"PATCH", "/api/admin/users/{id}", "Admin: update user (email, role, name)"},
        {"DELETE", "/api/admin/users/{id}", "Admin: delete user"},
        {"GET", "/api/admin/roles", "Admin: list roles"},
        {"POST", "/api/admin/roles", "Admin: create role"},
        {"PATCH", "/api/admin/roles/{id}", "Admin: update role (name, permissions, is_default)"},
        {"DELETE", "/api/admin/roles/{id}", "Admin: delete role"},
        {"GET", "/api/admin/audit", "Admin: list the audit trail (requires audit-read permission)"},
        {"GET", "/api/jobs", "List jobs"},
        {"POST", "/api/jobs", "Submit job"},
        {"GET", "/api/jobs/dlq", "List dead-letter queue"},
        {"POST", "/api/jobs/dlq/{id}/requeue", "Requeue a DLQ job"},
        {"GET", "/api/jobs/{id}", "Get job status"},
        {"DELETE", "/api/jobs/{id}", "Cancel job"},
    };
    return endpoints;
}

}  // namespace Api
