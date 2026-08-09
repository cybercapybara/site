/**
 * @file test_wallet.cpp
 * @brief Integration tests for the billing module's money core:
 *        src/billing/Wallet.hpp (Billing::credit_capture / refund_capture /
 *        adjust / balance_of / history) plus src/repositories/BillingRepository.hpp.
 *
 * Needs the billing migration (007) — applied unconditionally regardless of
 * the billing.enabled flag, same pattern as test_post_repository.cpp for the
 * content module. Also covers the Task-1-review follow-up: UserRepository::
 * remove() must map the new ON DELETE RESTRICT foreign keys (payments,
 * wallet_entries, wallet_balances → users) to a typed 409 instead of a bare
 * pqxx::sql_error / 500.
 */

#include <gtest/gtest.h>

#include "billing/Wallet.hpp"
#include "domain/Billing.hpp"
#include "repositories/BillingRepository.hpp"
#include "repositories/RoleRepository.hpp"
#include "repositories/UserRepository.hpp"
#include "test_helpers.hpp"

using json = nlohmann::json;

namespace {

class WalletTest : public TestHelpers::CoreBackedTest {
protected:
    Repositories::RoleRepository roles;
    Repositories::UserRepository users;
    Repositories::PaymentRepository payments;

    std::string config_file_name() const override { return "wallet_test_config.json"; }

    void config_overrides(json& cfg) override {
        cfg["database"]["migrations_enabled"] = true;
        cfg["database"]["migrations_dir"] = "migrations";
        cfg["billing"]["enabled"] = true;
    }

    void SetUp() override {
        TestHelpers::CoreBackedTest::SetUp();
        if (::testing::Test::IsSkipped())
            return;
        Database::get().execute_write([](auto& txn) {
            txn.exec("TRUNCATE TABLE wallet_entries, wallet_balances, payments, billing_packages CASCADE");
            txn.exec("TRUNCATE TABLE users CASCADE");
            txn.exec("DELETE FROM roles WHERE name NOT IN ('User', 'Administrator')");
            return 0;
        });
    }

    std::string seed_user(const std::string& email) {
        auto role = roles.find_by_name("User");
        if (!role) {
            ADD_FAILURE() << "role User missing — seed migration?";
            throw std::runtime_error("seed role missing: User");
        }
        auto created =
            users.create(email, std::string("$argon2id$placeholder"), std::nullopt, std::nullopt, role->id, /*confirmed=*/true);
        return created.id;
    }

    // Default rate: 100 credits per 100 cents (1 credit per cent) — keeps the
    // arithmetic in every test trivially readable.
    Domain::Payment seed_payment(const std::string& user_id,
                                 const std::string& order_id,
                                 std::int64_t amount_cents = 1000,
                                 std::int64_t credits_expected = 1000,
                                 std::int64_t rate_snapshot = 100) {
        return payments.create(user_id, order_id, amount_cents, "USD", credits_expected, rate_snapshot, std::nullopt);
    }
};

// ── credit_capture ──────────────────────────────────────────────────────────

TEST_F(WalletTest, CreditCaptureCreditsOnceAndUpdatesBalance) {
    auto user_id = seed_user("buyer1@example.com");
    seed_payment(user_id, "ORDER-1", /*amount_cents=*/1000, /*credits_expected=*/1000);

    auto result = Billing::credit_capture("ORDER-1", "CAPTURE-1", /*captured_amount_cents=*/1000);
    EXPECT_TRUE(result.credited);
    EXPECT_EQ(result.balance, 1000);
    EXPECT_FALSE(result.payment_id.empty());

    EXPECT_EQ(Billing::balance_of(user_id), 1000);

    auto hist = Billing::history(user_id, 10, 0);
    ASSERT_EQ(hist.size(), 1u);
    EXPECT_EQ(hist[0].delta_credits, 1000);
    EXPECT_EQ(hist[0].kind, "topup");
    EXPECT_EQ(hist[0].reference, result.payment_id);

    auto found = payments.find(result.payment_id);
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found->status, "captured");
    ASSERT_TRUE(found->provider_capture_id.has_value());
    EXPECT_EQ(*found->provider_capture_id, "CAPTURE-1");
}

TEST_F(WalletTest, CreditCaptureIsIdempotentOnCaptureId) {
    auto user_id = seed_user("buyer2@example.com");
    seed_payment(user_id, "ORDER-2", 500, 500);

    auto first = Billing::credit_capture("ORDER-2", "CAPTURE-2", 500);
    ASSERT_TRUE(first.credited);
    ASSERT_EQ(first.balance, 500);

    auto second = Billing::credit_capture("ORDER-2", "CAPTURE-2", 500);
    EXPECT_FALSE(second.credited);
    EXPECT_EQ(second.balance, 500);
    EXPECT_EQ(second.payment_id, first.payment_id);

    // Exactly one ledger row — the second call touched nothing.
    EXPECT_EQ(Billing::history(user_id, 10, 0).size(), 1u);
    EXPECT_EQ(Billing::balance_of(user_id), 500);
}

