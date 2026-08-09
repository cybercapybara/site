/**
 * @file PayPalClient.hpp
 * @brief PayPal REST v2 client: OAuth2 client-credentials token (cached,
 *        refreshed 60s before expiry), Orders v2 create/capture, and
 *        REST-based webhook signature verification.
 *
 * Mirrors S3Storage's libcurl usage in src/storage/Storage.hpp: one CURL easy
 * handle per call (no pooling/reuse), explicit connect+total timeouts so a
 * blackholed PayPal endpoint can't pin a Drogon IO thread forever, and
 * CURLOPT_NOSIGNAL because this runs from a multi-threaded process.
 *
 * Money-critical: the amount<->cents conversion NEVER goes through
 * double/float. PayPal amounts are decimal strings ("12.34");
 * detail::parse_decimal_to_cents splits on '.' and composes an int64 by
 * hand — see its doc comment for the exact rounding/rejection rules. This
 * matches the rest of the codebase's cents convention (Domain::Payment::
 * amount_cents, Billing::credit_capture's captured_amount_cents, …).
 *
 * Config keys (see config/config.json under billing.paypal.*):
 *   billing.paypal.environment    string  default "sandbox"  (sandbox|live)
 *   billing.paypal.client_id      string  default ""
 *   billing.paypal.client_secret  string  default ""          (never logged)
 *   billing.paypal.webhook_id     string  default ""
 *   billing.paypal.return_url     string  default ""
 *   billing.paypal.cancel_url     string  default ""
 *
 * Billing::initialize()/install_for_testing()/reset_for_testing() mirror
 * Storage::/Email::'s module test-seam shape (see the bottom of
 * src/storage/Storage.hpp): initialize() validates billing.paypal.* config
 * (throwing if billing.enabled=true and client_id/client_secret are empty)
 * and installs the production singleton; install_for_testing() lets Task 4/5
 * controller tests inject a fake/mock subclass instead of touching the
 * network. PayPalClient::get() — a class static method, per this module's
 * documented interface — lazily calls initialize() on first use if nothing
 * installed a client yet, so production code needs no explicit Core.hpp
 * wiring to keep working; an earlier explicit Billing::initialize() call
 * (e.g. from Core.hpp, if a future task wires it there) simply makes
 * misconfiguration fail at boot instead of on the first request.
 *
 * Every method that talks to PayPal (create_order, capture_order,
 * verify_webhook_signature's own call to PayPal) throws std::runtime_error
 * on a transport failure or a non-2xx response FROM PAYPAL — callers wrap in
 * Api::with_repo_errors, which maps an unexpected std::exception to 500 (a
 * webhook redelivery / a retried checkout is the correct outcome for a
 * transient PayPal-side or network problem). verify_webhook_signature's bool
 * return is reserved for "PayPal answered normally and the answer was
 * FAILURE / we couldn't even build a well-formed request" — seeing
 * `false` must never be confused with "PayPal is down".
 */

#pragma once

#include <cctype>
#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>

#include <curl/curl.h>
#include <spdlog/spdlog.h>

#include <nlohmann/json.hpp>

#include "utils/Config.hpp"

