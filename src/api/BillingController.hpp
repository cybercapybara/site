/**
 * @file BillingController.hpp
 * @brief User-facing billing API: browse packages, top up (PayPal checkout),
 *        capture a return-flow order, and read your own wallet.
 * @details Every handler: module guard (Core::billing_enabled()) →
 *          API_REQUIRE_PRINCIPAL → Api::with_repo_errors. This controller
 *          NEVER writes wallet_entries/wallet_balances directly — every
 *          credit change routes through Billing::credit_capture()
 *          (src/billing/Wallet.hpp), the only code allowed to touch the
 *          ledger. PayPal is only ever reached through Billing::PayPalClient
 *          (src/billing/PayPalClient.hpp), whose install_for_testing() seam
 *          is what lets tests/integration/test_billing_api.cpp exercise this
 *          controller with zero network calls.
 *
 * Security invariants (see Task 4 brief — a reviewer already flagged these):
 *   - POST .../capture takes an order id from the request body. Before ever
 *     driving a capture, ownership is verified via PaymentRepository::
 *     find_owned (kOwnerColumn="user_id") — a caller can never capture (and
 *     collect credits for) an order that belongs to a different user.
 *   - POST .../topup computes credits SERVER-SIDE only, from either the
 *     package's own frozen `credits` column or `amount_cents *
 *     billing.credits_per_unit / 100` (integer math). Any "credits" field on
 *     the request body is never read anywhere in this file — a client can
 *     send whatever it wants there and it has zero effect.
 *   - GET .../wallet takes no user-id parameter of any kind — it always
 *     reads the authenticated principal's own wallet.
 *   - Every amount is an integer (cents / credits); billing.min_amount_cents
 *     / billing.max_amount_cents bound a custom top-up amount.
 *
 * POST .../paypal/webhook (Task 5) — PayPal server-to-server notification,
 * NOT a user request:
 *   - Public (src/utils/Strings.hpp kDefaultPublicPathsCsv + config/config.json
 *     api.public_paths — BOTH, see that file's comment on the content-module
 *     incident) and CSRF-exempt by construction: Security::Csrf::passes()
 *     short-circuits on an empty access cookie, and PayPal never presents
 *     one (see Middleware.hpp / Csrf.hpp) — no path-based exemption needed.
 *   - The signature (PayPalClient::verify_webhook_signature) is checked
 *     against the RAW request body BEFORE that body is ever parsed as
 *     trusted JSON. No authenticated principal is required or possible.
 *   - Response codes are NOT the usual REST mapping — PayPal retries any
 *     non-2xx for days, so "handled" and "deliberately ignored" both answer
 *     200. See webhook()'s own doc comment for the exact 200/401/5xx rules.
 */

#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>

#include <drogon/HttpController.h>

#include <nlohmann/json.hpp>

#include "api/Guards.hpp"
#include "api/HandlerSupport.hpp"
#include "api/RequestUtils.hpp"
#include "api/Validation.hpp"
#include "billing/PayPalClient.hpp"
#include "billing/Wallet.hpp"
#include "core/Core.hpp"
#include "domain/Billing.hpp"
#include "repositories/BillingRepository.hpp"
#include "utils/Config.hpp"
#include "utils/ErrorResponse.hpp"

namespace Api {

using namespace drogon;
using json = nlohmann::json;

class BillingController : public HttpController<BillingController> {
public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(BillingController::listPackages, "/api/v1/billing/packages", Get);
    ADD_METHOD_TO(BillingController::getWallet, "/api/v1/billing/wallet", Get);
    ADD_METHOD_TO(BillingController::topup, "/api/v1/billing/topup", Post);
    ADD_METHOD_TO(BillingController::capture, "/api/v1/billing/capture", Post);
    ADD_METHOD_TO(BillingController::paypalWebhook, "/api/v1/billing/paypal/webhook", Post);
    METHOD_LIST_END

