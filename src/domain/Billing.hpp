/**
 * @file Billing.hpp
 * @brief Domain rows for the billing module (migration 007): sale packages,
 *        PayPal payments, and the append-only wallet ledger.
 *
 * Money columns are BIGINT end to end (cents for `amount_cents`, integer
 * credits for everything else) — no floating point anywhere in this module.
 * Persistence lives in src/repositories/BillingRepository.hpp and
 * src/billing/Wallet.hpp; this file is domain-only (no SQL, no pqxx calls
 * beyond from_row()).
 */

#pragma once

#include <cstdint>
#include <optional>
#include <pqxx/pqxx>
#include <string>

#include <nlohmann/json.hpp>

#include "utils/Time.hpp"

namespace Domain {

/// `payments.status` values. String constants (not a C++ enum) because the
/// column is a plain VARCHAR and every value crosses the DB boundary as text.
namespace PaymentStatus {
inline constexpr const char* kCreated = "created";
inline constexpr const char* kApproved = "approved";
inline constexpr const char* kCaptured = "captured";
inline constexpr const char* kFailed = "failed";
inline constexpr const char* kRefunded = "refunded";
}  // namespace PaymentStatus

/// `wallet_entries.kind` values.
namespace WalletEntryKind {
inline constexpr const char* kTopup = "topup";
inline constexpr const char* kSpend = "spend";
inline constexpr const char* kAdjustment = "adjustment";
inline constexpr const char* kRefund = "refund";
}  // namespace WalletEntryKind

/**
 * @brief Admin-managed sale catalogue row. Mirrors `billing_packages`.
 */
struct Package {
    std::string id;  // UUID v4 (text)
    std::string title;
    std::int64_t amount_cents{0};
    std::int64_t credits{0};
    bool active{true};
    int sort{0};
    std::string created_at;
    std::string updated_at;

    template <typename Row>
    static Package from_row(const Row& row) {
        Package p;
        p.id = row["id"].template as<std::string>();
        p.title = row["title"].template as<std::string>();
        p.amount_cents = row["amount_cents"].template as<std::int64_t>();
        p.credits = row["credits"].template as<std::int64_t>();
        p.active = row["active"].template as<bool>();
        p.sort = row["sort"].template as<int>();
        p.created_at = Utils::Time::pg_to_iso8601(row["created_at"].template as<std::string>());
        p.updated_at = Utils::Time::pg_to_iso8601(row["updated_at"].template as<std::string>());
        return p;
    }
};

inline void to_json(nlohmann::json& j, const Package& p) {
    j = nlohmann::json{
        {"id", p.id},
        {"title", p.title},
        {"amount_cents", p.amount_cents},
        {"credits", p.credits},
        {"active", p.active},
        {"sort", p.sort},
        {"created_at", p.created_at},
        {"updated_at", p.updated_at},
    };
}

/**
 * @brief The single-row admin-editable billing rate/bounds. Mirrors
 *        `billing_settings` (migration 009). Read by
 *        BillingController::billing_limits() (user-facing top-up + package
 *        list) and written only by Api::AdminBillingController's settings
 *        endpoint — see src/repositories/BillingRepository.hpp's
 *        BillingSettingsRepository.
 */
struct BillingSettings {
    std::int64_t credits_per_unit{100};
    std::int64_t min_amount_cents{100};
    std::int64_t max_amount_cents{100000};
    std::string updated_at;

    template <typename Row>
    static BillingSettings from_row(const Row& row) {
        BillingSettings s;
        s.credits_per_unit = row["credits_per_unit"].template as<std::int64_t>();
        s.min_amount_cents = row["min_amount_cents"].template as<std::int64_t>();
        s.max_amount_cents = row["max_amount_cents"].template as<std::int64_t>();
        s.updated_at = Utils::Time::pg_to_iso8601(row["updated_at"].template as<std::string>());
        return s;
    }
};

inline void to_json(nlohmann::json& j, const BillingSettings& s) {
    j = nlohmann::json{
        {"credits_per_unit", s.credits_per_unit},
        {"min_amount_cents", s.min_amount_cents},
        {"max_amount_cents", s.max_amount_cents},
        {"updated_at", s.updated_at},
    };
}

/**
 * @brief A PayPal order/payment attempt. Mirrors `payments`. `credits_expected`
 *        and `rate_snapshot` are frozen at creation — a later rate change
 *        cannot alter an in-flight order (see Billing::credit_capture).
 */
struct Payment {
    std::string id;  // UUID v4 (text)
    std::string user_id;
    std::string provider;  // "paypal"
    std::string provider_order_id;
    std::optional<std::string> provider_capture_id;
    std::int64_t amount_cents{0};
    std::string currency;  // ISO 4217, e.g. "USD"
    std::int64_t credits_expected{0};
    std::int64_t rate_snapshot{0};  // credits per 100 cents at creation time
    std::optional<std::string> package_id;
    std::string status;  // created | approved | captured | failed | refunded
    std::optional<std::string> failure_reason;
    std::string created_at;
    std::string updated_at;

