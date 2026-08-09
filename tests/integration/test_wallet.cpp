/**
 * @file test_wallet.cpp
 * @brief Integration tests for the billing module's money core:
 *        src/billing/Wallet.hpp (Billing::credit_capture / refund_capture /
 *        adjust / balance_of / history) plus src/repositories/BillingRepository.hpp
 *        (Repositories::PackageRepository / PaymentRepository).
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
    Repositories::PackageRepository packages;

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
    // arithmetic in every test trivially readable. Currency is always "USD"
    // here; the currency-mismatch test overrides it at the credit_capture call.
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

    auto result = Billing::credit_capture("ORDER-1", "CAPTURE-1", /*captured_amount_cents=*/1000, "USD");
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

    // from_primary=true must agree with the default (replica-tolerant) read.
    EXPECT_EQ(Billing::balance_of(user_id, /*from_primary=*/true), 1000);
    EXPECT_EQ(Billing::history(user_id, 10, 0, /*from_primary=*/true).size(), 1u);
}

TEST_F(WalletTest, CreditCaptureIsIdempotentOnCaptureId) {
    auto user_id = seed_user("buyer2@example.com");
    seed_payment(user_id, "ORDER-2", 500, 500);

    auto first = Billing::credit_capture("ORDER-2", "CAPTURE-2", 500, "USD");
    ASSERT_TRUE(first.credited);
    ASSERT_EQ(first.balance, 500);

    auto second = Billing::credit_capture("ORDER-2", "CAPTURE-2", 500, "USD");
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

    auto result = Billing::credit_capture("ORDER-3", "CAPTURE-3", /*captured_amount_cents=*/999, "USD");
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
    auto retry = Billing::credit_capture("ORDER-3", "CAPTURE-3", 999, "USD");
    EXPECT_FALSE(retry.credited);
    EXPECT_EQ(Billing::history(user_id, 10, 0).size(), 0u);
}

TEST_F(WalletTest, CreditCaptureRefusesCurrencyMismatch) {
    auto user_id = seed_user("buyer3b@example.com");
    auto payment = seed_payment(user_id, "ORDER-3B", 1000, 1000);  // seeded currency is "USD"

    auto result = Billing::credit_capture("ORDER-3B", "CAPTURE-3B", 1000, "EUR");
    EXPECT_FALSE(result.credited);
    EXPECT_EQ(Billing::history(user_id, 10, 0).size(), 0u);

    auto found = payments.find(payment.id);
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found->status, "failed");
    ASSERT_TRUE(found->failure_reason.has_value());
    EXPECT_NE(found->failure_reason->find("currency mismatch"), std::string::npos);
}

TEST_F(WalletTest, CreditCaptureThrowsOnUnknownOrder) {
    EXPECT_THROW(Billing::credit_capture("NO-SUCH-ORDER", "CAPTURE-X", 100, "USD"), Repositories::PaymentNotFound);
}

// Pathological: the same capture id presented for a DIFFERENT order than the
// one that already recorded it trips the UNIQUE index, not the ordinary
// idempotent branch (that only fires for a same-order retry).
TEST_F(WalletTest, CreditCaptureThrowsDuplicateCaptureIdAcrossDifferentOrders) {
    auto user_id = seed_user("buyer3c@example.com");
    seed_payment(user_id, "ORDER-DUP-A", 1000, 1000);
    seed_payment(user_id, "ORDER-DUP-B", 500, 500);

    auto first = Billing::credit_capture("ORDER-DUP-A", "CAPTURE-SHARED", 1000, "USD");
    ASSERT_TRUE(first.credited);

    EXPECT_THROW(Billing::credit_capture("ORDER-DUP-B", "CAPTURE-SHARED", 500, "USD"), Billing::DuplicateCaptureId);
    // Order B was never touched.
    auto order_b = payments.find_by_order_id("ORDER-DUP-B");
    ASSERT_TRUE(order_b.has_value());
    EXPECT_EQ(order_b->status, "created");
    EXPECT_FALSE(order_b->provider_capture_id.has_value());
}

// ── refund_capture ───────────────────────────────────────────────────────────