namespace Billing {

using json = nlohmann::json;

/// Result of PayPalClient::create_order — enough for the controller to
/// persist a Payment row (provider_order_id = order_id) and redirect the
/// browser to approve_url.
struct PayPalOrder {
    std::string order_id;
    std::string approve_url;
};

/// Result of PayPalClient::capture_order / parse_capture_response. Feeds
/// Billing::credit_capture(order_id, capture_id, amount_cents, currency)
/// directly (Task 4) — field names/types are chosen to match that call
/// site verbatim.
struct PayPalCapture {
    std::string capture_id;
    std::int64_t amount_cents = 0;
    std::string currency;
};

namespace detail {

/**
 * @brief Decimal-string -> integer cents, WITHOUT ever going through
 *        double/float (a stod round-trip is exactly how money bugs start).
 *        Splits on '.', validates every character is a digit, and composes
 *        the int64 by hand.
 *
 * Rules — every violation throws std::runtime_error, never a silent
 * zero/wrong amount:
 *   - empty string -> throws.
 *   - a leading '+' or '-' -> throws. PayPal never signs amounts; silently
 *     accepting '-' would be a straight path to a negative-money bug.
 *   - any character outside `[0-9]` (besides a single '.') -> throws.
 *   - no '.' at all ("12") -> treated as ".00": 1200.
 *   - exactly one fractional digit ("12.3") -> right-padded: 1230.
 *   - exactly two fractional digits ("12.34") -> 1234.
 *   - MORE than two fractional digits ("12.345") -> REJECTED (throws), not
 *     truncated. Every currency this template's `amount_cents` columns
 *     support is 2-decimal (USD/EUR/…); a 3rd digit means either a currency
 *     this code doesn't actually support (JPY=0 decimals, BHD/KWD=3) or a
 *     malformed/unexpected payload. Silently truncating would round money
 *     down without anyone noticing — reject and let the caller's exception
 *     path surface it instead.
 *   - a bare trailing '.' ("12.") -> throws (empty fractional part).
 *   - an integer part longer than 15 digits -> throws (overflow guard; no
 *     real payment is anywhere near this size).
 */
inline std::int64_t parse_decimal_to_cents(const std::string& s) {
    if (s.empty())
        throw std::runtime_error("paypal: empty amount string");
    if (s.front() == '+' || s.front() == '-')
        throw std::runtime_error("paypal: signed amount not accepted: '" + s + "'");

    const auto dot = s.find('.');
    const std::string int_part = (dot == std::string::npos) ? s : s.substr(0, dot);
    std::string frac_part = (dot == std::string::npos) ? std::string() : s.substr(dot + 1);

    if (int_part.empty())
        throw std::runtime_error("paypal: malformed amount '" + s + "'");
    if (int_part.size() > 15)
        throw std::runtime_error("paypal: amount '" + s + "' out of range");
    for (char c : int_part)
        if (!std::isdigit(static_cast<unsigned char>(c)))
            throw std::runtime_error("paypal: malformed amount '" + s + "'");

    if (dot != std::string::npos) {
        if (frac_part.empty())
            throw std::runtime_error("paypal: malformed amount '" + s + "' (trailing '.')");
        if (frac_part.size() > 2)
            throw std::runtime_error("paypal: amount '" + s + "' has more than 2 fractional digits");
        for (char c : frac_part)
            if (!std::isdigit(static_cast<unsigned char>(c)))
                throw std::runtime_error("paypal: malformed amount '" + s + "'");
    }
    while (frac_part.size() < 2)
        frac_part.push_back('0');

    const std::int64_t whole = std::stoll(int_part);
    const std::int64_t frac = std::stoll(frac_part);
    return whole * 100 + frac;
}

/// Inverse of parse_decimal_to_cents, for building request bodies
/// (`amount.value` on create_order). Integer-only, same no-double rule.
inline std::string cents_to_decimal_string(std::int64_t cents) {
    if (cents < 0)
        throw std::runtime_error("paypal: negative amount_cents");
    const std::int64_t whole = cents / 100;
    const std::int64_t frac = cents % 100;
    std::ostringstream oss;
    oss << whole << '.' << (frac < 10 ? "0" : "") << frac;
    return oss.str();
}

/// Case-insensitive header lookup. HTTP header names are case-insensitive by
/// spec (RFC 7230 §3.2); the caller (a Drogon webhook handler, Task 5) may
/// hand us headers in whatever case Drogon happened to preserve them in.
inline std::string find_header_ci(const std::map<std::string, std::string>& headers, const std::string& name) {
    for (const auto& kv : headers) {
        if (kv.first.size() != name.size())
            continue;
        bool match = true;
        for (std::size_t i = 0; i < name.size(); ++i) {
            if (std::tolower(static_cast<unsigned char>(kv.first[i])) !=
                std::tolower(static_cast<unsigned char>(name[i]))) {
                match = false;
                break;
            }
        }
        if (match)
            return kv.second;
    }
    return {};
}

/// RFC 3986 percent-encode a single path segment (order ids are opaque
/// PayPal-generated strings; this is defense-in-depth, mirrors
/// S3Storage::uri_encode_path).
inline std::string url_encode_segment(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
            c == '.' || c == '~')
            out.push_back(static_cast<char>(c));
        else {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 0xF]);
        }
    }
    return out;
}