    // ── GET /api/v1/billing/packages ──────────────────────────────────────
    // Also returns the current per-unit rate and the custom-amount bounds —
    // Task 8's custom-amount input validates client-side against these, and
    // the client has no other way to learn them (they're never guessable
    // from the package list alone: a package's own price/credits ratio can
    // legitimately differ from the generic rate — see topup's doc comment).
    void listPackages(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback) {
        if (!Core::billing_enabled()) {
            callback(ErrorResponse::not_found("billing"));
            return;
        }
        API_REQUIRE_PRINCIPAL(req, callback, principal);
        with_repo_errors(callback, "billing listPackages", [&] {
            Repositories::PackageRepository repo;
            auto items = repo.list_active();
            json data = json::array();
            for (const auto& p : items)
                data.push_back(p);
            std::int64_t rate = 0, min_cents = 0, max_cents = 0;
            billing_limits(rate, min_cents, max_cents);
            callback(Response::ok({{"data", data},
                                   {"credits_per_unit", rate},
                                   {"min_amount_cents", min_cents},
                                   {"max_amount_cents", max_cents}}));
        });
    }

    // ── GET /api/v1/billing/wallet ────────────────────────────────────────
    // No user-id parameter of any kind is accepted — always the caller's own
    // wallet, resolved solely from the authenticated principal.
    void getWallet(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback) {
        if (!Core::billing_enabled()) {
            callback(ErrorResponse::not_found("billing"));
            return;
        }
        API_REQUIRE_PRINCIPAL(req, callback, principal);
        const auto page = parse_page_params(req, /*default_limit=*/20, /*max_limit=*/100);
        with_repo_errors(callback, "billing getWallet", [&] {
            const auto balance = Billing::balance_of(principal->subject);
            auto hist = Billing::history(principal->subject, page.limit, page.offset);
            json data = json::array();
            for (const auto& e : hist)
                data.push_back(public_wallet_entry(e));
            callback(Response::ok(
                {{"data", {{"balance", balance}, {"history", data}}}, {"limit", page.limit}, {"offset", page.offset}}));
        });
    }

    // ── POST /api/v1/billing/topup ────────────────────────────────────────
    void topup(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback) {
        if (!Core::billing_enabled()) {
            callback(ErrorResponse::not_found("billing"));
            return;
        }
        API_REQUIRE_PRINCIPAL(req, callback, principal);
        json body;
        if (!Validation::parse_body(req, body, callback))
            return;

        TopupPlan plan;
        if (!resolve_topup_plan(body, plan, callback))
            return;

        auto& cfg = Config::get();
        const std::string currency = cfg.get<std::string>("billing.currency", "BILLING_CURRENCY", "USD");
        const std::string return_url = cfg.get<std::string>("billing.paypal.return_url", "PAYPAL_RETURN_URL", "");
        const std::string cancel_url = cfg.get<std::string>("billing.paypal.cancel_url", "PAYPAL_CANCEL_URL", "");

        with_repo_errors(callback, "billing topup", [&] {
            // reference_id is PayPal-dashboard reconciliation only (see
            // PayPalClient::create_order's doc comment) — it is never read
            // back and never influences credited amounts.
            const std::string reference = "topup:" + principal->subject;
            auto order = Billing::PayPalClient::get().create_order(plan.amount_cents,
                                                                    currency,
                                                                    reference,
                                                                    return_url,
                                                                    cancel_url);

            Repositories::PaymentRepository payments;
            payments.create(principal->subject,
                            order.order_id,
                            plan.amount_cents,
                            currency,
                            plan.credits_expected,
                            plan.rate_snapshot,
                            plan.package_id);

            // The response deliberately carries the approve URL (+ order id,
            // needed to later call /capture) only — the client never sees or
            // supplies a credit count.
            auto resp = Response::ok({{"data", {{"order_id", order.order_id}, {"approve_url", order.approve_url}}}});
            resp->setStatusCode(k201Created);
            callback(resp);
        });
    }