TEST_F(WalletTest, CreditCaptureRefusesAmountMismatch) {
    auto user_id = seed_user("buyer3@example.com");
    auto payment = seed_payment(user_id, "ORDER-3", 1000, 1000);

    auto result = Billing::credit_capture("ORDER-3", "CAPTURE-3", /*captured_amount_cents=*/999);
    EXPECT_FALSE(result.credited);
    EXPECT_EQ(result.balance, 0);

    // No ledger row at all.
    EXPECT_EQ(Billing::history(user_id, 10, 0).size(), 0u);
    EXPECT_EQ(Billing::balance_of(user_id), 0);

    auto found = payments.find(payment.id);
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found->status, "failed");
    ASSERT_TRUE(found->failure_reason.has_value());
    EXPECT_NE(found->failure_reason->find("mismatch"), std::string::npos);

    // The capture id was still recorded, so a retried mismatch never reopens
    // this order for crediting.
    auto retry = Billing::credit_capture("ORDER-3", "CAPTURE-3", 999);
    EXPECT_FALSE(retry.credited);
    EXPECT_EQ(Billing::history(user_id, 10, 0).size(), 0u);
}

TEST_F(WalletTest, CreditCaptureThrowsOnUnknownOrder) {
    EXPECT_THROW(Billing::credit_capture("NO-SUCH-ORDER", "CAPTURE-X", 100), Repositories::PaymentNotFound);
}

// ── refund_capture ───────────────────────────────────────────────────────────

TEST_F(WalletTest, RefundWritesNegativeEntry) {
    auto user_id = seed_user("buyer4@example.com");
    seed_payment(user_id, "ORDER-4", 1000, 1000);
    auto captured = Billing::credit_capture("ORDER-4", "CAPTURE-4", 1000);
    ASSERT_TRUE(captured.credited);
    ASSERT_EQ(captured.balance, 1000);

    auto refunded = Billing::refund_capture("CAPTURE-4", /*refunded_amount_cents=*/1000);
    EXPECT_TRUE(refunded.credited);
    EXPECT_EQ(refunded.balance, 0);
    EXPECT_EQ(Billing::balance_of(user_id), 0);

    auto hist = Billing::history(user_id, 10, 0);
    ASSERT_EQ(hist.size(), 2u);
    EXPECT_EQ(hist[0].kind, "refund");  // newest first
    EXPECT_EQ(hist[0].delta_credits, -1000);

    auto found = payments.find_by_capture_id("CAPTURE-4");
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found->status, "refunded");

    // Idempotent: a second refund of the same capture is a no-op.
    auto second = Billing::refund_capture("CAPTURE-4", 1000);
    EXPECT_FALSE(second.credited);
    EXPECT_EQ(second.balance, 0);
    EXPECT_EQ(Billing::history(user_id, 10, 0).size(), 2u);
}

TEST_F(WalletTest, RefundCaptureThrowsOnUnknownCaptureId) {
    EXPECT_THROW(Billing::refund_capture("NO-SUCH-CAPTURE", 100), Repositories::PaymentNotFound);
}

// A refund must never be silently dropped even when the wallet_balances
// CHECK(credits >= 0) would reject the ledger write (the user already spent
// below the refund amount). "spend" itself is a later wave — simulate the
// spent-down state directly to exercise the invariant.
TEST_F(WalletTest, RefundBeyondRemainingBalanceMarksPaymentRefundedWithoutGoingNegative) {
    auto user_id = seed_user("buyer5@example.com");
    seed_payment(user_id, "ORDER-5", 1000, 1000);
    auto captured = Billing::credit_capture("ORDER-5", "CAPTURE-5", 1000);
    ASSERT_TRUE(captured.credited);
    ASSERT_EQ(captured.balance, 1000);

    Database::get().execute_write([&](auto& txn) {
        txn.exec_params(
            "INSERT INTO wallet_entries (user_id, delta_credits, kind, reference) VALUES ($1, -900, 'spend', 'sim')",
            user_id);
        txn.exec_params("UPDATE wallet_balances SET credits = credits - 900 WHERE user_id = $1", user_id);
        return 0;
    });
    ASSERT_EQ(Billing::balance_of(user_id), 100);

    auto refunded = Billing::refund_capture("CAPTURE-5", 1000);
    EXPECT_TRUE(refunded.credited);
    // The 1000-credit refund CANNOT apply (would drive balance to -900) — the
    // wallet is left exactly where the simulated spend left it.
    EXPECT_EQ(refunded.balance, 100);
    EXPECT_EQ(Billing::balance_of(user_id), 100);

    auto found = payments.find_by_capture_id("CAPTURE-5");
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found->status, "refunded");

    // No refund row was written — only the topup and the simulated spend exist.
    auto hist = Billing::history(user_id, 10, 0);
    ASSERT_EQ(hist.size(), 2u);
    for (const auto& e : hist)
        EXPECT_NE(e.kind, "refund");
}