inline std::size_t curl_write_cb(char* ptr, std::size_t size, std::size_t nmemb, void* userdata) {
    auto* out = static_cast<std::string*>(userdata);
    if (out)
        out->append(ptr, size * nmemb);
    return size * nmemb;
}

/// Best-effort extraction of PayPal's structured error fields from a
/// non-2xx response BODY, for inclusion in a thrown error message.
/// Orders/Payments/webhook-verify errors look like `{"name":...,
/// "message":..., "debug_id":...}`; OAuth errors look like
/// `{"error":..., "error_description":...}`. Without `debug_id`, PayPal
/// support cannot look anything up, and Task 4/5 need to surface these to
/// admins — silently keeping only the HTTP status throws that away.
///
/// Falls back to the raw body (truncated to @p kMaxRaw bytes, so one
/// unexpectedly huge error page can't make the exception message itself a
/// problem) when the body isn't JSON, isn't an object, or doesn't contain
/// any of the known fields. NEVER receives request headers or the token —
/// this only ever sees a RESPONSE body.
inline std::string describe_error_body(const std::string& resp_body) {
    constexpr std::size_t kMaxRaw = 500;
    const auto truncated = [&](const std::string& s) {
        return s.size() <= kMaxRaw ? s : s.substr(0, kMaxRaw) + "...(truncated)";
    };

    json j;
    try {
        j = json::parse(resp_body);
    } catch (const json::parse_error&) {
        return truncated(resp_body);
    }
    if (!j.is_object())
        return truncated(resp_body);

    std::string out;
    auto append_field = [&](const char* key) {
        if (j.contains(key) && j[key].is_string()) {
            if (!out.empty())
                out += " ";
            out += std::string(key) + "=" + j[key].get<std::string>();
        }
    };
    append_field("name");
    append_field("message");
    append_field("debug_id");
    append_field("error");
    append_field("error_description");

    // Valid JSON, but none of the known fields — still surface something
    // rather than silently saying nothing.
    return out.empty() ? truncated(j.dump()) : out;
}

}  // namespace detail

/// Construction config for PayPalClient — kept as a free struct (not a
/// nested `PayPalClient::Config`) to avoid colliding with the top-level
/// `Config::` namespace already used throughout this codebase. Mirrors
/// Email::MailerConfig's shape/naming.
struct PayPalClientConfig {
    std::string environment = "sandbox";
    std::string client_id;
    std::string client_secret;
    std::string webhook_id;
    std::string return_url;
    std::string cancel_url;
    // Whole-request and TCP-connect budgets, mirroring S3Storage::Config —
    // every call here can run inline on a Drogon IO thread, so these bound
    // how long one blackholed PayPal endpoint can pin it. No unbounded waits.
    long timeout_sec = 15;
    long connect_timeout_sec = 3;
};

class PayPalClient {
public:
    explicit PayPalClient(PayPalClientConfig cfg) : cfg_(std::move(cfg)) {
        curl_global_init(CURL_GLOBAL_DEFAULT);  // idempotent; Storage/Mailer may have run it
        if (cfg_.timeout_sec <= 0)
            cfg_.timeout_sec = 15;
        if (cfg_.connect_timeout_sec <= 0)
            cfg_.connect_timeout_sec = 3;
    }

    // Virtual so tests can install a fake/mock subclass via
    // install_for_testing() below that overrides create_order/capture_order/
    // verify_webhook_signature to return canned data with no network call —
    // otherwise install_for_testing() could only swap credentials, not
    // actually keep Task 4/5's controller tests off the network.
    virtual ~PayPalClient() = default;