    // ── POST /api/v1/billing/capture ──────────────────────────────────────
    void capture(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback) {
        if (!Core::billing_enabled()) {
            callback(ErrorResponse::not_found("billing"));
            return;
        }
        API_REQUIRE_PRINCIPAL(req, callback, principal);
        json body;
        if (!Validation::parse_body(req, body, callback))
            return;
        Validation::Errors errs;
        Validation::require(errs, body, "order_id");
        if (!errs.any() && !body["order_id"].is_string())
            errs.add("order_id", "not_string", "order_id must be a string");
        if (errs.any()) {
            callback(Validation::response_400(errs));
            return;
        }
        const std::string order_id = body["order_id"].get<std::string>();

        with_repo_errors(callback, "billing capture", [&] {
            Repositories::PaymentRepository payments;
            // Read the PRIMARY: a capture attempted moments after topup (the
            // common case — the buyer approves on PayPal and bounces straight
            // back) must not spuriously 404 because of replica lag.
            auto payment = payments.find_by_order_id(order_id, /*from_primary=*/true);
            if (!payment) {
                callback(ErrorResponse::not_found("payment"));
                return;
            }
            // SECURITY: order_id comes straight from the request body — an
            // unauthorized order id must never reach PayPalClient::capture_order
            // or Billing::credit_capture. Read the PRIMARY: this check is the
            // only thing standing between an attacker and someone else's
            // wallet, so a lagging replica must never make it fail open.
            auto owned = payments.find_owned(payment->id, principal->subject, /*from_primary=*/true);
            if (!owned) {
                // Same 404 whether the order doesn't exist or belongs to
                // someone else — no signal to a caller probing order ids.
                callback(ErrorResponse::not_found("payment"));
                return;
            }

            if (owned->status == Domain::PaymentStatus::kCaptured) {
                // Idempotent short-circuit: never re-issue a capture call to
                // PayPal for an order this service already captured — a
                // second real PayPal capture on an already-captured order is
                // an error on PayPal's side, not a no-op.
                const auto balance = Billing::balance_of(principal->subject, /*from_primary=*/true);
                callback(Response::ok({{"data", {{"credited", false}, {"balance", balance}, {"status", "captured"}}}}));
                return;
            }
            if (owned->status == Domain::PaymentStatus::kFailed || owned->status == Domain::PaymentStatus::kRefunded) {
                // A clean 409, no PayPal call — capturing an order that's
                // already failed or refunded on our side can only ever error
                // on PayPal's side too (or worse, be misleading).
                callback(ErrorResponse::conflict(
                    "payment_not_capturable", "This payment is " + owned->status + " and cannot be captured"));
                return;
            }

            Billing::PayPalCapture cap;
            try {
                cap = Billing::PayPalClient::get().capture_order(order_id);
            } catch (const std::exception& e) {
                // PayPal's own "buyer hasn't approved this order yet" error —
                // a real, expected 4xx-shaped condition (a client racing the
                // approve redirect), not a server fault.
                if (std::string(e.what()).find("ORDER_NOT_APPROVED") != std::string::npos) {
                    callback(ErrorResponse::conflict("order_not_approved",
                                                     "This PayPal order has not been approved yet"));
                    return;
                }
                throw;  // anything else: with_repo_errors' outer catch maps it to 500.
            }

            if (cap.status != "COMPLETED") {
                // PayPal answers 2xx for PENDING (eCheck, fraud review) and
                // DECLINED captures too — crediting on either would hand out
                // credits for money that may never settle. Leave the payment
                // uncaptured (still created/approved) so the webhook (Task 5)
                // can resolve it once PayPal reaches a final state.
                const auto balance = Billing::balance_of(principal->subject);
                callback(Response::ok({{"data",
                                        {{"credited", false},
                                         {"balance", balance},
                                         {"status", cap.status},
                                         {"pending", true}}}}));
                return;
            }

            auto result = Billing::credit_capture(order_id, cap.capture_id, cap.amount_cents, cap.currency);
            callback(Response::ok(
                {{"data", {{"credited", result.credited}, {"balance", result.balance}, {"status", "captured"}}}}));
        });
    }