TEST_F(WalletTest, RefundWritesNegativeEntry) {
    auto user_id = seed_user("buyer4@example.com");
    seed_payment(user_id, "ORDER-4", 1000, 1000);
    auto captured = Billing::credit_capture("ORDER-4", "CAPTURE-4", 1000, "USD");
    ASSERT_TRUE(captured.credited);
    ASSERT_EQ(captured.balance, 1000);

    auto refunded = Billing::refund_capture("CAPTURE-4", "REFUND-4", /*refunded_amount_cents=*/1000);
    EXPECT_TRUE(refunded.credited);
    EXPECT_EQ(refunded.balance, 0);
    EXPECT_EQ(Billing::balance_of(user_id), 0);

    auto hist = Billing::history(user_id, 10, 0);
    ASSERT_EQ(hist.size(), 2u);
    EXPECT_EQ(hist[0].kind, "refund");  // newest first
    EXPECT_EQ(hist[0].delta_credits, -1000);
    EXPECT_EQ(hist[0].reference, "REFUND-4");

    auto found = payments.find_by_capture_id("CAPTURE-4");
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found->status, "refunded");

    // Idempotent: redelivering the SAME refund id is a no-op.
    auto redelivered = Billing::refund_capture("CAPTURE-4", "REFUND-4", 1000);
    EXPECT_FALSE(redelivered.credited);
    EXPECT_EQ(redelivered.balance, 0);
    EXPECT_EQ(Billing::history(user_id, 10, 0).size(), 2u);
}

TEST_F(WalletTest, RefundCaptureThrowsOnUnknownCaptureId) {
    EXPECT_THROW(Billing::refund_capture("NO-SUCH-CAPTURE", "REFUND-X", 100), Repositories::PaymentNotFound);
}

// IMPORTANT-2 fix: idempotency is keyed on the refund id, not on
// payments.status — two DISTINCT partial refunds on the same capture must
// both apply, not collapse into one.
TEST_F(WalletTest, DistinctPartialRefundsBothApply) {
    auto user_id = seed_user("buyer4b@example.com");
    seed_payment(user_id, "ORDER-4B", 1000, 1000);
    auto captured = Billing::credit_capture("ORDER-4B", "CAPTURE-4B", 1000, "USD");
    ASSERT_TRUE(captured.credited);

    auto first_partial = Billing::refund_capture("CAPTURE-4B", "REFUND-4B-1", 400);
    EXPECT_TRUE(first_partial.credited);
    EXPECT_EQ(first_partial.balance, 600);

    auto second_partial = Billing::refund_capture("CAPTURE-4B", "REFUND-4B-2", 600);
    EXPECT_TRUE(second_partial.credited);
    EXPECT_EQ(second_partial.balance, 0);

    EXPECT_EQ(Billing::balance_of(user_id), 0);
    auto hist = Billing::history(user_id, 10, 0);
    ASSERT_EQ(hist.size(), 3u);  // topup + 2 distinct refunds
    int refund_rows = 0;
    for (const auto& e : hist)
        if (e.kind == "refund")
            ++refund_rows;
    EXPECT_EQ(refund_rows, 2);

    // Redelivering the FIRST partial refund's id again is still a no-op —
    // per-id idempotency, not a payments.status guard.
    auto redelivered_first = Billing::refund_capture("CAPTURE-4B", "REFUND-4B-1", 400);
    EXPECT_FALSE(redelivered_first.credited);
    EXPECT_EQ(Billing::history(user_id, 10, 0).size(), 3u);
}

TEST_F(WalletTest, RefundCaptureRejectsInvalidAmounts) {
    auto user_id = seed_user("buyer4c@example.com");
    seed_payment(user_id, "ORDER-4C", 1000, 1000);
    Billing::credit_capture("ORDER-4C", "CAPTURE-4C", 1000, "USD");

    EXPECT_THROW(Billing::refund_capture("CAPTURE-4C", "REFUND-ZERO", 0), Billing::InvalidRefundAmount);
    EXPECT_THROW(Billing::refund_capture("CAPTURE-4C", "REFUND-NEG", -100), Billing::InvalidRefundAmount);
    EXPECT_THROW(Billing::refund_capture("CAPTURE-4C", "REFUND-OVER", 1001), Billing::InvalidRefundAmount);

    // Nothing was written by any of the three rejected calls — not even the
    // payment status.
    EXPECT_EQ(Billing::history(user_id, 10, 0).size(), 1u);  // just the topup
    EXPECT_EQ(Billing::balance_of(user_id), 1000);
    auto found = payments.find_by_capture_id("CAPTURE-4C");
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found->status, "captured");
}

