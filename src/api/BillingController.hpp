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
 */

#pragma once

#include <cstdint>
#include <functional>
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

private:
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
