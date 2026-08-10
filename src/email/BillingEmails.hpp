/**
 * @file BillingEmails.hpp
 * @brief Best-effort transactional emails for the billing module: top-up
 *        receipts, refund/reversal notices, failed-payment notices, and
 *        (Task 2) admin wallet-adjustment notices.
 *
 * Routed through Email::SendEmail::send() (src/email/GenericEmail.hpp) — the
 * generic ad-hoc "email.send" job type, NOT a new job kind. Every public
 * function here (receipt/refund/failed/adjustment) wraps its entire body in
 * try/catch and NEVER throws: a template error, a missing user, or a mail
 * outage must never surface into money code. Callers (BillingController /
 * AdminBillingController) are required to dispatch these AFTER a
 * Billing::* wallet call has already returned — never from inside
 * Database::execute_write. See BillingController.hpp's dispatch helpers for
 * how each call site decides WHETHER to send (the credited/applied/failed
 * dedupe logic lives there, not here — this file only renders and sends).
 *
 * Money formatting: cents -> "12.34" is plain integer division/modulo
 * (amount_cents / 100, amount_cents % 100, zero-padded) — no double
 * anywhere, matching this codebase's money-handling convention (see
 * Wallet.hpp).
 */

#pragma once

#include <cstdint>
#include <string>

#include <spdlog/spdlog.h>

#include <nlohmann/json.hpp>

#include "domain/User.hpp"
#include "email/GenericEmail.hpp"
#include "email/Templates.hpp"
#include "utils/Config.hpp"
#include "utils/Strings.hpp"
#include "utils/Time.hpp"