// A refund must never be silently dropped even when it would drive the
// balance negative (the user already spent below the refund amount).
// "spend" itself is a later wave — simulate the spent-down state directly.
TEST_F(WalletTest, RefundBeyondRemainingBalanceMarksPaymentRefundedWithoutGoingNegative) {
    auto user_id = seed_user("buyer5@example.com");
    seed_payment(user_id, "ORDER-5", 1000, 1000);
    auto captured = Billing::credit_capture("ORDER-5", "CAPTURE-5", 1000, "USD");
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

    auto refunded = Billing::refund_capture("CAPTURE-5", "REFUND-5", 1000);
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

    // Redelivering the same (still-unfulfillable) refund id is still safe:
    // no duplicate row, same outcome.
    auto redelivered = Billing::refund_capture("CAPTURE-5", "REFUND-5", 1000);
    EXPECT_TRUE(redelivered.credited);
    EXPECT_EQ(redelivered.balance, 100);
    EXPECT_EQ(Billing::history(user_id, 10, 0).size(), 2u);
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

TEST_F(WalletTest, AdjustRejectsZeroDelta) {
    auto user_id = seed_user("buyer6b@example.com");
    auto admin_id = seed_user("admin1b@example.com");

    EXPECT_THROW(Billing::adjust(user_id, 0, "noop", admin_id), Billing::ZeroAdjustment);
    EXPECT_EQ(Billing::history(user_id, 10, 0).size(), 0u);
    EXPECT_EQ(Billing::balance_of(user_id), 0);
}

TEST_F(WalletTest, AdjustThrowsOnUnknownUser) {
    auto admin_id = seed_user("admin1c@example.com");
    // Syntactically valid UUID, but no such row.
    EXPECT_THROW(Billing::adjust("00000000-0000-0000-0000-000000000000", 10, "note", admin_id), Billing::UnknownUser);
}

TEST_F(WalletTest, AdjustThrowsOnMalformedUserId) {
    auto admin_id = seed_user("admin1d@example.com");
    EXPECT_THROW(Billing::adjust("not-a-uuid", 10, "note", admin_id), Billing::MalformedUserId);
}

// ── the money invariant ──────────────────────────────────────────────────────

TEST_F(WalletTest, LedgerSumEqualsCachedBalanceAfterMixedTraffic) {
    auto user_a = seed_user("mix-a@example.com");
    auto user_b = seed_user("mix-b@example.com");
    auto admin_id = seed_user("admin2@example.com");

    seed_payment(user_a, "ORDER-A1", 1000, 1000);
    seed_payment(user_a, "ORDER-A2", 500, 500);
    seed_payment(user_b, "ORDER-B1", 2000, 2000);

    Billing::credit_capture("ORDER-A1", "CAP-A1", 1000, "USD");
    Billing::credit_capture("ORDER-A2", "CAP-A2", 500, "USD");
    Billing::credit_capture("ORDER-B1", "CAP-B1", 2000, "USD");
    Billing::refund_capture("CAP-A1", "REFUND-MIX-1", 1000);
    Billing::adjust(user_a, 50, "bonus", admin_id);
    Billing::adjust(user_b, -100, "correction", admin_id);
    // A duplicate capture attempt, a failed one, and a redelivered refund id
    // — none of these should perturb the invariant.
    Billing::credit_capture("ORDER-A2", "CAP-A2", 500, "USD");
    Billing::refund_capture("CAP-A1", "REFUND-MIX-1", 1000);
    seed_payment(user_a, "ORDER-A3", 300, 300);
    Billing::credit_capture("ORDER-A3", "CAP-A3", 999, "USD");

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
    auto result = Billing::credit_capture("ORDER-DEL-1", "CAPTURE-DEL-1", 1000, "USD");
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

// ── Repositories::PackageRepository ─────────────────────────────────────────

TEST_F(WalletTest, PackageRepositoryCreateAndFindRoundtrip) {
    auto created = packages.create("Starter Pack", /*amount_cents=*/500, /*credits=*/500, /*active=*/true, /*sort=*/1);
    EXPECT_EQ(created.title, "Starter Pack");
    EXPECT_EQ(created.amount_cents, 500);
    EXPECT_EQ(created.credits, 500);
    EXPECT_TRUE(created.active);
    EXPECT_EQ(created.sort, 1);

    auto found = packages.find(created.id);
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found->title, "Starter Pack");
}

TEST_F(WalletTest, PackageRepositoryListActiveExcludesInactiveAndOrdersBySort) {
    packages.create("Second", 1000, 1000, /*active=*/true, /*sort=*/2);
    packages.create("First", 500, 500, /*active=*/true, /*sort=*/1);
    packages.create("Hidden", 2000, 2000, /*active=*/false, /*sort=*/0);

    auto active = packages.list_active();
    ASSERT_EQ(active.size(), 2u);
    EXPECT_EQ(active[0].title, "First");   // sort=1 before sort=2
    EXPECT_EQ(active[1].title, "Second");
    for (const auto& p : active)
        EXPECT_NE(p.title, "Hidden");
}

TEST_F(WalletTest, PackageRepositoryUpdateKeepsOmittedFields) {
    auto created = packages.create("Original", 500, 500, true, 1);

    auto updated = packages.update(created.id,
                                   /*title=*/std::string("Renamed"),
                                   /*amount_cents=*/std::nullopt,
                                   /*credits=*/std::nullopt,
                                   /*active=*/false,
                                   /*sort=*/std::nullopt);
    EXPECT_EQ(updated.title, "Renamed");
    EXPECT_EQ(updated.amount_cents, 500);  // unchanged
    EXPECT_EQ(updated.credits, 500);       // unchanged
    EXPECT_FALSE(updated.active);
    EXPECT_EQ(updated.sort, 1);  // unchanged

    EXPECT_THROW(packages.update("00000000-0000-0000-0000-000000000000",
                                 std::string("x"),
                                 std::nullopt,
                                 std::nullopt,
                                 std::nullopt,
                                 std::nullopt),
                Repositories::PackageNotFound);
}

TEST_F(WalletTest, PackageRepositoryRemoveDeletesRow) {
    auto created = packages.create("Removable", 500, 500, true, 1);
    packages.remove(created.id);
    EXPECT_FALSE(packages.find(created.id).has_value());
    EXPECT_THROW(packages.remove(created.id), Repositories::PackageNotFound);
}

// ── Repositories::PaymentRepository::mark_approved ──────────────────────────

TEST_F(WalletTest, MarkApprovedTransitionsFromCreated) {
    auto user_id = seed_user("approver@example.com");
    seed_payment(user_id, "ORDER-APPROVE-1", 1000, 1000);

    EXPECT_TRUE(payments.mark_approved("ORDER-APPROVE-1"));
    auto found = payments.find_by_order_id("ORDER-APPROVE-1");
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found->status, "approved");
}

TEST_F(WalletTest, MarkApprovedIsNoOpAfterCapture) {
    auto user_id = seed_user("approver2@example.com");
    seed_payment(user_id, "ORDER-APPROVE-2", 1000, 1000);
    Billing::credit_capture("ORDER-APPROVE-2", "CAPTURE-APPROVE-2", 1000, "USD");

    // A duplicate/out-of-order APPROVED webhook after capture must not throw
    // (that would make PayPal redeliver it forever) — just report false.
    EXPECT_FALSE(payments.mark_approved("ORDER-APPROVE-2"));
    auto found = payments.find_by_order_id("ORDER-APPROVE-2");
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found->status, "captured");  // unchanged
}

TEST_F(WalletTest, MarkApprovedThrowsOnUnknownOrder) {
    EXPECT_THROW(payments.mark_approved("NO-SUCH-ORDER"), Repositories::PaymentNotFound);
}

}  // namespace