    /// sandbox -> api-m.sandbox.paypal.com, live -> api-m.paypal.com, any
    /// other/unknown value -> sandbox. Fails SAFE: a config typo must never
    /// accidentally start moving real money against the live API.
    static std::string base_url(const std::string& environment) {
        if (environment == "live")
            return "https://api-m.paypal.com";
        return "https://api-m.sandbox.paypal.com";
    }

    /// Production accessor. Defined out-of-line below (after the
    /// initialize()/install_for_testing()/reset_for_testing() test-seam
    /// section), because it needs std::unique_ptr<PayPalClient> to be a
    /// complete type. Lazily calls initialize() on first use if nothing
    /// installed it yet, so production code keeps working with zero
    /// required wiring — but Core.hpp (or Task 4/5's controller
    /// registration) MAY call Billing::initialize() explicitly earlier to
    /// fail fast at boot instead of on the first request. Throws
    /// std::runtime_error if billing.enabled=true and client_id/
    /// client_secret are empty (see initialize()).
    static PayPalClient& get();

    /// POST /v2/checkout/orders. `reference` becomes purchase_units[0].
    /// reference_id (the app's own order/payment id, for reconciliation on
    /// PayPal's dashboard). Throws std::runtime_error on transport/non-2xx
    /// or a response missing an id / an "approve" link.
    virtual PayPalOrder create_order(std::int64_t amount_cents,
                                     const std::string& currency,
                                     const std::string& reference,
                                     const std::string& return_url,
                                     const std::string& cancel_url) {
        json amount = json::object();
        amount["currency_code"] = currency;
        amount["value"] = detail::cents_to_decimal_string(amount_cents);

        json purchase_unit = json::object();
        purchase_unit["reference_id"] = reference;
        purchase_unit["amount"] = amount;

        json app_ctx = json::object();
        app_ctx["return_url"] = return_url;
        app_ctx["cancel_url"] = cancel_url;
        app_ctx["user_action"] = "PAY_NOW";

        json body = json::object();
        body["intent"] = "CAPTURE";
        body["purchase_units"] = json::array({purchase_unit});
        body["application_context"] = app_ctx;

        std::string resp_body;
        const long code = authorized_request("POST", "/v2/checkout/orders", body.dump(), &resp_body);
        if (code < 200 || code >= 300)
            throw std::runtime_error("paypal: create_order failed with HTTP " + std::to_string(code) + ": " +
                                     detail::describe_error_body(resp_body));

        json j;
        try {
            j = json::parse(resp_body);
        } catch (const json::parse_error&) {
            throw std::runtime_error("paypal: create_order response is not valid JSON");
        }
        if (!j.contains("id") || !j["id"].is_string() || j["id"].get<std::string>().empty())
            throw std::runtime_error("paypal: create_order response missing id");

        PayPalOrder out;
        out.order_id = j["id"].get<std::string>();
        if (j.contains("links") && j["links"].is_array()) {
            for (const auto& link : j["links"]) {
                if (link.is_object() && link.value("rel", std::string()) == "approve" && link.contains("href") &&
                    link["href"].is_string()) {
                    out.approve_url = link["href"].get<std::string>();
                    break;
                }
            }
        }
        if (out.approve_url.empty())
            throw std::runtime_error("paypal: create_order response missing 'approve' link");
        return out;
    }

    /// POST /v2/checkout/orders/{order_id}/capture. Throws
    /// std::runtime_error on transport/non-2xx or a malformed response body
    /// (see parse_capture_response).
    virtual PayPalCapture capture_order(const std::string& order_id) {
        if (order_id.empty())
            throw std::runtime_error("paypal: capture_order: empty order_id");
        std::string resp_body;
        const long code = authorized_request(
            "POST", "/v2/checkout/orders/" + detail::url_encode_segment(order_id) + "/capture", "{}", &resp_body);
        if (code < 200 || code >= 300)
            throw std::runtime_error("paypal: capture_order failed with HTTP " + std::to_string(code) + ": " +
                                     detail::describe_error_body(resp_body));
        return parse_capture_response(resp_body);
    }

