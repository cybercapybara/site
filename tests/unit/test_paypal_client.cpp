/**
 * @file test_paypal_client.cpp
 * @brief PayPalClient's pure surfaces: base URL selection, capture-response
 *        parsing, the money-critical decimal<->cents parser, and
 *        case-insensitive header extraction. NO network — canned JSON only.
 *        Owner policy: GitHub CI is the only thing that builds/runs this;
 *        this file must never dial out to PayPal.
 */

#include <cstdint>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>

#include <gtest/gtest.h>

#include "billing/PayPalClient.hpp"
#include "test_helpers.hpp"
#include "utils/Config.hpp"

namespace {

using Billing::PayPalCapture;
using Billing::PayPalClient;
namespace detail = Billing::detail;

// ---------------------------------------------------------------------------
// Brief-named cases
// ---------------------------------------------------------------------------

TEST(PayPalClient, BaseUrlSelectsSandboxAndLive) {
    EXPECT_EQ(PayPalClient::base_url("sandbox"), "https://api-m.sandbox.paypal.com");
    EXPECT_EQ(PayPalClient::base_url("live"), "https://api-m.paypal.com");
    // Unknown/typo'd environment fails SAFE to sandbox — never accidentally
    // live because of a config mistake.
    EXPECT_EQ(PayPalClient::base_url("production"), "https://api-m.sandbox.paypal.com");
    EXPECT_EQ(PayPalClient::base_url(""), "https://api-m.sandbox.paypal.com");
}

TEST(PayPalClient, ParseCaptureExtractsIdAndIntegerCents) {
    const std::string body = R"JSON({
        "id": "5O190127TN364715T",
        "status": "COMPLETED",
        "purchase_units": [
            {
                "reference_id": "order-123",
                "payments": {
                    "captures": [
                        {
                            "id": "3C679366HH908993F",
                            "status": "COMPLETED",
                            "amount": { "currency_code": "USD", "value": "12.34" }
                        }
                    ]
                }
            }
        ]
    })JSON";

    const PayPalCapture cap = PayPalClient::parse_capture_response(body);
    EXPECT_EQ(cap.capture_id, "3C679366HH908993F");
    EXPECT_EQ(cap.currency, "USD");
    // No float rounding drift: "12.34" must land on exactly 1234, not
    // 1233/1235 the way a naive `llround(stod(...) * 100)` could for some
    // decimal values.
    EXPECT_EQ(cap.amount_cents, 1234);
    // Callers (BillingController::capture) MUST branch on this before
    // crediting anything — see PayPalCapture's doc comment.
    EXPECT_EQ(cap.status, "COMPLETED");
}

TEST(PayPalClient, ParseCaptureExtractsNonCompletedStatusVerbatim) {
    // PayPal answers 2xx for a PENDING or DECLINED capture too — the parser
    // must hand the real status back unmodified, not silently normalize it.
    for (const std::string& status : {std::string("PENDING"), std::string("DECLINED")}) {
        const std::string body = R"JSON({
            "purchase_units": [
                {
                    "payments": {
                        "captures": [
                            {
                                "id": "CAP-)JSON" +
                                 status + R"JSON(",
                                "status": ")JSON" +
                                 status + R"JSON(",
                                "amount": { "currency_code": "USD", "value": "5.00" }
                            }
                        ]
                    }
                }
            ]
        })JSON";
        const PayPalCapture cap = PayPalClient::parse_capture_response(body);
        EXPECT_EQ(cap.status, status);
    }
}