    // ── POST /api/v1/billing/paypal/webhook ─────────────────────────────────
    // PayPal server-to-server notification. Public, unauthenticated, CSRF-
    // exempt by construction (see the class doc comment). Response codes:
    //   - 401: verify_webhook_signature() returned false (malformed body,
    //     missing paypal-* headers, or PayPal itself said the signature
    //     doesn't check out). Nothing is credited/refunded past this point.
    //   - 5xx (via with_repo_errors' std::exception catch -> 500): PayPal's
    //     OWN verify-webhook-signature API was unreachable or answered
    //     non-2xx (verify_webhook_signature() THROWS for this — see
    //     PayPalClient.hpp's class doc comment). This is a real "ask me
    //     again later", unlike every other branch below.
    //   - 200: everything else — a credit/refund actually applied, an
    //     already-processed event replayed as a no-op, an event type this
    //     handler deliberately doesn't act on, or (KNOWN GAP, see
    //     handleCaptureReversed's doc comment) a reversal event it can't yet
    //     structurally represent. PayPal retries any non-2xx for days, so
    //     every one of these must answer 200 or PayPal will hammer this
    //     endpoint forever for a condition retrying can never fix.
    void paypalWebhook(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback) {
        if (!Core::billing_enabled()) {
            callback(ErrorResponse::not_found("billing"));
            return;
        }

        // RAW body, exactly as received — verified BEFORE any trusted parse.
        // See PayPalClient::verify_webhook_signature's doc comment: it does
        // its own json::parse of this same string to build PayPal's verify
        // request, but nothing upstream may re-serialize/re-derive it first.
        const std::string raw_body(req->body());
        const auto headers = collect_paypal_headers(req);

        bool verified = false;
        try {
            verified = Billing::PayPalClient::get().verify_webhook_signature(headers, raw_body);
        } catch (const std::exception& e) {
            // Transport/non-2xx from PayPal's OWN verify API — "we couldn't
            // ask PayPal", never conflated with "PayPal said no" (see
            // PayPalClient.hpp). 500 tells PayPal to retry later.
            spdlog::error("billing webhook: verify-webhook-signature API unreachable: {}", e.what());
            callback(ErrorResponse::internal_error());
            return;
        }
        if (!verified) {
            spdlog::warn("billing webhook: signature verification failed — rejecting, nothing credited/refunded");
            callback(ErrorResponse::unauthorized("invalid_signature"));
            return;
        }

        // Verified true implies raw_body was valid JSON (verify_webhook_signature
        // parses it itself and returns false otherwise) — this re-parse is
        // defensive only, never expected to fail in practice.
        json event;
        try {
            event = json::parse(raw_body);
        } catch (const json::parse_error& e) {
            spdlog::error("billing webhook: signature-verified body failed to re-parse (unreachable in practice): {}",
                         e.what());
            callback(Response::ok({{"data", {{"handled", false}}}}));
            return;
        }

        const std::string event_id = event.value("id", std::string());
        const std::string event_type = event.value("event_type", std::string());
        // Every received (signature-valid) event id, at info level — the
        // dedupe/debug trail the brief asks for, independent of whether this
        // handler acts on the event.
        spdlog::info("billing webhook: received event id={} type={}", event_id, event_type);

        auto respond_handled = [callback](bool handled) { callback(Response::ok({{"data", {{"handled", handled}}}})); };

        if (event_type == "PAYMENT.CAPTURE.COMPLETED") {
            handleCaptureCompleted(event, event_id, respond_handled);
        } else if (event_type == "PAYMENT.CAPTURE.REFUNDED") {
            handleCaptureRefunded(event, event_id, respond_handled);
        } else if (event_type == "PAYMENT.CAPTURE.REVERSED") {
            handleCaptureReversed(event, event_id, respond_handled);
        } else {
            spdlog::info("billing webhook: ignoring unrelated event type '{}' (event {})", event_type, event_id);
            respond_handled(false);
        }
    }

private:
    // Case-insensitive per Drogon's HttpRequest::getHeader — collects only
    // the five paypal-* headers verify_webhook_signature actually reads
    // (PayPalClient::detail::find_header_ci does its own case-insensitive
    // lookup within this map, so the case used as keys here doesn't matter).
    static std::map<std::string, std::string> collect_paypal_headers(const HttpRequestPtr& req) {
        std::map<std::string, std::string> h;
        auto add = [&](const char* name) {
            std::string v = req->getHeader(name);
            if (!v.empty())
                h[name] = v;
        };
        add("Paypal-Auth-Algo");
        add("Paypal-Cert-Url");
        add("Paypal-Transmission-Id");
        add("Paypal-Transmission-Sig");
        add("Paypal-Transmission-Time");
        return h;
    }

