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
            txn.exec(
                "TRUNCATE TABLE wallet_entries, wallet_balances, billing_refunds, payments, billing_packages CASCADE");
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
        auto created = users.create(
            email, std::string("$argon2id$placeholder"), std::nullopt, std::nullopt, role->id, /*confirmed=*/true);
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

    // billing_refunds has no repository (it's Wallet.hpp's own internal
    // idempotency marker) — read it directly for assertions.
    std::optional<std::string> refund_outcome(const std::string& provider_refund_id) {
        return Database::get().execute_read([&](auto& txn) -> std::optional<std::string> {
            auto r = txn.exec_params("SELECT outcome FROM billing_refunds WHERE provider_refund_id = $1",
                                     provider_refund_id);
            if (r.empty())
                return std::nullopt;
            return r[0]["outcome"].template as<std::string>();
        });
    }

    long refund_row_count(const std::string& payment_id) {
        return Database::get().execute_read([&](auto& txn) {
            auto r = txn.exec_params("SELECT COUNT(*) FROM billing_refunds WHERE payment_id = $1", payment_id);
            return r[0][0].template as<long>();
        });
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

    // NEW-B: the cumulative total (400 so far) is still short of the full
    // 1000, so the payment stays 'captured' after just the first partial.
    auto after_first = payments.find_by_capture_id("CAPTURE-4B");
    ASSERT_TRUE(after_first.has_value());
    EXPECT_EQ(after_first->status, "captured");

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

    // NEW-B: status flips on the CUMULATIVE total reaching the full amount —
    // 400 + 600 == 1000, so once the second (distinct) partial lands, the
    // payment is 'refunded' even though neither individual call's own
    // amount equaled the full 1000.
    auto found = payments.find_by_capture_id("CAPTURE-4B");
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found->status, "refunded");

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

    // The attempt is durably recorded (billing_refunds), not the ledger.
    EXPECT_EQ(refund_outcome("REFUND-5"), "skipped_insufficient");

    // Redelivering the same refund id is now a genuine idempotent no-op —
    // the durable billing_refunds row is the marker, checked first.
    auto redelivered = Billing::refund_capture("CAPTURE-5", "REFUND-5", 1000);
    EXPECT_FALSE(redelivered.credited);
    EXPECT_EQ(redelivered.balance, 100);
    EXPECT_EQ(Billing::history(user_id, 10, 0).size(), 2u);
}

// NEW-1: a refund small enough that it converts to 0 credits must never trip
// wallet_entries' CHECK(delta_credits <> 0) — it used to escape as an
// uncaught 500 that PayPal would redeliver forever. Also exercises NEW-4:
// since this is a PARTIAL refund (50 cents of a 1000-cent payment), the
// payment status must stay 'captured', not flip to 'refunded'.
TEST_F(WalletTest, RefundSubUnitAmountConvertsToZeroCreditsIsRecordedNotCrashed) {
    auto user_id = seed_user("buyer5b@example.com");
    // 10 credits for the whole 1000-cent payment — 50 cents prorates to
    // (10 * 50) / 1000 = 0 credits (floors below 1 unit).
    seed_payment(user_id, "ORDER-5B", /*amount_cents=*/1000, /*credits_expected=*/10, /*rate_snapshot=*/1);
    auto captured = Billing::credit_capture("ORDER-5B", "CAPTURE-5B", 1000, "USD");
    ASSERT_TRUE(captured.credited);
    ASSERT_EQ(captured.balance, 10);

    auto result = Billing::refund_capture("CAPTURE-5B", "REFUND-SUBUNIT", /*refunded_amount_cents=*/50);
    EXPECT_TRUE(result.credited);
    EXPECT_EQ(result.balance, 10);  // untouched
    EXPECT_EQ(Billing::balance_of(user_id), 10);

    // No refund ledger row — just the original topup.
    auto hist = Billing::history(user_id, 10, 0);
    ASSERT_EQ(hist.size(), 1u);
    EXPECT_EQ(hist[0].kind, "topup");

    EXPECT_EQ(refund_outcome("REFUND-SUBUNIT"), "skipped_zero_credits");

    // Partial (50 != 1000) — status stays captured.
    auto found = payments.find_by_capture_id("CAPTURE-5B");
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found->status, "captured");
}