    template <typename Row>
    static Payment from_row(const Row& row) {
        Payment p;
        p.id = row["id"].template as<std::string>();
        p.user_id = row["user_id"].template as<std::string>();
        p.provider = row["provider"].template as<std::string>();
        p.provider_order_id = row["provider_order_id"].template as<std::string>();
        if (!row["provider_capture_id"].is_null())
            p.provider_capture_id = row["provider_capture_id"].template as<std::string>();
        p.amount_cents = row["amount_cents"].template as<std::int64_t>();
        p.currency = row["currency"].template as<std::string>();
        p.credits_expected = row["credits_expected"].template as<std::int64_t>();
        p.rate_snapshot = row["rate_snapshot"].template as<std::int64_t>();
        if (!row["package_id"].is_null())
            p.package_id = row["package_id"].template as<std::string>();
        p.status = row["status"].template as<std::string>();
        if (!row["failure_reason"].is_null())
            p.failure_reason = row["failure_reason"].template as<std::string>();
        p.created_at = Utils::Time::pg_to_iso8601(row["created_at"].template as<std::string>());
        p.updated_at = Utils::Time::pg_to_iso8601(row["updated_at"].template as<std::string>());
        return p;
    }
};

inline void to_json(nlohmann::json& j, const Payment& p) {
    j = nlohmann::json{
        {"id", p.id},
        {"user_id", p.user_id},
        {"provider", p.provider},
        {"provider_order_id", p.provider_order_id},
        {"provider_capture_id",
         p.provider_capture_id ? nlohmann::json(*p.provider_capture_id) : nlohmann::json(nullptr)},
        {"amount_cents", p.amount_cents},
        {"currency", p.currency},
        {"credits_expected", p.credits_expected},
        {"rate_snapshot", p.rate_snapshot},
        {"package_id", p.package_id ? nlohmann::json(*p.package_id) : nlohmann::json(nullptr)},
        {"status", p.status},
        {"failure_reason", p.failure_reason ? nlohmann::json(*p.failure_reason) : nlohmann::json(nullptr)},
        {"created_at", p.created_at},
        {"updated_at", p.updated_at},
    };
}

/**
 * @brief One row of the append-only credit ledger. Mirrors `wallet_entries`.
 *        The ONLY writer is Billing::Wallet (see src/billing/Wallet.hpp) —
 *        no repository in this codebase may update or delete a wallet_entries
 *        row; corrections are new rows (kind=adjustment / refund), never
 *        mutations of history.
 */
struct WalletEntry {
    std::string id;  // UUID v4 (text)
    std::string user_id;
    std::int64_t delta_credits{0};  // signed, never 0 (CHECK delta_credits <> 0)
    std::string kind;               // topup | spend | adjustment | refund
    std::string reference;          // payment id / service id
    std::string note;
    std::optional<std::string> created_by;  // admin, for adjustments
    std::string created_at;

    template <typename Row>
    static WalletEntry from_row(const Row& row) {
        WalletEntry e;
        e.id = row["id"].template as<std::string>();
        e.user_id = row["user_id"].template as<std::string>();
        e.delta_credits = row["delta_credits"].template as<std::int64_t>();
        e.kind = row["kind"].template as<std::string>();
        e.reference = row["reference"].template as<std::string>();
        e.note = row["note"].template as<std::string>();
        if (!row["created_by"].is_null())
            e.created_by = row["created_by"].template as<std::string>();
        e.created_at = Utils::Time::pg_to_iso8601(row["created_at"].template as<std::string>());
        return e;
    }
};

inline void to_json(nlohmann::json& j, const WalletEntry& e) {
    j = nlohmann::json{
        {"id", e.id},
        {"user_id", e.user_id},
        {"delta_credits", e.delta_credits},
        {"kind", e.kind},
        {"reference", e.reference},
        {"note", e.note},
        {"created_by", e.created_by ? nlohmann::json(*e.created_by) : nlohmann::json(nullptr)},
        {"created_at", e.created_at},
    };
}

}  // namespace Domain