// ── adjust ───────────────────────────────────────────────────────────────────

TEST_F(WalletTest, AdjustWritesAuditedEntryAndMovesBalance) {
    auto user_id = seed_user("buyer6@example.com");
    auto admin_id = seed_user("admin1@example.com");

    auto result = Billing::adjust(user_id, 250, "goodwill credit", admin_id);
    EXPECT_TRUE(result.credited);
    EXPECT_EQ(result.balance, 250);

    auto hist = Billing::history(user_id, 10, 0);
    ASSERT_EQ(hist.size(), 1u);
    EXPECT_EQ(hist[0].kind, "adjustment");
    EXPECT_EQ(hist[0].delta_credits, 250);
    EXPECT_EQ(hist[0].note, "goodwill credit");
    ASSERT_TRUE(hist[0].created_by.has_value());
    EXPECT_EQ(*hist[0].created_by, admin_id);

    // A negative delta that would drive the balance below zero is refused
    // outright — nothing is applied (unlike a refund, no real money has
    // already moved for a manual adjustment).
    EXPECT_THROW(Billing::adjust(user_id, -1000, "oops", admin_id), Billing::InsufficientBalance);
    EXPECT_EQ(Billing::balance_of(user_id), 250);
    EXPECT_EQ(Billing::history(user_id, 10, 0).size(), 1u);
}

// ── the money invariant ──────────────────────────────────────────────────────

TEST_F(WalletTest, LedgerSumEqualsCachedBalanceAfterMixedTraffic) {
    auto user_a = seed_user("mix-a@example.com");
    auto user_b = seed_user("mix-b@example.com");
    auto admin_id = seed_user("admin2@example.com");

    seed_payment(user_a, "ORDER-A1", 1000, 1000);
    seed_payment(user_a, "ORDER-A2", 500, 500);
    seed_payment(user_b, "ORDER-B1", 2000, 2000);

    Billing::credit_capture("ORDER-A1", "CAP-A1", 1000);
    Billing::credit_capture("ORDER-A2", "CAP-A2", 500);
    Billing::credit_capture("ORDER-B1", "CAP-B1", 2000);
    Billing::refund_capture("CAP-A1", 1000);
    Billing::adjust(user_a, 50, "bonus", admin_id);
    Billing::adjust(user_b, -100, "correction", admin_id);
    // A duplicate capture attempt and a failed one — neither should perturb
    // the invariant.
    Billing::credit_capture("ORDER-A2", "CAP-A2", 500);
    seed_payment(user_a, "ORDER-A3", 300, 300);
    Billing::credit_capture("ORDER-A3", "CAP-A3", 999);

    for (const auto& user_id : {user_a, user_b}) {
        auto hist = Billing::history(user_id, 100, 0);
        std::int64_t sum = 0;
        for (const auto& e : hist)
            sum += e.delta_credits;
        EXPECT_EQ(sum, Billing::balance_of(user_id)) << "user " << user_id;
    }
}

// ── UserRepository::remove() vs. billing history (Task 1 review follow-up) ──

TEST_F(WalletTest, DeletingUserWithWalletHistoryIsBlocked) {
    auto user_id = seed_user("has-history@example.com");
    seed_payment(user_id, "ORDER-DEL-1", 1000, 1000);
    auto result = Billing::credit_capture("ORDER-DEL-1", "CAPTURE-DEL-1", 1000);
    ASSERT_TRUE(result.credited);

    EXPECT_THROW(users.remove(user_id), Repositories::UserHasBillingHistory);
    // Not deleted — the typed error means the transaction rolled back cleanly.
    EXPECT_TRUE(users.find(user_id).has_value());
}

TEST_F(WalletTest, DeletingUserWithoutBillingHistorySucceeds) {
    auto user_id = seed_user("no-history@example.com");
    EXPECT_NO_THROW(users.remove(user_id));
    EXPECT_FALSE(users.find(user_id).has_value());
}

}  // namespace
