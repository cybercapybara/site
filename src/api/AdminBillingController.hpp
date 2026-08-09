/**
 * @file AdminBillingController.hpp
 * @brief Admin billing API: sale-package CRUD, the runtime rate/bounds
 *        settings, the payments ledger view, and manual wallet adjustments.
 * @details Every handler: module guard (Core::billing_enabled()) →
 *          API_REQUIRE_ADMIN → Api::with_repo_errors — same shape as
 *          BillingController (Task 4) and AdminController. Every mutation
 *          (package create/update/delete, settings update, manual adjust)
 *          writes a Security::Audit::record() row AFTER its own write
 *          succeeds, mirroring AdminController's user/role CRUD exactly.
 *
 * Money-critical notes:
 *   - POST .../users/{id}/adjust is a thin wrapper over Billing::adjust
 *     (src/billing/Wallet.hpp) — this controller NEVER writes wallet_entries
 *     / wallet_balances directly. `note` must be non-empty (validated here;
 *     Billing::adjust itself does not enforce that) and `admin_id` is always
 *     the authenticated caller's own subject, never a client-supplied value.
 *   - PUT .../settings changes the rate/bounds `billing_settings` row
 *     (migration 009) that BillingController::billing_limits() now reads.
 *     This can NEVER retroactively change an in-flight/already-created
 *     payment: `payments.credits_expected` / `rate_snapshot` are frozen at
 *     creation time (Task 2/4 behavior, untouched by this file) — only a
 *     NEW top-up computed after the change observes it.
 *   - Amounts/credits/rate are integers end to end; no floating point.
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
#include "billing/Wallet.hpp"
#include "core/Core.hpp"
#include "domain/Billing.hpp"
#include "repositories/BillingRepository.hpp"
#include "security/Audit.hpp"
#include "security/Auth.hpp"
#include "utils/ErrorResponse.hpp"

namespace Api {

using namespace drogon;
using json = nlohmann::json;

class AdminBillingController : public HttpController<AdminBillingController> {
public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(AdminBillingController::listPayments, "/api/v1/admin/billing/payments", Get);
    ADD_METHOD_TO(AdminBillingController::listPackages, "/api/v1/admin/billing/packages", Get);
    ADD_METHOD_TO(AdminBillingController::createPackage, "/api/v1/admin/billing/packages", Post);
    ADD_METHOD_TO(AdminBillingController::updatePackage, "/api/v1/admin/billing/packages/{1}", Patch);
    ADD_METHOD_TO(AdminBillingController::deletePackage, "/api/v1/admin/billing/packages/{1}", Delete);
    ADD_METHOD_TO(AdminBillingController::getSettings, "/api/v1/admin/billing/settings", Get);
    ADD_METHOD_TO(AdminBillingController::updateSettings, "/api/v1/admin/billing/settings", Put);
    ADD_METHOD_TO(AdminBillingController::adjustWallet, "/api/v1/admin/billing/users/{1}/adjust", Post);
    METHOD_LIST_END

    // ── GET /api/v1/admin/billing/payments ──────────────────────────────────
    // Paged, newest first. Optional ?status= and ?user_id= filters, combined
    // with AND (both may be given at once). See BillingRepository.hpp's
    // PaymentRepository::list_filtered for the parameter-bound SQL.
    void listPayments(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback) {
        if (!Core::billing_enabled()) {
            callback(ErrorResponse::not_found("billing"));
            return;
        }
        API_REQUIRE_ADMIN(req, callback);
        const auto page = parse_page_params(req, /*default_limit=*/50, /*max_limit=*/200);

        Repositories::PaymentRepository::Filters f;
        const auto status_param = req->getParameter("status");
        if (!status_param.empty())
            f.status = status_param;
        const auto user_id_param = req->getParameter("user_id");
        if (!user_id_param.empty()) {
            if (!is_valid_uuid(user_id_param)) {
                callback(ErrorResponse::bad_request("invalid_user_id", "user_id filter is not a valid UUID"));
                return;
            }
            f.user_id = user_id_param;
        }

        with_repo_errors(callback, "admin billing listPayments", [&] {
            Repositories::PaymentRepository repo;
            auto result = repo.list_filtered(f, page.limit, page.offset);
            json data = json::array();
            for (const auto& p : result.entries)
                data.push_back(p);
            callback(Response::paginated(data, result.total, page.limit, page.offset));
        });
    }

    // ── GET /api/v1/admin/billing/packages ──────────────────────────────────
    // Unlike the user-facing GET /billing/packages, this returns EVERY
    // package (active and inactive) so the admin catalogue view can manage
    // both — PackageRepository::list_active() is deliberately not used here.
    void listPackages(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback) {
        if (!Core::billing_enabled()) {
            callback(ErrorResponse::not_found("billing"));
            return;
        }
        API_REQUIRE_ADMIN(req, callback);
        with_repo_errors(callback, "admin billing listPackages", [&] {
            Repositories::PackageRepository repo;
            // CrudBase::list defaults to LIMIT 100; the catalogue is small but
            // pass a high cap so it isn't silently truncated — same reasoning
            // as AdminController::listRoles.
            auto items = repo.list(1000);
            json data = json::array();
            for (const auto& p : items)
                data.push_back(p);
            callback(Response::list(data));
        });
    }

    // ── POST /api/v1/admin/billing/packages ─────────────────────────────────
    void createPackage(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback) {
        if (!Core::billing_enabled()) {
            callback(ErrorResponse::not_found("billing"));
            return;
        }
        API_REQUIRE_ADMIN(req, callback);
        json body;
        if (!Validation::parse_body(req, body, callback))
            return;

        Validation::Errors errs;
        Validation::require(errs, body, "title");
        Validation::string_length(errs, body, "title", 1, 200);
        Validation::require(errs, body, "amount_cents");
        if (body.contains("amount_cents") && !body["amount_cents"].is_null() &&
            (!body["amount_cents"].is_number_integer() || body["amount_cents"].get<std::int64_t>() <= 0))
            errs.add("amount_cents", "invalid", "amount_cents must be a positive integer");
        Validation::require(errs, body, "credits");
        if (body.contains("credits") && !body["credits"].is_null() &&
            (!body["credits"].is_number_integer() || body["credits"].get<std::int64_t>() <= 0))
            errs.add("credits", "invalid", "credits must be a positive integer");
        if (errs.any()) {
            callback(Validation::response_400(errs));
            return;
        }
        const bool active = body.value("active", true);
        const int sort = body.value("sort", 0);

        with_repo_errors(callback, "admin billing createPackage", [&] {
            Repositories::PackageRepository repo;
            auto created = repo.create(body["title"].get<std::string>(),
                                       body["amount_cents"].get<std::int64_t>(),
                                       body["credits"].get<std::int64_t>(),
                                       active,
                                       sort);
            Security::Audit::record(actor_of(req),
                                    "billing.package.create",
                                    "billing_package",
                                    created.id,
                                    {{"title", created.title}, {"amount_cents", created.amount_cents}});
            callback(Response::created({{"data", json(created)}}));
        });
    }

    // ── PATCH /api/v1/admin/billing/packages/{id} ───────────────────────────
    void updatePackage(const HttpRequestPtr& req,
                       std::function<void(const HttpResponsePtr&)>&& callback,
                       const std::string& id) {
        if (!Core::billing_enabled()) {
            callback(ErrorResponse::not_found("billing"));
            return;
        }
        API_REQUIRE_ADMIN(req, callback);
        if (!is_valid_uuid(id)) {
            callback(ErrorResponse::bad_request("invalid_id", "Malformed package id"));
            return;
        }
        json body;
        if (!Validation::parse_body(req, body, callback))
            return;

        std::optional<std::string> title;
        std::optional<std::int64_t> amount_cents;
        std::optional<std::int64_t> credits;
        std::optional<bool> active;
        std::optional<int> sort;

        Validation::Errors errs;
        if (body.contains("title") && !body["title"].is_null()) {
            Validation::string_length(errs, body, "title", 1, 200);
            if (!errs.any())
                title = body["title"].get<std::string>();
        }
        if (body.contains("amount_cents") && !body["amount_cents"].is_null()) {
            if (!body["amount_cents"].is_number_integer() || body["amount_cents"].get<std::int64_t>() <= 0)
                errs.add("amount_cents", "invalid", "amount_cents must be a positive integer");
            else
                amount_cents = body["amount_cents"].get<std::int64_t>();
        }
        if (body.contains("credits") && !body["credits"].is_null()) {
            if (!body["credits"].is_number_integer() || body["credits"].get<std::int64_t>() <= 0)
                errs.add("credits", "invalid", "credits must be a positive integer");
            else
                credits = body["credits"].get<std::int64_t>();
        }
        if (body.contains("active") && body["active"].is_boolean())
            active = body["active"].get<bool>();
        if (body.contains("sort") && body["sort"].is_number_integer())
            sort = body["sort"].get<int>();
        if (errs.any()) {
            callback(Validation::response_400(errs));
            return;
        }
        if (!title && !amount_cents && !credits && !active && !sort) {
            callback(ErrorResponse::bad_request(
                "empty_patch", "Provide at least one of title / amount_cents / credits / active / sort"));
            return;
        }

        with_repo_errors(callback, "admin billing updatePackage", [&] {
            Repositories::PackageRepository repo;
            auto updated = repo.update(id, title, amount_cents, credits, active, sort);
            Security::Audit::record(actor_of(req), "billing.package.update", "billing_package", id);
            callback(Response::ok({{"data", json(updated)}}));
        });
    }

    // ── DELETE /api/v1/admin/billing/packages/{id} ──────────────────────────
    void deletePackage(const HttpRequestPtr& req,
                       std::function<void(const HttpResponsePtr&)>&& callback,
                       const std::string& id) {
        if (!Core::billing_enabled()) {
            callback(ErrorResponse::not_found("billing"));
            return;
        }
        API_REQUIRE_ADMIN(req, callback);
        if (!is_valid_uuid(id)) {
            callback(ErrorResponse::bad_request("invalid_id", "Malformed package id"));
            return;
        }
        with_repo_errors(callback, "admin billing deletePackage", [&] {
            Repositories::PackageRepository repo;
            repo.remove(id);  // throws PackageNotFound -> 404
            Security::Audit::record(actor_of(req), "billing.package.delete", "billing_package", id);
            callback(Response::ok({{"message", "Package deleted"}}));
        });
    }

    // ── GET /api/v1/admin/billing/settings ──────────────────────────────────
    void getSettings(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback) {
        if (!Core::billing_enabled()) {
            callback(ErrorResponse::not_found("billing"));
            return;
        }
        API_REQUIRE_ADMIN(req, callback);
        with_repo_errors(callback, "admin billing getSettings", [&] {
            Repositories::BillingSettingsRepository repo;
            auto s = repo.get();
            callback(Response::ok({{"data", json(s)}}));
        });
    }

    // ── PUT /api/v1/admin/billing/settings ──────────────────────────────────
    // A full replace (not a partial patch): all three fields are required,
    // so the resulting row is never a mix of an old and a new value the
    // caller never actually agreed to.
    void updateSettings(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback) {
        if (!Core::billing_enabled()) {
            callback(ErrorResponse::not_found("billing"));
            return;
        }
        API_REQUIRE_ADMIN(req, callback);
        json body;
        if (!Validation::parse_body(req, body, callback))
            return;

        Validation::Errors errs;
        Validation::require(errs, body, "credits_per_unit");
        if (body.contains("credits_per_unit") && !body["credits_per_unit"].is_null() &&
            (!body["credits_per_unit"].is_number_integer() || body["credits_per_unit"].get<std::int64_t>() <= 0))
            errs.add("credits_per_unit", "invalid", "credits_per_unit must be a positive integer");
        Validation::require(errs, body, "min_amount_cents");
        if (body.contains("min_amount_cents") && !body["min_amount_cents"].is_null() &&
            (!body["min_amount_cents"].is_number_integer() || body["min_amount_cents"].get<std::int64_t>() <= 0))
            errs.add("min_amount_cents", "invalid", "min_amount_cents must be a positive integer");
        Validation::require(errs, body, "max_amount_cents");
        if (body.contains("max_amount_cents") && !body["max_amount_cents"].is_null() &&
            (!body["max_amount_cents"].is_number_integer() || body["max_amount_cents"].get<std::int64_t>() <= 0))
            errs.add("max_amount_cents", "invalid", "max_amount_cents must be a positive integer");
        // The cross-field bound only makes sense once both individual fields
        // already checked out — deliberately gated on !errs.any(), unlike the
        // independent per-field checks above.
        if (!errs.any() &&
            body["max_amount_cents"].get<std::int64_t>() < body["min_amount_cents"].get<std::int64_t>())
            errs.add("max_amount_cents", "below_min", "max_amount_cents must be >= min_amount_cents");
        if (errs.any()) {
            callback(Validation::response_400(errs));
            return;
        }

        const auto credits_per_unit = body["credits_per_unit"].get<std::int64_t>();
        const auto min_amount_cents = body["min_amount_cents"].get<std::int64_t>();
        const auto max_amount_cents = body["max_amount_cents"].get<std::int64_t>();

        with_repo_errors(callback, "admin billing updateSettings", [&] {
            Repositories::BillingSettingsRepository repo;
            auto updated = repo.update(credits_per_unit, min_amount_cents, max_amount_cents);
            Security::Audit::record(actor_of(req),
                                    "billing.settings.update",
                                    "billing_settings",
                                    "1",
                                    {{"credits_per_unit", credits_per_unit},
                                     {"min_amount_cents", min_amount_cents},
                                     {"max_amount_cents", max_amount_cents}});
            callback(Response::ok({{"data", json(updated)}}));
        });
    }

    // ── POST /api/v1/admin/billing/users/{id}/adjust ────────────────────────
    // Manual wallet credit/debit. Routes through Billing::adjust — this
    // controller never touches wallet_entries/wallet_balances directly.
    // `note` is mandatory (non-empty): Billing::adjust itself does not
    // enforce that, so it's validated here before any DB write. `admin_id`
    // is always the authenticated caller's own subject — never taken from
    // the request body — so wallet_entries.created_by can't be spoofed.
    void adjustWallet(const HttpRequestPtr& req,
                      std::function<void(const HttpResponsePtr&)>&& callback,
                      const std::string& id) {
        if (!Core::billing_enabled()) {
            callback(ErrorResponse::not_found("billing"));
            return;
        }
        API_REQUIRE_ADMIN(req, callback);
        if (!is_valid_uuid(id)) {
            callback(ErrorResponse::bad_request("invalid_id", "Malformed user id"));
            return;
        }
        json body;
        if (!Validation::parse_body(req, body, callback))
            return;

        Validation::Errors errs;
        Validation::require(errs, body, "delta_credits");
        if (!errs.any() && !body["delta_credits"].is_number_integer())
            errs.add("delta_credits", "not_integer", "delta_credits must be an integer");
        Validation::require(errs, body, "note");
        Validation::string_length(errs, body, "note", 1, 2000);
        if (errs.any()) {
            callback(Validation::response_400(errs));
            return;
        }
        const std::int64_t delta_credits = body["delta_credits"].get<std::int64_t>();
        const std::string note = body["note"].get<std::string>();

        auto principal = Security::Auth::principal_of(req);
        const std::string admin_id = principal ? principal->subject : std::string{};

        with_repo_errors(callback, "admin billing adjustWallet", [&] {
            auto result = Billing::adjust(id, delta_credits, note, admin_id);
            Security::Audit::record(admin_id,
                                    "billing.wallet.adjust",
                                    "user",
                                    id,
                                    {{"delta_credits", delta_credits}, {"note", note}});
            callback(Response::ok({{"data", {{"balance", result.balance}, {"credited", result.credited}}}}));
        });
    }

private:
    /// Acting admin's principal subject for the audit trail ("" when auth off).
    static std::string actor_of(const HttpRequestPtr& req) {
        auto p = Security::Auth::principal_of(req);
        return p ? p->subject : std::string{};
    }
};

}  // namespace Api