    /// POST /v1/notifications/verify-webhook-signature. Operates on the RAW
    /// body bytes exactly as received on the wire — @p raw_body is parsed
    /// here, once, and embedded verbatim as `webhook_event`; nothing
    /// upstream should re-serialize/re-derive it first.
    ///
    /// Returns false (does NOT throw) when:
    ///   - @p raw_body is not valid JSON, or one of the required
    ///     `paypal-*` headers is missing (case-insensitive lookup) — we
    ///     never even reach PayPal in that case;
    ///   - PayPal answers 2xx but `verification_status != "SUCCESS"`, or
    ///     answers 2xx with an unparseable body.
    /// Throws std::runtime_error (does NOT return false) on a transport
    /// failure or a non-2xx response FROM PAYPAL'S OWN API — that is a
    /// "we couldn't ask PayPal" condition, not "PayPal said no", and must
    /// not be reported to a caller as if the signature were checked and
    /// failed (a caller that conflated the two could log a benign PayPal
    /// outage as a spoofed-webhook security incident).
    virtual bool verify_webhook_signature(const std::map<std::string, std::string>& headers,
                                          const std::string& raw_body) {
        json event;
        try {
            event = json::parse(raw_body);
        } catch (const json::parse_error&) {
            spdlog::warn("paypal: webhook body is not valid JSON — treating as unverified");
            return false;
        }

        const std::string auth_algo = detail::find_header_ci(headers, "paypal-auth-algo");
        const std::string cert_url = detail::find_header_ci(headers, "paypal-cert-url");
        const std::string transmission_id = detail::find_header_ci(headers, "paypal-transmission-id");
        const std::string transmission_sig = detail::find_header_ci(headers, "paypal-transmission-sig");
        const std::string transmission_time = detail::find_header_ci(headers, "paypal-transmission-time");
        if (auth_algo.empty() || cert_url.empty() || transmission_id.empty() || transmission_sig.empty() ||
            transmission_time.empty()) {
            spdlog::warn("paypal: webhook missing one or more paypal-* headers — treating as unverified");
            return false;
        }

        json verify_req = json::object();
        verify_req["auth_algo"] = auth_algo;
        verify_req["cert_url"] = cert_url;
        verify_req["transmission_id"] = transmission_id;
        verify_req["transmission_sig"] = transmission_sig;
        verify_req["transmission_time"] = transmission_time;
        verify_req["webhook_id"] = cfg_.webhook_id;
        verify_req["webhook_event"] = event;

        std::string resp_body;
        const long code =
            authorized_request("POST", "/v1/notifications/verify-webhook-signature", verify_req.dump(), &resp_body);
        if (code < 200 || code >= 300)
            throw std::runtime_error("paypal: verify-webhook-signature failed with HTTP " + std::to_string(code) +
                                     ": " + detail::describe_error_body(resp_body));

        json j;
        try {
            j = json::parse(resp_body);
        } catch (const json::parse_error&) {
            spdlog::warn("paypal: verify-webhook-signature returned a non-JSON 2xx body — treating as unverified");
            return false;
        }
        return j.value("verification_status", std::string()) == "SUCCESS";
    }