// CRITICAL fix-round-1 regression test: refund_capture must prorate off
// credits_expected, NOT rate_snapshot. A bonus package sells 500 credits for
// only 400 cents (rate_snapshot frozen at the unrelated generic per-unit
// rate, 100 — i.e. "1 credit per cent" — which would derive only 400 credits
// from 400 cents if the old formula were still used). A FULL refund must
// deduct exactly the 500 credits that were actually granted, not 400 — proof
// that credit_capture and refund_capture agree on what a "full refund" means
// regardless of how the payment was priced.
TEST_F(WalletTest, RefundOfBonusPackagePurchaseDeductsExactCreditsGranted) {
    auto user_id = seed_user("buyer5g@example.com");
    seed_payment(user_id,
                 "ORDER-5G",
                 /*amount_cents=*/400,
                 /*credits_expected=*/500,
                 /*rate_snapshot=*/100);  // rate disagrees with credits_expected on purpose
    auto captured = Billing::credit_capture("ORDER-5G", "CAPTURE-5G", 400, "USD");
    ASSERT_TRUE(captured.credited);
    ASSERT_EQ(captured.balance, 500);

    auto refunded = Billing::refund_capture("CAPTURE-5G", "REFUND-5G", /*refunded_amount_cents=*/400);
    EXPECT_TRUE(refunded.credited);
    // A rate_snapshot-based conversion would have computed 400*100/100=400,
    // leaving the user with 100 credits after a "full" refund. The correct,
    // credits_expected-prorated answer deducts all 500.
    EXPECT_EQ(refunded.balance, 0);
    EXPECT_EQ(Billing::balance_of(user_id), 0);
    EXPECT_EQ(refund_outcome("REFUND-5G"), "applied");

    auto found = payments.find_by_capture_id("CAPTURE-5G");
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found->status, "refunded");
}

// NEW-4: a normal (nonzero-credit, sufficient-balance) PARTIAL refund must
// not flip payments.status to 'refunded' — only a single call whose amount
// equals the full payment amount does that.
TEST_F(WalletTest, PartialRefundLeavesPaymentCaptured) {
    auto user_id = seed_user("buyer5c@example.com");
    seed_payment(user_id, "ORDER-5C", 1000, 1000);
    auto captured = Billing::credit_capture("ORDER-5C", "CAPTURE-5C", 1000, "USD");
    ASSERT_TRUE(captured.credited);

    auto result = Billing::refund_capture("CAPTURE-5C", "REFUND-5C", /*refunded_amount_cents=*/300);
    EXPECT_TRUE(result.credited);
    EXPECT_EQ(result.balance, 700);
    EXPECT_EQ(refund_outcome("REFUND-5C"), "applied");

    auto found = payments.find_by_capture_id("CAPTURE-5C");
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found->status, "captured");  // NOT refunded — this was partial
}

// NEW-4: a refund against a payment that failed capture (amount/currency
// mismatch) must not overwrite 'failed' with 'refunded', even for an amount
// that matches the payment's amount_cents (the "full refund" condition on
// its own is not enough — the payment must also currently be 'captured').
TEST_F(WalletTest, RefundAgainstFailedPaymentDoesNotOverwriteStatus) {
    auto user_id = seed_user("buyer5d@example.com");
    seed_payment(user_id, "ORDER-5D", 1000, 1000);
    // Mismatched capture -> status='failed', but provider_capture_id is
    // still recorded (credit_capture sets it before validating the amount).
    auto mismatch = Billing::credit_capture("ORDER-5D", "CAPTURE-5D", /*captured_amount_cents=*/999, "USD");
    ASSERT_FALSE(mismatch.credited);
    auto pre = payments.find_by_capture_id("CAPTURE-5D");
    ASSERT_TRUE(pre.has_value());
    ASSERT_EQ(pre->status, "failed");

    auto result = Billing::refund_capture("CAPTURE-5D", "REFUND-5D", /*refunded_amount_cents=*/1000);
    EXPECT_TRUE(result.credited);  // recorded — nothing to refund since nothing was credited
    EXPECT_EQ(refund_outcome("REFUND-5D"), "skipped_insufficient");  // balance is 0

    auto found = payments.find_by_capture_id("CAPTURE-5D");
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found->status, "failed");  // untouched
}

// NEW-2/NEW-3: once a refund id is durably recorded as skipped
// (insufficient balance), a redelivery of that SAME id must stay a no-op
// forever — even if the wallet balance later recovers enough that the
// refund would now succeed. A delayed debit would double-process money the
// caller already considers refunded once.
TEST_F(WalletTest, DuplicateRefundIdAfterInsufficiencySkipNeverAppliesLater) {
    auto user_id = seed_user("buyer5e@example.com");
    seed_payment(user_id, "ORDER-5E", 1000, 1000);
    auto captured = Billing::credit_capture("ORDER-5E", "CAPTURE-5E", 1000, "USD");
    ASSERT_TRUE(captured.credited);

    // Spend down so the (full) refund can't apply.
    Database::get().execute_write([&](auto& txn) {
        txn.exec_params(
            "INSERT INTO wallet_entries (user_id, delta_credits, kind, reference) VALUES ($1, -950, 'spend', 'sim')",
            user_id);
        txn.exec_params("UPDATE wallet_balances SET credits = credits - 950 WHERE user_id = $1", user_id);
        return 0;
    });
    ASSERT_EQ(Billing::balance_of(user_id), 50);

    auto first = Billing::refund_capture("CAPTURE-5E", "REFUND-RECOVER", 1000);
    EXPECT_TRUE(first.credited);
    EXPECT_EQ(first.balance, 50);
    EXPECT_EQ(refund_outcome("REFUND-RECOVER"), "skipped_insufficient");

    // "Recover" the balance well past what the refund would have needed.
    Billing::adjust(user_id, 5000, "top up for the test", seed_user("recover-admin@example.com"));
    ASSERT_EQ(Billing::balance_of(user_id), 5050);

    // Redelivering the SAME refund id must still be a no-op — no delayed
    // debit now that the balance could technically afford it.
    auto redelivered = Billing::refund_capture("CAPTURE-5E", "REFUND-RECOVER", 1000);
    EXPECT_FALSE(redelivered.credited);
    EXPECT_EQ(Billing::balance_of(user_id), 5050);     // untouched by the redelivery
    EXPECT_EQ(refund_row_count(first.payment_id), 1);  // still exactly one billing_refunds row
}