namespace Email::BillingEmails {

using json = nlohmann::json;

namespace detail {

inline std::string app_name() {
    if (Config::is_initialized())
        return Config::get().get<std::string>("app.name", "APP_NAME", "App");
    return "App";
}

/// cents -> "12.34" (or "-12.34" for a negative value) — integer-only, no
/// floating point anywhere in this conversion.
inline std::string format_cents(std::int64_t cents) {
    const bool negative = cents < 0;
    const std::int64_t magnitude = negative ? -cents : cents;
    const std::int64_t whole = magnitude / 100;
    const std::int64_t frac = magnitude % 100;
    std::string out = negative ? "-" : "";
    out += std::to_string(whole);
    out += ".";
    if (frac < 10)
        out += "0";
    out += std::to_string(frac);
    return out;
}

/// delta_credits -> "+42" / "-7" — plain signed integer formatting for the
/// admin adjustment notice. Unlike format_cents above, this is NOT a money
/// amount (no /100 cents conversion) — delta_credits is already a plain
/// credit count. std::to_string() already prepends '-' for a negative
/// value, so only the positive case needs an explicit '+'; no double
/// anywhere.
inline std::string format_signed_credits(std::int64_t delta_credits) {
    if (delta_credits >= 0)
        return "+" + std::to_string(delta_credits);
    return std::to_string(delta_credits);
}

/// Dispatch-time wall clock, ISO-8601 — the same choice receipt/refund/
/// failed's caller (BillingController's dispatch helpers) makes for those
/// emails (see Task 1 report, "Design decisions" #4). adjustment() has no
/// stored historical timestamp to draw from at all (it's a live admin
/// action), so this file computes it directly rather than taking it as a
/// parameter.
inline std::string now_iso8601() { return Utils::Time::epoch_to_iso8601(Utils::Time::now_epoch_seconds()); }

/**
 * @brief Render + enqueue one billing email. NEVER throws — a render
 *        failure or an enqueue/send failure is logged and swallowed, the
 *        same contract as AccountEmails' controller-facing dispatch().
 */
inline void send_rendered(const std::string& tmpl, const std::string& subject, const Domain::User& user, json ctx) {
    try {
        ctx["app_name"] = app_name();
        ctx["user"] = json{{"full_name", user.full_name()}};
        auto rendered = Email::Templates::render_pair(tmpl, ctx);
        Email::SendEmail::send(user.email, subject, rendered.text, rendered.html);
    } catch (const std::exception& e) {
        spdlog::warn("BillingEmails: {} for {} failed: {}", tmpl, Utils::Strings::mask_email(user.email), e.what());
    }
}

}  // namespace detail

/**
 * @brief Top-up receipt. Callers must send this ONLY when
 *        Billing::CreditResult::credited == true on the credit_capture call
 *        that produced it — that flag is exactly what makes a return-flow
 *        capture and a webhook for the SAME payment produce one receipt,
 *        never two (see BillingController::capture / handleCaptureCompleted).
 */
inline void receipt(const Domain::User& user,
                    const std::string& package_title,
                    std::int64_t amount_cents,
                    const std::string& currency,
                    std::int64_t credits,
                    std::int64_t new_balance,
                    const std::string& payment_id,
                    const std::string& date) {
    try {
        json ctx;
        ctx["package_title"] = package_title;
        ctx["amount"] = detail::format_cents(amount_cents);
        ctx["currency"] = currency;
        ctx["credits"] = credits;
        ctx["new_balance"] = new_balance;
        ctx["payment_id"] = payment_id;
        ctx["date"] = date;
        detail::send_rendered("billing_receipt", "Your top-up receipt", user, ctx);
    } catch (const std::exception& e) {
        spdlog::warn(
            "BillingEmails::receipt: failed for {}: {}", Utils::Strings::mask_email(user.email), e.what());
    }
}

/**
 * @brief Refund/reversal notice. Callers must send this ONLY when the
 *        underlying refund_capture call actually debited the wallet (ledger
 *        outcome "applied") — never for a redelivery (credited == false)
 *        and never for a durably-recorded-but-skipped attempt (insufficient
 *        balance, or a sub-unit amount that converts to 0 credits). See
 *        BillingController::handleCaptureRefunded's dispatch helper for how
 *        "applied" is determined.
 *
 * @param kind_label Human label — "Refund" or "Reversal".
 */
inline void refund(const Domain::User& user,
                   const std::string& kind_label,
                   std::int64_t amount_cents,
                   const std::string& currency,
                   std::int64_t credits_deducted,
                   std::int64_t new_balance,
                   const std::string& payment_id,
                   const std::string& date) {
    try {
        json ctx;
        ctx["kind_label"] = kind_label;
        ctx["amount"] = detail::format_cents(amount_cents);
        ctx["currency"] = currency;
        ctx["credits_deducted"] = credits_deducted;
        ctx["new_balance"] = new_balance;
        ctx["payment_id"] = payment_id;
        ctx["date"] = date;
        detail::send_rendered("billing_refund", kind_label + " processed", user, ctx);
    } catch (const std::exception& e) {
        spdlog::warn("BillingEmails::refund: failed for {}: {}", Utils::Strings::mask_email(user.email), e.what());
    }
}

/**
 * @brief Failed-payment notice — sent once, at the amount/currency-mismatch
 *        transition inside Billing::credit_capture (the payment is marked
 *        'failed', wallet left untouched). The template MUST make clear the
 *        user was NOT charged — the mismatch is caught before any credit is
 *        ever applied.
 */
inline void failed(const Domain::User& user,
                   std::int64_t amount_cents,
                   const std::string& currency,
                   const std::string& reason,
                   const std::string& date) {
    try {
        json ctx;
        ctx["amount"] = detail::format_cents(amount_cents);
        ctx["currency"] = currency;
        ctx["reason"] = reason;
        ctx["date"] = date;
        detail::send_rendered("billing_failed", "Your payment could not be completed", user, ctx);
    } catch (const std::exception& e) {
        spdlog::warn("BillingEmails::failed: failed for {}: {}", Utils::Strings::mask_email(user.email), e.what());
    }
}

/**
 * @brief Admin wallet-adjustment notice (Task 2). Callers must send this
 *        ONLY when the admin explicitly opted in via the `notify` flag on
 *        POST .../admin/billing/users/{id}/adjust AND Billing::adjust plus
 *        its audit row have already both succeeded — Billing::adjust itself
 *        has no notion of notification, so AdminBillingController::
 *        adjustWallet decides whether to call this at all, after (not
 *        inside) its with_repo_errors lambda, mirroring capture()'s
 *        dispatch pattern in BillingController.hpp (see Task 1 report,
 *        "Fix round 1").
 *
 * @param delta_credits Signed adjustment; rendered as "+250" / "-50" (see
 *        detail::format_signed_credits) — plain integer formatting, no
 *        double anywhere, matching this file's money-handling convention.
 * @param reason The admin's mandatory `note` from the request, reused
 *        verbatim as the notice's reason — adjustWallet has no separate
 *        "reason" field.
 * @param new_balance The wallet balance AFTER this adjustment
 *        (Billing::CreditResult::balance from the adjust() call that
 *        produced it).
 */
inline void adjustment(const Domain::User& user,
                       std::int64_t delta_credits,
                       const std::string& reason,
                       std::int64_t new_balance) {
    try {
        json ctx;
        ctx["delta_credits"] = detail::format_signed_credits(delta_credits);
        ctx["reason"] = reason;
        ctx["new_balance"] = new_balance;
        ctx["date"] = detail::now_iso8601();
        detail::send_rendered("billing_adjustment", "Your wallet balance was adjusted", user, ctx);
    } catch (const std::exception& e) {
        spdlog::warn(
            "BillingEmails::adjustment: failed for {}: {}", Utils::Strings::mask_email(user.email), e.what());
    }
}

}  // namespace Email::BillingEmails