    /// Pure, no I/O — exposed for tests. Extracts the FIRST capture from
    /// purchase_units[0].payments.captures[0] of a PayPal v2 orders capture
    /// response. Throws std::runtime_error on any missing/malformed field;
    /// NEVER returns a zero-amount capture for a malformed body (a bug here
    /// would silently under-credit a wallet — see parse_decimal_to_cents).
    static PayPalCapture parse_capture_response(const std::string& json_body) {
        json j;
        try {
            j = json::parse(json_body);
        } catch (const json::parse_error& e) {
            throw std::runtime_error(std::string("paypal: malformed capture response JSON: ") + e.what());
        }

        if (!j.is_object() || !j.contains("purchase_units") || !j["purchase_units"].is_array() ||
            j["purchase_units"].empty())
            throw std::runtime_error("paypal: capture response missing purchase_units");
        const json& pu0 = j["purchase_units"][0];

        if (!pu0.is_object() || !pu0.contains("payments") || !pu0["payments"].is_object() ||
            !pu0["payments"].contains("captures") || !pu0["payments"]["captures"].is_array() ||
            pu0["payments"]["captures"].empty())
            throw std::runtime_error("paypal: capture response missing payments.captures");
        const json& cap = pu0["payments"]["captures"][0];

        if (!cap.is_object() || !cap.contains("id") || !cap["id"].is_string() || cap["id"].get<std::string>().empty())
            throw std::runtime_error("paypal: capture response missing capture id");
        if (!cap.contains("amount") || !cap["amount"].is_object() || !cap["amount"].contains("value") ||
            !cap["amount"]["value"].is_string() || !cap["amount"].contains("currency_code") ||
            !cap["amount"]["currency_code"].is_string())
            throw std::runtime_error("paypal: capture response missing amount");

        PayPalCapture out;
        out.capture_id = cap["id"].get<std::string>();
        out.currency = cap["amount"]["currency_code"].get<std::string>();
        out.amount_cents = detail::parse_decimal_to_cents(cap["amount"]["value"].get<std::string>());
        return out;
    }

private:
    /// POST /v1/oauth2/token with HTTP Basic auth (client_id/client_secret
    /// handed straight to libcurl's CURLOPT_USERNAME/PASSWORD — we never
    /// build the "Authorization: Basic ..." string ourselves, so there is
    /// nothing home-grown to accidentally log). Caches the token in-process
    /// until `expires_in - 60s`; a request that arrives inside that 60s
    /// safety margin never races an in-flight expiry.
    ///
    /// Double-checked: the fast "is my cached token still good" check runs
    /// under token_mutex_, but the OAuth network round-trip itself runs
    /// WITHOUT holding it — otherwise every concurrent billing request
    /// queues behind one slow/blackholed token endpoint for up to
    /// timeout_sec, not just the request that triggered the refresh. The
    /// lock is re-taken only to publish the result, re-checking first
    /// whether another thread already refreshed while we were making our
    /// own (possibly redundant, but harmless — both are valid) request.
    void ensure_access_token() {
        {
            std::lock_guard<std::mutex> lock(token_mutex_);
            if (!access_token_.empty() && std::chrono::steady_clock::now() < token_expiry_)
                return;
        }

        CURL* h = curl_easy_init();
        if (!h)
            throw std::runtime_error("paypal: curl_easy_init failed");

        const std::string url = base_url(cfg_.environment) + "/v1/oauth2/token";
        const std::string post_fields = "grant_type=client_credentials";
        std::string resp_body;

        curl_easy_setopt(h, CURLOPT_URL, url.c_str());
        curl_easy_setopt(h, CURLOPT_POST, 1L);
        curl_easy_setopt(h, CURLOPT_POSTFIELDS, post_fields.c_str());
        curl_easy_setopt(h, CURLOPT_POSTFIELDSIZE, static_cast<long>(post_fields.size()));
        curl_easy_setopt(h, CURLOPT_HTTPAUTH, static_cast<long>(CURLAUTH_BASIC));
        curl_easy_setopt(h, CURLOPT_USERNAME, cfg_.client_id.c_str());
        curl_easy_setopt(h, CURLOPT_PASSWORD, cfg_.client_secret.c_str());
        curl_easy_setopt(h, CURLOPT_TIMEOUT, cfg_.timeout_sec);
        curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT, cfg_.connect_timeout_sec);
        // Multi-threaded process: without this libcurl uses SIGALRM for its
        // own timeouts and the resolver, which is not thread-safe here.
        curl_easy_setopt(h, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, &detail::curl_write_cb);
        curl_easy_setopt(h, CURLOPT_WRITEDATA, &resp_body);

        struct curl_slist* hdrs = nullptr;
        hdrs = curl_slist_append(hdrs, "Accept: application/json");
        curl_easy_setopt(h, CURLOPT_HTTPHEADER, hdrs);