    // PayPal's "up" link on a v2 refund resource points at
    // .../v2/payments/captures/{capture_id} — the refund resource itself
    // carries no direct capture_id field. Returns "" if no "up" link is
    // present (malformed/unexpected payload).
    static std::string extract_capture_id_from_links(const json& resource) {
        if (!resource.contains("links") || !resource["links"].is_array())
            return {};
        for (const auto& link : resource["links"]) {
            if (!link.is_object() || link.value("rel", std::string()) != "up")
                continue;
            const std::string href = link.value("href", std::string());
            const auto pos = href.find_last_of('/');
            if (pos == std::string::npos || pos + 1 >= href.size())
                continue;
            return href.substr(pos + 1);
        }
        return {};
    }

    // PAYMENT.CAPTURE.COMPLETED → credit. Handles BOTH "the user never
    // returned, this webhook is the only signal we ever get" AND "a capture
    // that was PENDING at return-flow time (BillingController::capture left
    // provider_capture_id NULL) now resolves to COMPLETED" — both funnel
    // into the exact same Billing::credit_capture call, whose own guarded
    // UPDATE (WHERE provider_capture_id IS NULL) makes a capture already
    // credited via the return flow a true no-op here (credited=false, same
    // balance, no second ledger row).
    static void handleCaptureCompleted(const json& event, const std::string& event_id, std::function<void(bool)> respond) {
        const json resource = event.value("resource", json::object());
        std::string order_id, capture_id, currency;
        std::int64_t amount_cents = 0;
        try {
            order_id = resource.at("supplementary_data").at("related_ids").at("order_id").get<std::string>();
            capture_id = resource.at("id").get<std::string>();
            currency = resource.at("amount").at("currency_code").get<std::string>();
            amount_cents = Billing::detail::parse_decimal_to_cents(resource.at("amount").at("value").get<std::string>());
        } catch (const std::exception& e) {
            spdlog::error("billing webhook: malformed PAYMENT.CAPTURE.COMPLETED resource (event {}): {}", event_id, e.what());
            respond(false);
            return;
        }

        try {
            auto result = Billing::credit_capture(order_id, capture_id, amount_cents, currency);
            spdlog::info("billing webhook: capture {} order {} (event {}) — credited={} balance={}",
                         capture_id,
                         order_id,
                         event_id,
                         result.credited,
                         result.balance);
        } catch (const std::exception& e) {
            // An unknown order id, a capture id already claimed by a
            // different order, or any other repository-layer anomaly: log
            // loudly for manual reconciliation but still ack 200 — retrying
            // this exact event can never resolve a structural mismatch, and
            // a non-2xx here just means PayPal hammers this endpoint for
            // days over a condition that will never change on its own.
            spdlog::error("billing webhook: credit_capture failed for order {} capture {} (event {}): {}",
                          order_id,
                          capture_id,
                          event_id,
                          e.what());
        }
        respond(true);
    }

