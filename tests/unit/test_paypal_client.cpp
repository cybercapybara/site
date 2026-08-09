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
#include <stdexcept>
#include <string>

#include <gtest/gtest.h>

#include "billing/PayPalClient.hpp"

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
    EXPECT_THROW(detail::parse_decimal_to_cents("1234567890123456.00"), std::runtime_error);
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

}  // namespace