        const CURLcode rc = curl_easy_perform(h);
        long code = 0;
        if (rc == CURLE_OK)
            curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &code);
        curl_slist_free_all(hdrs);
        curl_easy_cleanup(h);

        if (rc != CURLE_OK)
            throw std::runtime_error(std::string("paypal: oauth transport error: ") + curl_easy_strerror(rc));
        if (code < 200 || code >= 300)
            throw std::runtime_error("paypal: oauth token request failed with HTTP " + std::to_string(code) + ": " +
                                     detail::describe_error_body(resp_body));

        json j;
        try {
            j = json::parse(resp_body);
        } catch (const json::parse_error&) {
            throw std::runtime_error("paypal: oauth response is not valid JSON");
        }
        if (!j.contains("access_token") || !j["access_token"].is_string() ||
            j["access_token"].get<std::string>().empty())
            throw std::runtime_error("paypal: oauth response missing access_token");

        const std::int64_t expires_in = j.value("expires_in", static_cast<std::int64_t>(0));
        const std::string new_token = j["access_token"].get<std::string>();
        const std::int64_t safe_ttl = expires_in > 60 ? expires_in - 60 : 0;
        const auto new_expiry = std::chrono::steady_clock::now() + std::chrono::seconds(safe_ttl);

        {
            std::lock_guard<std::mutex> lock(token_mutex_);
            // Re-check: another thread may have refreshed while we were
            // doing our own round-trip outside the lock. If its token is
            // already valid, there's nothing to fix — both refreshes are
            // equally good, so just keep whichever landed first rather than
            // clobbering it with ours.
            if (access_token_.empty() || std::chrono::steady_clock::now() >= token_expiry_) {
                access_token_ = new_token;
                token_expiry_ = new_expiry;
            }
        }

        // Never log the token itself — only that one was obtained and for
        // how long it's cached.
        spdlog::info("paypal: oauth token acquired env={} expires_in={}s", cfg_.environment, expires_in);
    }

    /// Shared authenticated-request path for every call after OAuth:
    /// ensures a fresh token, sends `Authorization: Bearer <token>` plus a
    /// JSON body, and returns the HTTP status. Throws std::runtime_error on
    /// a transport failure; does NOT interpret the status code — every
    /// caller checks it themselves so each can phrase its own error.
    long authorized_request(const std::string& method,
                            const std::string& path,
                            const std::string& body,
                            std::string* out_body) {
        ensure_access_token();
        std::string token_copy;
        {
            std::lock_guard<std::mutex> lock(token_mutex_);
            token_copy = access_token_;
        }

        CURL* h = curl_easy_init();
        if (!h)
            throw std::runtime_error("paypal: curl_easy_init failed");

        const std::string url = base_url(cfg_.environment) + path;
        curl_easy_setopt(h, CURLOPT_URL, url.c_str());
        curl_easy_setopt(h, CURLOPT_TIMEOUT, cfg_.timeout_sec);
        curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT, cfg_.connect_timeout_sec);
        curl_easy_setopt(h, CURLOPT_NOSIGNAL, 1L);

        if (method == "POST") {
            curl_easy_setopt(h, CURLOPT_POST, 1L);
            curl_easy_setopt(h, CURLOPT_POSTFIELDS, body.c_str());
            curl_easy_setopt(h, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
        } else if (method != "GET") {
            curl_easy_setopt(h, CURLOPT_CUSTOMREQUEST, method.c_str());
        }

        curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, &detail::curl_write_cb);
        curl_easy_setopt(h, CURLOPT_WRITEDATA, out_body);

        struct curl_slist* hdrs = nullptr;
        hdrs = curl_slist_append(hdrs, "Content-Type: application/json");
        hdrs = curl_slist_append(hdrs, "Accept: application/json");
        // Built here and handed to curl_slist_append, which copies the
        // string internally — the token never gets logged, only ever sent
        // as a real request header. See the class doc comment: transport
        // errors below intentionally never echo request headers or body.
        const std::string auth_header = "Authorization: Bearer " + token_copy;
        hdrs = curl_slist_append(hdrs, auth_header.c_str());
        curl_easy_setopt(h, CURLOPT_HTTPHEADER, hdrs);

        const CURLcode rc = curl_easy_perform(h);
        long code = 0;
        if (rc == CURLE_OK)
            curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &code);
        curl_slist_free_all(hdrs);
        curl_easy_cleanup(h);

        if (rc != CURLE_OK)
            throw std::runtime_error(std::string("paypal: ") + method + " " + path +
                                     " transport error: " + curl_easy_strerror(rc));
        return code;
    }

    PayPalClientConfig cfg_;
    std::mutex token_mutex_;
    std::string access_token_;
    std::chrono::steady_clock::time_point token_expiry_{};
};

