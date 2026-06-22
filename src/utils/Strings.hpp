/**
 * @file Strings.hpp
 * @brief Small string helpers used in multiple modules.
 */

#pragma once

#include <sstream>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace Utils::Strings {

/**
 * @brief Paths that every middleware treats as never-authenticated and
 *        never-rate-limited. Read once from `api.public_paths` config /
 *        `API_PUBLIC_PATHS` env and reused by all security modules to
 *        avoid skew between per-module overrides.
 *
 * Entries are exact-match, except a trailing `*` matches by prefix — needed
 * for the token-bearing account routes (confirm / reset / change-email),
 * which carry the token as a path segment and so can't be matched exactly.
 * Those flows MUST be reachable without a session (the user clicking an email
 * link isn't logged in), so they ship public by default. Note the static
 * `*-request` / `confirm-resend` routes are deliberately NOT here:
 * change-email-request and confirm-resend require an authenticated principal.
 */
inline constexpr const char* kDefaultPublicPathsCsv =
    "/,/healthz,/ready,/health,/metrics,"
    "/api/docs,/api/openapi.yaml,"
    "/api/auth/login,/api/auth/register,/api/auth/refresh,"
    "/api/account/confirm/*,/api/account/reset-password-request,"
    "/api/account/reset-password/*,/api/account/change-email/*,"
    "/api/account/join-from-invite/*";

/**
 * @brief True if @p path is covered by @p public_paths — exact match, or a
 *        prefix match for an entry ending in `*`. Shared by Auth / RateLimit /
 *        Idempotency so they can't disagree about what's public.
 */
inline bool path_is_public(const std::unordered_set<std::string>& public_paths, const std::string& path) {
    if (public_paths.count(path) > 0)
        return true;
    for (const auto& p : public_paths) {
        if (!p.empty() && p.back() == '*') {
            const std::string_view prefix(p.data(), p.size() - 1);
            if (path.size() >= prefix.size() && path.compare(0, prefix.size(), prefix) == 0)
                return true;
        }
    }
    return false;
}

/**
 * @brief Split @p csv on commas, dropping empty components.
 */
inline std::vector<std::string> split_csv_vec(const std::string& csv) {
    std::vector<std::string> out;
    std::stringstream ss(csv);
    std::string piece;
    while (std::getline(ss, piece, ',')) {
        // Trim surrounding whitespace so "a, b" yields {"a","b"} not {"a"," b"}
        // — public-path / whitelist / CORS configs are routinely written with
        // spaces after commas.
        const size_t a = piece.find_first_not_of(" \t\r\n");
        if (a == std::string::npos)
            continue;  // all-whitespace / empty
        const size_t b = piece.find_last_not_of(" \t\r\n");
        out.push_back(piece.substr(a, b - a + 1));
    }
    return out;
}

/// CSV → unordered_set wrapper for callers that need set semantics (auth /
/// rate-limit public paths). Built on top of split_csv_vec — single source.
inline std::unordered_set<std::string> split_csv_set(const std::string& csv) {
    auto v = split_csv_vec(csv);
    return {v.begin(), v.end()};
}

}  // namespace Utils::Strings