// NEW-2/NEW-3: the running total of every refund recorded against a payment
// must never exceed what was actually paid, even across separate calls that
// each individually look valid.
TEST_F(WalletTest, AggregateOverRefundIsRefused) {
    auto user_id = seed_user("buyer5f@example.com");
    seed_payment(user_id, "ORDER-5F", 1000, 1000);
    auto captured = Billing::credit_capture("ORDER-5F", "CAPTURE-5F", 1000, "USD");
    ASSERT_TRUE(captured.credited);

    auto first = Billing::refund_capture("CAPTURE-5F", "REFUND-AGG-1", 600);
    ASSERT_TRUE(first.credited);
    ASSERT_EQ(first.balance, 400);

    // 600 (already refunded) + 500 (this call) = 1100 > 1000 — individually
    // 500 <= amount_cents, so only the AGGREGATE check catches this.
    EXPECT_THROW(Billing::refund_capture("CAPTURE-5F", "REFUND-AGG-2", 500), Billing::InvalidRefundAmount);

    // The rejected call wrote nothing at all.
    EXPECT_EQ(Billing::balance_of(user_id), 400);
    EXPECT_EQ(refund_row_count(first.payment_id), 1);
    EXPECT_FALSE(refund_outcome("REFUND-AGG-2").has_value());
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

// Minor fix: user_id and admin_id are both FKs on the same insert — a bad
// ADMIN id must be reported as such, not misattributed to user_id.
TEST_F(WalletTest, AdjustThrowsOnUnknownAdmin) {
    auto user_id = seed_user("buyer6c@example.com");
    EXPECT_THROW(Billing::adjust(user_id, 10, "note", "00000000-0000-0000-0000-000000000000"), Billing::UnknownAdmin);
    EXPECT_EQ(Billing::history(user_id, 10, 0).size(), 0u);
}

TEST_F(WalletTest, AdjustThrowsOnMalformedAdminId) {
    auto user_id = seed_user("buyer6d@example.com");
    EXPECT_THROW(Billing::adjust(user_id, 10, "note", "not-a-uuid"), Billing::MalformedAdminId);
    EXPECT_EQ(Billing::history(user_id, 10, 0).size(), 0u);
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

// Structural regression test for the lost-update fix in Wallet.hpp:
// `SELECT ... FOR UPDATE` cannot lock a `wallet_balances` row that doesn't
// exist yet, so a brand-new user's first pair of concurrent balance-changing
// writes used to both read `current = 0` unlocked, and the second writer's
// upsert would silently clobber the first's result — the cache disagreeing
// with SUM(wallet_entries.delta_credits) even though the ledger itself
// stayed correct (see the Wallet.hpp file comment for the full diagnosis).
// A genuine multi-threaded race isn't this codebase's test convention (see
// LedgerSumEqualsCachedBalanceAfterMixedTraffic above, which is sequential
// too) and isn't required here — the fix is structural (materialize the row
// before locking it, then a plain UPDATE instead of an upsert), so what
// matters is exercising that exact path — starting from NO wallet_balances
// row at all — and asserting the invariant holds afterward.
TEST_F(WalletTest, LedgerSumEqualsCachedBalanceForNewUserAfterCreditThenAdjust) {
    auto user_id = seed_user("new-user-race@example.com");
    auto admin_id = seed_user("admin-race@example.com");

    // Confirm there really is no wallet_balances row yet — the exact
    // condition that used to make the first `SELECT ... FOR UPDATE` lock
    // nothing.
    auto has_row = Database::get().execute_read([&](auto& txn) {
        auto r = txn.exec_params("SELECT 1 FROM wallet_balances WHERE user_id = $1", user_id);
        return !r.empty();
    });
    ASSERT_FALSE(has_row);

    seed_payment(user_id, "ORDER-NEW-1", 400, 400);
    auto credit_result = Billing::credit_capture("ORDER-NEW-1", "CAP-NEW-1", 400, "USD");
    ASSERT_TRUE(credit_result.credited);
    EXPECT_EQ(credit_result.balance, 400);

    auto adjust_result = Billing::adjust(user_id, 75, "welcome bonus", admin_id);
    ASSERT_TRUE(adjust_result.credited);
    EXPECT_EQ(adjust_result.balance, 475);

    auto hist = Billing::history(user_id, 10, 0);
    std::int64_t sum = 0;
    for (const auto& e : hist)
        sum += e.delta_credits;
    EXPECT_EQ(sum, Billing::balance_of(user_id));
    EXPECT_EQ(Billing::balance_of(user_id), 475);
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
    EXPECT_EQ(active[0].title, "First");  // sort=1 before sort=2
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