TEST(PayPalClient, ParseCaptureRejectsMalformedBody) {
    // Not JSON at all.
    EXPECT_THROW(PayPalClient::parse_capture_response("not json"), std::runtime_error);
    // Valid JSON, wrong shape.
    EXPECT_THROW(PayPalClient::parse_capture_response(R"({"id":"x"})"), std::runtime_error);
    // purchase_units present but empty.
    EXPECT_THROW(PayPalClient::parse_capture_response(R"({"purchase_units":[]})"), std::runtime_error);
    // captures array present but empty.
    EXPECT_THROW(PayPalClient::parse_capture_response(R"({"purchase_units":[{"payments":{"captures":[]}}]})"),
                 std::runtime_error);
    // capture present but missing amount entirely.
    EXPECT_THROW(
        PayPalClient::parse_capture_response(R"({"purchase_units":[{"payments":{"captures":[{"id":"c1"}]}}]})"),
        std::runtime_error);
    // capture present with amount but missing currency_code.
    EXPECT_THROW(PayPalClient::parse_capture_response(
                     R"({"purchase_units":[{"payments":{"captures":[{"id":"c1","amount":{"value":"1.00"}}]}}]})"),
                 std::runtime_error);
    // capture present with a full amount but missing status entirely — must
    // never silently default to an empty (falsy-looking, but NOT "COMPLETED")
    // status; a caller comparing against "COMPLETED" would treat that as
    // "not completed" today, but requiring it up front removes any doubt.
    EXPECT_THROW(PayPalClient::parse_capture_response(R"({"purchase_units":[{"payments":{"captures":[{"id":"c1",)"
                                                      R"("amount":{"value":"1.00","currency_code":"USD"}}]}}]})"),
                 std::runtime_error);

    // None of the malformed bodies above may fall through to a default
    // PayPalCapture{} (capture_id empty, amount_cents 0) — every one of the
    // EXPECT_THROW calls above proves the function actually threw rather
    // than silently returning a zero-amount capture.
}

// ---------------------------------------------------------------------------
// Money-critical: decimal -> cents parser edge cases. Not brief-named, but
// required by the task ("Handle: ... pick one, document it, test it").
// ---------------------------------------------------------------------------

TEST(PayPalClient, ParseDecimalToCentsWholeNumberHasNoFractionalPart) {
    EXPECT_EQ(detail::parse_decimal_to_cents("12"), 1200);
    EXPECT_EQ(detail::parse_decimal_to_cents("0"), 0);
}

TEST(PayPalClient, ParseDecimalToCentsPadsOneFractionalDigit) {
    EXPECT_EQ(detail::parse_decimal_to_cents("12.3"), 1230);
    EXPECT_EQ(detail::parse_decimal_to_cents("0.5"), 50);
}

TEST(PayPalClient, ParseDecimalToCentsTwoFractionalDigits) {
    EXPECT_EQ(detail::parse_decimal_to_cents("12.34"), 1234);
    EXPECT_EQ(detail::parse_decimal_to_cents("0.01"), 1);
    EXPECT_EQ(detail::parse_decimal_to_cents("19.99"), 1999);
}

TEST(PayPalClient, ParseDecimalToCentsRejectsMoreThanTwoFractionalDigits) {
    // Documented policy: reject, don't silently truncate — see the doc
    // comment on parse_decimal_to_cents for why truncation was rejected.
    EXPECT_THROW(detail::parse_decimal_to_cents("12.345"), std::runtime_error);
    EXPECT_THROW(detail::parse_decimal_to_cents("1.999"), std::runtime_error);
}

TEST(PayPalClient, ParseDecimalToCentsRejectsSignedInput) {
    EXPECT_THROW(detail::parse_decimal_to_cents("-1.00"), std::runtime_error);
    EXPECT_THROW(detail::parse_decimal_to_cents("+1.00"), std::runtime_error);
}

TEST(PayPalClient, ParseDecimalToCentsRejectsEmptyAndGarbage) {
    EXPECT_THROW(detail::parse_decimal_to_cents(""), std::runtime_error);
    EXPECT_THROW(detail::parse_decimal_to_cents("abc"), std::runtime_error);
    EXPECT_THROW(detail::parse_decimal_to_cents("12.3a"), std::runtime_error);
    EXPECT_THROW(detail::parse_decimal_to_cents("12."), std::runtime_error);
    EXPECT_THROW(detail::parse_decimal_to_cents("."), std::runtime_error);
    EXPECT_THROW(detail::parse_decimal_to_cents(".5"), std::runtime_error);
    EXPECT_THROW(detail::parse_decimal_to_cents("12..34"), std::runtime_error);
    EXPECT_THROW(detail::parse_decimal_to_cents("1 2.34"), std::runtime_error);
}