    // PAYMENT.CAPTURE.REFUNDED → refund, keyed on PayPal's OWN refund id
    // (resource.id) as the idempotency key into Billing::refund_capture —
    // per Task 2's report, the durable `billing_refunds` row on that id is
    // what makes a redelivered refund event a true no-op.
    static void handleCaptureRefunded(const json& event, const std::string& event_id, std::function<void(bool)> respond) {
        const json resource = event.value("resource", json::object());
        std::string refund_id, currency, capture_id;
        std::int64_t amount_cents = 0;
        try {
            refund_id = resource.at("id").get<std::string>();
            currency = resource.at("amount").at("currency_code").get<std::string>();
            amount_cents = Billing::detail::parse_decimal_to_cents(resource.at("amount").at("value").get<std::string>());
            capture_id = extract_capture_id_from_links(resource);
        } catch (const std::exception& e) {
            spdlog::error("billing webhook: malformed PAYMENT.CAPTURE.REFUNDED resource (event {}): {}", event_id, e.what());
            respond(false);
            return;
        }
        if (capture_id.empty()) {
            spdlog::error(
                "billing webhook: PAYMENT.CAPTURE.REFUNDED refund {} (event {}) has no 'up' link — cannot resolve "
                "its capture id, refund not applied",
                refund_id,
                event_id);
            respond(false);
            return;
        }

        try {
            auto result = Billing::refund_capture(capture_id, refund_id, amount_cents);
            spdlog::info("billing webhook: refund {} capture {} (event {}) — newly recorded={} balance={}",
                         refund_id,
                         capture_id,
                         event_id,
                         result.credited,
                         result.balance);
        } catch (const std::exception& e) {
            // Unknown capture, or an amount PayPal itself reports that fails
            // our own sanity bounds (InvalidRefundAmount) — see the note on
            // handleCaptureCompleted's catch: logged loudly, still 200,
            // retrying changes nothing.
            spdlog::error("billing webhook: refund_capture failed for capture {} refund {} (event {}): {}",
                          capture_id,
                          refund_id,
                          event_id,
                          e.what());
        }
        respond(true);
    }

    // KNOWN GAP (flagged in Task 2's report, decided here): PayPal can later
    // reverse a refund (PAYMENT.CAPTURE.REVERSED, or void a refund outright)
    // and this schema has no representation for it — migrations/
    // 008_billing_refunds.sql's `billing_refunds` table records refunds
    // applied, never a later reversal of one. If reversal were silently
    // ignored, the stale billing_refunds row from the original refund stays
    // in refund_capture's step-3 cumulative-total check forever, which could
    // wrongly refuse a later LEGITIMATE refund on the same payment as
    // "cumulative total exceeds amount_cents" even though the reversed
    // refund never actually took money out a second time.
    //
    // Deferring an actual fix to a follow-up task is deliberate, not an
    // oversight: representing a reversal correctly needs a real schema/
    // Wallet.hpp change (at minimum, a way to mark a billing_refunds row
    // voided and re-credit the wallet for it) — a money-semantics change on
    // par with Task 2's own work, out of scope for Task 5's Files list
    // (BillingController.hpp / Endpoints.hpp / openapi.yaml / Strings.hpp /
    // config.json only, no migration). PayPal reversing a refund it itself
    // just paid out is also an operationally rare event (typically a bank
    // dispute on the refund transfer itself), not a routine flow.
    //
    // What this DOES do, so the gap is loud instead of silent: log at ERROR
    // (not info, unlike a genuinely-ignored event type) with the event id
    // and, if resolvable, the capture id — so it shows up in on-call/alert
    // pipelines and the next task to touch billing has a concrete trail to
    // follow instead of rediscovering this from scratch. Still acks 200:
    // there is nothing this handler CAN do about it today, and refusing the
    // ack would just make PayPal retry an event we're already unable to act
    // on for days.
    static void handleCaptureReversed(const json& event, const std::string& event_id, std::function<void(bool)> respond) {
        const json resource = event.value("resource", json::object());
        const std::string capture_id = extract_capture_id_from_links(resource);
        spdlog::error(
            "billing webhook: KNOWN GAP — received PAYMENT.CAPTURE.REVERSED (event {}, capture {}) with no schema "
            "representation for a refund reversal; the original refund's billing_refunds row is NOT adjusted. "
            "Manual reconciliation required — see BillingController::handleCaptureReversed's doc comment",
            event_id,
            capture_id.empty() ? "<unresolved>" : capture_id);
        respond(false);
    }

    // Strips `created_by` (an admin's raw UUID on adjustment rows) before a
    // wallet_entries row is ever handed to an end user — Domain::to_json
    // includes it for internal/admin views, but this endpoint is user-facing.
    static json public_wallet_entry(const Domain::WalletEntry& e) {
        return json{
            {"id", e.id},
            {"user_id", e.user_id},
            {"delta_credits", e.delta_credits},
            {"kind", e.kind},
            {"reference", e.reference},
            {"note", e.note},
            {"created_at", e.created_at},
        };
    }