// ── Config loading + global accessor / test seam ────────────────────────────
// Mirrors Storage::/Email::'s initialize()/install_for_testing()/
// reset_for_testing() shape (see src/storage/Storage.hpp's bottom section)
// so Tasks 4/5 can inject a fake PayPalClient subclass in controller tests
// instead of ever touching the network. PayPalClient::get() itself stays a
// class static method (its documented public interface), but is defined
// out-of-line below, after global_paypal_client, because it needs
// std::unique_ptr<PayPalClient> to be a complete type.

inline PayPalClientConfig load_config_from_global() {
    PayPalClientConfig cfg;
    if (!Config::is_initialized())
        return cfg;
    auto& c = Config::get();
    cfg.environment = c.get<std::string>("billing.paypal.environment", "PAYPAL_ENV", "sandbox");
    cfg.client_id = c.get<std::string>("billing.paypal.client_id", "PAYPAL_CLIENT_ID", "");
    cfg.client_secret = c.get<std::string>("billing.paypal.client_secret", "PAYPAL_CLIENT_SECRET", "");
    cfg.webhook_id = c.get<std::string>("billing.paypal.webhook_id", "PAYPAL_WEBHOOK_ID", "");
    cfg.return_url = c.get<std::string>("billing.paypal.return_url", "PAYPAL_RETURN_URL", "");
    cfg.cancel_url = c.get<std::string>("billing.paypal.cancel_url", "PAYPAL_CANCEL_URL", "");
    return cfg;
}

inline std::unique_ptr<PayPalClient> global_paypal_client = nullptr;

inline bool is_initialized() {
    return global_paypal_client != nullptr;
}

/// Reads billing.paypal.* (and billing.enabled) from Config and installs the
/// production singleton. Throws std::runtime_error if billing.enabled=true
/// and client_id/client_secret are empty — fail loud rather than 500ing on
/// the first real checkout attempt (mirrors S3Storage::initialize throwing
/// when storage.backend=s3 is missing its endpoint/bucket, Storage.hpp
/// around line 506). When billing is disabled (or Config was never
/// initialized — a narrow unit test that never boots Core), an empty/default
/// config is installed without validation, matching how the rest of this
/// module treats "billing off" as a no-op rather than an error.
///
/// Idempotent: a no-op if a client is already installed, whether by an
/// earlier call to this function OR by install_for_testing() — so it's safe
/// for get() to call this defensively below without ever clobbering a test
/// double a caller already installed.
inline void initialize() {
    if (global_paypal_client != nullptr)
        return;
    PayPalClientConfig cfg = load_config_from_global();
    const bool billing_on =
        Config::is_initialized() && Config::get().get<bool>("billing.enabled", "BILLING_ENABLED", false);
    if (billing_on && (cfg.client_id.empty() || cfg.client_secret.empty()))
        throw std::runtime_error(
            "billing.paypal.client_id / billing.paypal.client_secret must be set when billing.enabled=true");
    global_paypal_client = std::make_unique<PayPalClient>(std::move(cfg));
}

/// Test seam: install a fake/mock PayPalClient (a test-only subclass
/// overriding create_order/capture_order/verify_webhook_signature) so
/// controller tests (Task 4/5) can exercise the checkout/capture/webhook
/// flow with no network access. Mirrors Storage::install_for_testing.
inline void install_for_testing(std::unique_ptr<PayPalClient> client) {
    global_paypal_client = std::move(client);
}

inline void reset_for_testing() {
    global_paypal_client.reset();
}

inline PayPalClient& PayPalClient::get() {
    if (!global_paypal_client)
        initialize();
    return *global_paypal_client;
}

}  // namespace Billing