TEST(PayPalClient, ParseDecimalToCentsRejectsOversizedIntegerPart) {
    // 16 digits: just past the >15 guard.
    EXPECT_THROW(detail::parse_decimal_to_cents("1234567890123456.00"), std::runtime_error);
    // 19 digits: at the int64_t digit-count boundary (INT64_MAX is
    // 9223372036854775807, 19 digits) — must still be rejected by the size
    // check before std::stoll ever runs, not merely "happen to" throw from
    // std::stoll's own out_of_range. Confirms the guard isn't just wide
    // enough for the first overflow example anyone thinks to write.
    EXPECT_THROW(detail::parse_decimal_to_cents("9223372036854775807.00"), std::runtime_error);
}

TEST(PayPalClient, CentsToDecimalStringRoundTripsThroughParse) {
    for (std::int64_t cents : {0LL, 1LL, 9LL, 10LL, 99LL, 100LL, 1234LL, 1999LL, 1000000LL}) {
        const std::string s = detail::cents_to_decimal_string(cents);
        EXPECT_EQ(detail::parse_decimal_to_cents(s), cents) << "round-trip failed for " << cents;
    }
}

TEST(PayPalClient, CentsToDecimalStringRejectsNegative) {
    EXPECT_THROW(detail::cents_to_decimal_string(-1), std::runtime_error);
}

// ---------------------------------------------------------------------------
// Header extraction used by verify_webhook_signature — pure, no network.
// ---------------------------------------------------------------------------

TEST(PayPalClient, FindHeaderCiIsCaseInsensitive) {
    const std::map<std::string, std::string> headers = {
        {"PayPal-Transmission-Id", "abc123"},
        {"paypal-cert-url", "https://api.paypal.com/cert"},
    };
    EXPECT_EQ(detail::find_header_ci(headers, "paypal-transmission-id"), "abc123");
    EXPECT_EQ(detail::find_header_ci(headers, "PAYPAL-CERT-URL"), "https://api.paypal.com/cert");
    EXPECT_EQ(detail::find_header_ci(headers, "paypal-transmission-sig"), "");
}

// ---------------------------------------------------------------------------
// describe_error_body — pure, no network. Feeds non-2xx exception messages;
// PayPal support cannot look anything up without debug_id, so this must
// actually extract it rather than discarding the response body.
// ---------------------------------------------------------------------------

TEST(PayPalClient, DescribeErrorBodyExtractsOrdersApiFields) {
    const std::string body =
        R"({"name":"RESOURCE_NOT_FOUND","message":"The requested resource was not found.","debug_id":"abc123def456"})";
    const std::string desc = detail::describe_error_body(body);
    EXPECT_NE(desc.find("name=RESOURCE_NOT_FOUND"), std::string::npos);
    EXPECT_NE(desc.find("message=The requested resource was not found."), std::string::npos);
    EXPECT_NE(desc.find("debug_id=abc123def456"), std::string::npos);
}

TEST(PayPalClient, DescribeErrorBodyExtractsOAuthFields) {
    const std::string body = R"({"error":"invalid_client","error_description":"Client Authentication failed"})";
    const std::string desc = detail::describe_error_body(body);
    EXPECT_NE(desc.find("error=invalid_client"), std::string::npos);
    EXPECT_NE(desc.find("error_description=Client Authentication failed"), std::string::npos);
}

TEST(PayPalClient, DescribeErrorBodyExtractsDetailsIssue) {
    // Real PayPal Orders API 422 shape for "capture an order the buyer never
    // approved" — the actionable code lives in details[].issue, not in name
    // (which is just the generic "UNPROCESSABLE_ENTITY" bucket).
    const std::string body =
        R"({"name":"UNPROCESSABLE_ENTITY","message":"The requested action could not be performed.",)"
        R"("details":[{"issue":"ORDER_NOT_APPROVED","description":"Payer has not yet approved the Order"}],)"
        R"("debug_id":"xyz789"})";
    const std::string desc = detail::describe_error_body(body);
    EXPECT_NE(desc.find("name=UNPROCESSABLE_ENTITY"), std::string::npos);
    EXPECT_NE(desc.find("issue=ORDER_NOT_APPROVED"), std::string::npos);
}

TEST(PayPalClient, DescribeErrorBodyFallsBackToRawBodyWhenNotJson) {
    EXPECT_EQ(detail::describe_error_body("not json at all"), "not json at all");
}

TEST(PayPalClient, DescribeErrorBodyFallsBackWhenJsonIsNotAnObject) {
    EXPECT_EQ(detail::describe_error_body("[1,2,3]"), "[1,2,3]");
}

