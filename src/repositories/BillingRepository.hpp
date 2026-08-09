/**
 * @file BillingRepository.hpp
 * @brief All SQL touching `billing_packages` and `payments` lives here.
 *
 * `wallet_entries` / `wallet_balances` are deliberately NOT exposed by a
 * repository in this file: src/billing/Wallet.hpp is the only code allowed
 * to write the ledger (one atomic transaction spanning both tables plus the
 * `payments` row it credits/refunds against), and it talks to those two
 * tables directly. Keeping that write path out of a general-purpose
 * repository is what makes "wallet_entries is insert-only, no update/delete
 * method exists anywhere" true by construction rather than by convention.
 */

#pragma once

#include <cstdint>
#include <optional>
#include <pqxx/pqxx>
#include <stdexcept>
#include <string>
#include <vector>

#include "database/Database.hpp"
#include "domain/Billing.hpp"
#include "repositories/CrudBase.hpp"
#include "repositories/RepoErrors.hpp"
#include "repositories/SqlErrors.hpp"

namespace Repositories {

// Stable 409/404 codes carried on the exception — see RoleRepository::RoleInUse
// for the pattern this mirrors.
struct PackageNotFound : NotFoundError {
    PackageNotFound() : NotFoundError("billing_package") {}
};

struct PaymentNotFound : NotFoundError {
    PaymentNotFound() : NotFoundError("payment") {}
};

struct DuplicatePaymentOrder : ConflictError {
    DuplicatePaymentOrder() : ConflictError("order_exists", "A payment for that provider order already exists") {}
};

/**
 * @brief `billing_packages` — the admin-managed sale catalogue.
 */
class PackageRepository : public CrudBase<PackageRepository, Domain::Package, std::string> {
public:
    // CrudBase contract — supplies find(id) / list(limit,offset) / count().
    static constexpr const char* kTable = "billing_packages";
    static constexpr const char* kColumns = "id, title, amount_cents, credits, active, sort, created_at, updated_at";
    static constexpr const char* kIdColumn = "id";
    static constexpr const char* kOrderBy = "sort ASC, created_at ASC";

    /// Packages shown to end users on the top-up screen.
    std::vector<Domain::Package> list_active() {
        return Database::get().execute_read([&](auto& txn) {
            auto r = txn.exec(std::string("SELECT ") + kColumns +
                              " FROM billing_packages WHERE active = TRUE ORDER BY " + kOrderBy);
            std::vector<Domain::Package> out;
            out.reserve(r.size());
            for (const auto& row : r)
                out.push_back(Domain::Package::from_row(row));
            return out;
        });
    }

    Domain::Package create(const std::string& title, std::int64_t amount_cents, std::int64_t credits, bool active, int sort) {
        return Database::get().execute_write([&](auto& txn) {
            auto r = txn.exec_params(std::string("INSERT INTO billing_packages (title, amount_cents, credits, active, sort) "
                                                 "VALUES ($1, $2, $3, $4, $5) RETURNING ") +
                                         kColumns,
                                     title,
                                     amount_cents,
                                     credits,
                                     active,
                                     sort);
            return Domain::Package::from_row(r[0]);
        });
    }

    /// Partial update: pass nullopt for any field that should keep its current value.
    Domain::Package update(const std::string& id,
                           std::optional<std::string> title,
                           std::optional<std::int64_t> amount_cents,
                           std::optional<std::int64_t> credits,
                           std::optional<bool> active,
                           std::optional<int> sort) {
        return Database::get().execute_write([&](auto& txn) {
            auto r = txn.exec_params(std::string("UPDATE billing_packages SET "
                                                 "title = COALESCE($1, title), "
                                                 "amount_cents = COALESCE($2, amount_cents), "
                                                 "credits = COALESCE($3, credits), "
                                                 "active = COALESCE($4, active), "
                                                 "sort = COALESCE($5, sort) "
                                                 "WHERE id = $6 RETURNING ") +
                                         kColumns,
                                     title,
                                     amount_cents,
                                     credits,
                                     active,
                                     sort,
                                     id);
            if (r.empty())
                throw PackageNotFound{};
            return Domain::Package::from_row(r[0]);
        });
    }