    // Config::parse_env_value<T> has no std::int64_t specialization (only
    // int/long/double/bool/string — see utils/Config.hpp) and silently falls
    // back to T{} = 0 for an unknown T, so every money-shaped config read
    // goes through `long` (this codebase's existing convention, e.g.
    // server.max_body_bytes in main.cpp) and is narrowed to std::int64_t
    // afterward, never read as std::int64_t directly.
    static void billing_limits(std::int64_t& rate, std::int64_t& min_cents, std::int64_t& max_cents) {
        auto& cfg = Config::get();
        rate = static_cast<std::int64_t>(cfg.get<long>("billing.credits_per_unit", "BILLING_CREDITS_PER_UNIT", 100));
        min_cents =
            static_cast<std::int64_t>(cfg.get<long>("billing.min_amount_cents", "BILLING_MIN_AMOUNT_CENTS", 100));
        max_cents =
            static_cast<std::int64_t>(cfg.get<long>("billing.max_amount_cents", "BILLING_MAX_AMOUNT_CENTS", 100000));
    }

    struct TopupPlan {
        std::int64_t amount_cents = 0;
        std::int64_t credits_expected = 0;
        std::int64_t rate_snapshot = 0;
        std::optional<std::string> package_id;
    };

    // Resolves amount_cents/credits_expected/rate_snapshot/package_id
    // entirely server-side. Deliberately never reads a "credits" key from
    // @p body at any point — the caller cannot influence the credited
    // amount by sending one. min/max_amount_cents is enforced on the
    // resulting amount_cents for BOTH branches — a misconfigured package
    // price outside the configured bounds is refused just like an
    // out-of-range custom amount, not silently sold.
    static bool resolve_topup_plan(const json& body,
                                   TopupPlan& out,
                                   const std::function<void(const HttpResponsePtr&)>& callback) {
        std::int64_t min_cents = 0, max_cents = 0;
        billing_limits(out.rate_snapshot, min_cents, max_cents);

        const bool has_package = body.contains("package_id") && body["package_id"].is_string() &&
                                 !body["package_id"].get<std::string>().empty();
        const bool has_amount = body.contains("amount_cents") && !body["amount_cents"].is_null();

        if (has_package == has_amount) {
            callback(ErrorResponse::bad_request("invalid_topup_request",
                                                "Provide exactly one of package_id or amount_cents"));
            return false;
        }

        if (has_package) {
            const std::string package_id = body["package_id"].get<std::string>();
            if (!is_valid_uuid(package_id)) {
                callback(ErrorResponse::bad_request("invalid_uuid", "package_id is not a valid UUID"));
                return false;
            }
            Repositories::PackageRepository packages;
            auto pkg = packages.find(package_id);
            if (!pkg || !pkg->active) {
                callback(ErrorResponse::not_found("billing_package"));
                return false;
            }
            out.package_id = pkg->id;
            out.amount_cents = pkg->amount_cents;
            // Frozen exactly as priced by the admin catalogue — never
            // re-derived from the current per-unit rate.
            out.credits_expected = pkg->credits;
        } else {
            if (!body["amount_cents"].is_number_integer()) {
                callback(ErrorResponse::bad_request("not_integer", "amount_cents must be an integer"));
                return false;
            }
            out.amount_cents = body["amount_cents"].get<std::int64_t>();
            // Integer math only — see Billing::refund_capture's identical rule.
            out.credits_expected = (out.amount_cents * out.rate_snapshot) / 100;
        }

        if (out.amount_cents <= 0 || out.amount_cents < min_cents || out.amount_cents > max_cents) {
            const char* code = has_package ? "package_price_out_of_range" : "amount_out_of_range";
            callback(ErrorResponse::bad_request(
                code,
                "amount_cents must be between " + std::to_string(min_cents) + " and " + std::to_string(max_cents)));
            return false;
        }
        return true;
    }
};

}  // namespace Api