TEST(PayPalClient, DescribeErrorBodyFallsBackWhenJsonHasNoKnownFields) {
    const std::string desc = detail::describe_error_body(R"({"foo":"bar"})");
    EXPECT_NE(desc.find("foo"), std::string::npos);
    EXPECT_NE(desc.find("bar"), std::string::npos);
}

TEST(PayPalClient, DescribeErrorBodyTruncatesOversizedRawBody) {
    const std::string huge(1000, 'x');  // not valid JSON, well past the 500-byte cap
    const std::string desc = detail::describe_error_body(huge);
    EXPECT_LT(desc.size(), huge.size());
    EXPECT_NE(desc.find("...(truncated)"), std::string::npos);
}

// ---------------------------------------------------------------------------
// Test seam — install_for_testing()/reset_for_testing()/is_initialized().
// A fake subclass proves get() actually dispatches to the installed
// override (no real network call happens here).
// ---------------------------------------------------------------------------

class FakePayPalClient : public Billing::PayPalClient {
public:
    FakePayPalClient() : Billing::PayPalClient(Billing::PayPalClientConfig{}) {}

    Billing::PayPalOrder create_order(std::int64_t /*amount_cents*/,
                                      const std::string& /*currency*/,
                                      const std::string& /*reference*/,
                                      const std::string& /*return_url*/,
                                      const std::string& /*cancel_url*/) override {
        return Billing::PayPalOrder{"FAKE-ORDER-ID", "https://example.test/approve"};
    }
};

TEST(PayPalClient, InstallForTestingInjectsFakeClientWithNoNetworkCall) {
    Billing::reset_for_testing();
    EXPECT_FALSE(Billing::is_initialized());

    Billing::install_for_testing(std::make_unique<FakePayPalClient>());
    EXPECT_TRUE(Billing::is_initialized());

    const Billing::PayPalOrder order =
        Billing::PayPalClient::get().create_order(1000, "USD", "ref", "https://return.test", "https://cancel.test");
    EXPECT_EQ(order.order_id, "FAKE-ORDER-ID");
    EXPECT_EQ(order.approve_url, "https://example.test/approve");

    Billing::reset_for_testing();
    EXPECT_FALSE(Billing::is_initialized());
}

// ---------------------------------------------------------------------------
// initialize() — eager config validation. Uses a throwaway Config so this
// stays a "pure logic" test (no network): PayPalClient's constructor itself
// never dials out, only create_order/capture_order/verify_webhook_signature
// do.
// ---------------------------------------------------------------------------

class PayPalClientInitializeTest : public ::testing::Test {
protected:
    void TearDown() override {
        Billing::reset_for_testing();
        TestHelpers::reset_all_globals();
    }
};

TEST_F(PayPalClientInitializeTest, ThrowsWhenBillingEnabledWithoutCredentials) {
    const auto path = TestHelpers::create_temp_config(
        R"({"billing": {"enabled": true, "paypal": {"client_id": "", "client_secret": ""}}})",
        "paypal_client_test_missing_creds.json");
    Config::initialize(path);
    EXPECT_THROW(Billing::initialize(), std::runtime_error);
    EXPECT_FALSE(Billing::is_initialized());
    TestHelpers::remove_temp_config(path);
}

TEST_F(PayPalClientInitializeTest, SucceedsWhenBillingEnabledWithCredentials) {
    const auto path = TestHelpers::create_temp_config(
        R"({"billing": {"enabled": true, "paypal": {"client_id": "id", "client_secret": "secret"}}})",
        "paypal_client_test_has_creds.json");
    Config::initialize(path);
    EXPECT_NO_THROW(Billing::initialize());
    EXPECT_TRUE(Billing::is_initialized());
    TestHelpers::remove_temp_config(path);
}

TEST_F(PayPalClientInitializeTest, DoesNotThrowWhenBillingDisabledEvenWithoutCredentials) {
    const auto path =
        TestHelpers::create_temp_config(R"({"billing": {"enabled": false}})", "paypal_client_test_billing_off.json");
    Config::initialize(path);
    EXPECT_NO_THROW(Billing::initialize());
    EXPECT_TRUE(Billing::is_initialized());
    TestHelpers::remove_temp_config(path);
}

}  // namespace