    void remove(const std::string& id) {
        Database::get().execute_write([&](auto& txn) {
            auto r = txn.exec_params("DELETE FROM billing_packages WHERE id = $1 RETURNING id", id);
            if (r.empty())
                throw PackageNotFound{};
            return 0;
        });
    }
};

/**
 * @brief `payments` — one row per PayPal order. Rows are never deleted
 *        (payments.user_id is ON DELETE RESTRICT — see UserRepository::remove)
 *        so there is deliberately no remove() here.
 */
class PaymentRepository : public CrudBase<PaymentRepository, Domain::Payment, std::string> {
public:
    // CrudBase contract. kOwnerColumn enables find_owned/list_owned/count_owned
    // for the user-facing "my payments" reads (a later task) without an IDOR.
    static constexpr const char* kTable = "payments";
    static constexpr const char* kColumns =
        "id, user_id, provider, provider_order_id, provider_capture_id, amount_cents, currency, "
        "credits_expected, rate_snapshot, package_id, status, failure_reason, created_at, updated_at";
    static constexpr const char* kIdColumn = "id";
    static constexpr const char* kOwnerColumn = "user_id";
    static constexpr const char* kOrderBy = "created_at DESC";

    /**
     * @brief Create a `created` payment row for a fresh PayPal order.
     *        Throws DuplicatePaymentOrder on UNIQUE(provider_order_id).
     */
    Domain::Payment create(const std::string& user_id,
                           const std::string& provider_order_id,
                           std::int64_t amount_cents,
                           const std::string& currency,
                           std::int64_t credits_expected,
                           std::int64_t rate_snapshot,
                           std::optional<std::string> package_id) {
        return detail::translate_sql(
            [&] {
                return Database::get().execute_write([&](auto& txn) {
                    auto r = txn.exec_params(
                        std::string("INSERT INTO payments (user_id, provider, provider_order_id, amount_cents, "
                                    "currency, credits_expected, rate_snapshot, package_id, status) "
                                    "VALUES ($1, 'paypal', $2, $3, $4, $5, $6, $7, 'created') RETURNING ") +
                            kColumns,
                        user_id,
                        provider_order_id,
                        amount_cents,
                        currency,
                        credits_expected,
                        rate_snapshot,
                        package_id);
                    return Domain::Payment::from_row(r[0]);
                });
            },
            [](std::string_view ss) {
                if (ss == "23505")
                    throw DuplicatePaymentOrder{};
            });
    }

    std::optional<Domain::Payment> find_by_order_id(const std::string& provider_order_id) {
        return Database::get().execute_read([&](auto& txn) -> std::optional<Domain::Payment> {
            auto r = txn.exec_params(std::string("SELECT ") + kColumns + " FROM payments WHERE provider_order_id = $1",
                                     provider_order_id);
            if (r.empty())
                return std::nullopt;
            return Domain::Payment::from_row(r[0]);
        });
    }

    std::optional<Domain::Payment> find_by_capture_id(const std::string& provider_capture_id) {
        return Database::get().execute_read([&](auto& txn) -> std::optional<Domain::Payment> {
            auto r = txn.exec_params(
                std::string("SELECT ") + kColumns + " FROM payments WHERE provider_capture_id = $1", provider_capture_id);
            if (r.empty())
                return std::nullopt;
            return Domain::Payment::from_row(r[0]);
        });
    }

    /**
     * @brief PayPal order approved (buyer authorised, not yet captured).
     *        Only fires from `created` so an out-of-order webhook can't
     *        regress an already-captured/refunded payment back to `approved`.
     *
     * @return true if the transition applied; false if the order is known
     *         but no longer in `created` (already approved/captured/failed/
     *         refunded) — a duplicate or out-of-order APPROVED webhook is a
     *         no-op, not an error, so PayPal doesn't redeliver it forever.
     * @throws PaymentNotFound only when @p provider_order_id is genuinely
     *         unknown.
     */
    bool mark_approved(const std::string& provider_order_id) {
        return Database::get().execute_write([&](auto& txn) {
            auto r = txn.exec_params(
                "UPDATE payments SET status = 'approved' WHERE provider_order_id = $1 AND status = 'created' "
                "RETURNING id",
                provider_order_id);
            if (!r.empty())
                return true;
            auto pr = txn.exec_params("SELECT id FROM payments WHERE provider_order_id = $1", provider_order_id);
            if (pr.empty())
                throw PaymentNotFound{};
            return false;
        });
    }

    std::vector<Domain::Payment> list_by_status(const std::string& status, int limit, int offset) {
        return Database::get().execute_read([&](auto& txn) {
            auto r = txn.exec_params(std::string("SELECT ") + kColumns +
                                         " FROM payments WHERE status = $1 ORDER BY created_at DESC LIMIT $2 OFFSET $3",
                                     status,
                                     limit,
                                     offset);
            std::vector<Domain::Payment> out;
            out.reserve(r.size());
            for (const auto& row : r)
                out.push_back(Domain::Payment::from_row(row));
            return out;
        });
    }
};

}  // namespace Repositories
