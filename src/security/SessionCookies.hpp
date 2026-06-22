/**
 * @file SessionCookies.hpp
 * @brief Cookie-based session plumbing: config + request-side extraction +
 *        response-side emission. No dependency on the Auth singleton.
 */

#pragma once

#include <string>
#include <string_view>

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>

namespace Security::Auth {

/**
 * @brief Cookie-based session settings. When enabled, the middleware
 *        accepts the access token from `cookie_access_name` cookie in
 *        addition to the Authorization header. Refresh tokens never
 *        leave the cookie channel.
 */
struct CookieConfig {
    bool enabled = false;
    // Use __Host-prefixed names by default — browsers refuse to set them
    // unless Secure + Path=/ + no Domain, which is exactly what we want
    // and prevents subdomain confusion. Override for local dev (e.g.
    // strip the prefix if you can't run https://localhost).
    std::string access_name = "__Host-access";
    std::string refresh_name = "__Host-refresh";
    int access_ttl_sec = 15 * 60;            // 15 minutes
    int refresh_ttl_sec = 7 * 24 * 60 * 60;  // 7 days
    bool secure = true;                      // emit Secure cookie attribute
    std::string samesite = "Lax";            // Lax | Strict | None
    // Redis key prefix for refresh-token revocation. Each issued refresh
    // token has a JTI that we write here on issue and check on /refresh.
    // Logout deletes the entry, instantly revoking the chain.
    std::string refresh_revocation_prefix = "auth:refresh:";
};

/**
 * @brief Pull the access token from either Authorization: Bearer or the
 *        configured access cookie. Cookie wins when both are present —
 *        SPAs always have a cookie, only API clients use the header.
 *        Empty string means "no credential found".
 *
 * Uses Drogon's native getCookie() so we don't have to re-parse the
 * Cookie header by hand (Drogon already populates a name->value map
 * during request decode).
 */
inline std::string extract_access_token(const drogon::HttpRequestPtr& req, const CookieConfig& cookies) {
    if (cookies.enabled) {
        const auto& c = req->getCookie(cookies.access_name);
        if (!c.empty())
            return c;
    }
    const auto& header = req->getHeader("Authorization");
    constexpr std::string_view prefix = "Bearer ";
    if (header.size() > prefix.size() && header.compare(0, prefix.size(), prefix) == 0) {
        return header.substr(prefix.size());
    }
    return {};
}

inline std::string extract_refresh_token(const drogon::HttpRequestPtr& req, const CookieConfig& cookies) {
    if (!cookies.enabled)
        return {};
    return req->getCookie(cookies.refresh_name);
}

/**
 * @brief Emit Set-Cookie headers for a fresh access + refresh pair.
 *        Pass empty access_token to clear (logout) — emits Max-Age=0.
 *
 * Drogon's HttpResponse stores headers in a map keyed by name, so a
 * second `addHeader("Set-Cookie", ...)` overwrites the first. Use
 * `addCookie(drogon::Cookie{...})` instead — Drogon's serializer
 * emits a real `Set-Cookie:` line per cookie.
 */
inline void set_session_cookies(const drogon::HttpResponsePtr& resp,
                                const CookieConfig& cookies,
                                const std::string& access_token,
                                const std::string& refresh_token) {
    auto make_cookie = [&](const std::string& name, const std::string& value, int ttl) {
        drogon::Cookie c(name, value);
        c.setHttpOnly(true);
        c.setPath("/");
        c.setMaxAge(ttl);
        if (cookies.secure)
            c.setSecure(true);
        if (!cookies.samesite.empty()) {
            // Drogon's enum spells the values without the "Same" prefix.
            if (cookies.samesite == "Lax")
                c.setSameSite(drogon::Cookie::SameSite::kLax);
            else if (cookies.samesite == "Strict")
                c.setSameSite(drogon::Cookie::SameSite::kStrict);
            else if (cookies.samesite == "None")
                c.setSameSite(drogon::Cookie::SameSite::kNone);
        }
        return c;
    };
    if (access_token.empty()) {
        // logout: zero out both. Cookie with Max-Age=0 + empty value
        // tells the browser to discard.
        resp->addCookie(make_cookie(cookies.access_name, "", 0));
        resp->addCookie(make_cookie(cookies.refresh_name, "", 0));
        return;
    }
    resp->addCookie(make_cookie(cookies.access_name, access_token, cookies.access_ttl_sec));
    if (!refresh_token.empty())
        resp->addCookie(make_cookie(cookies.refresh_name, refresh_token, cookies.refresh_ttl_sec));
}

}  // namespace Security::Auth
