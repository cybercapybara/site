/**
 * @file test_request_utils.cpp
 * @brief Unit tests for the path-normalizer / pagination helpers in
 *        src/api/RequestUtils.hpp (pure, no services).
 *
 * Path normalization does double duty: cardinality control for metrics/traces
 * AND log redaction of account tokens, so the redaction cases are security-
 * relevant — a regression would drop reset tokens back into the access log.
 */

#include <gtest/gtest.h>

#include "api/RequestUtils.hpp"

namespace {

using Api::normalize_path_for_metrics;

TEST(RequestUtilsTest, PlainPathUnchanged) {
    EXPECT_EQ(normalize_path_for_metrics("/api/jobs"), "/api/jobs");
    EXPECT_EQ(normalize_path_for_metrics("/"), "/");
    EXPECT_EQ(normalize_path_for_metrics("/healthz"), "/healthz");
}

TEST(RequestUtilsTest, UuidSegmentRedacted) {
    EXPECT_EQ(normalize_path_for_metrics("/api/jobs/123e4567-e89b-12d3-a456-426614174000"), "/api/jobs/:id");
    EXPECT_EQ(normalize_path_for_metrics("/api/admin/users/123e4567-e89b-12d3-a456-426614174000"),
              "/api/admin/users/:id");
    // UUID mid-path + trailing segment.
    EXPECT_EQ(normalize_path_for_metrics("/api/jobs/dlq/123e4567-e89b-12d3-a456-426614174000/requeue"),
              "/api/jobs/dlq/:id/requeue");
}

TEST(RequestUtilsTest, AccountTokensRedacted) {
    EXPECT_EQ(normalize_path_for_metrics("/api/account/confirm/eyJhbGciOi.JTV.sig"), "/api/account/confirm/:token");
    EXPECT_EQ(normalize_path_for_metrics("/api/account/reset-password/some.long.token"),
              "/api/account/reset-password/:token");
    EXPECT_EQ(normalize_path_for_metrics("/api/account/change-email/some.long.token"),
              "/api/account/change-email/:token");
    // Regression: the invite-redeem token (a 7-day account-takeover secret) must
    // be redacted too — it was leaking into access logs + the metric path label.
    EXPECT_EQ(normalize_path_for_metrics("/api/account/join-from-invite/eyJhbGciOi.JTV.sig"),
              "/api/account/join-from-invite/:token");
}

TEST(RequestUtilsTest, RequestVariantsNotMistakenForTokenRoutes) {
    // The *-request / *-resend single-segment routes carry no token.
    EXPECT_EQ(normalize_path_for_metrics("/api/account/reset-password-request"), "/api/account/reset-password-request");
    EXPECT_EQ(normalize_path_for_metrics("/api/account/confirm-resend"), "/api/account/confirm-resend");
}

TEST(RequestUtilsTest, IsValidUuid) {
    EXPECT_TRUE(Api::is_valid_uuid("123e4567-e89b-12d3-a456-426614174000"));
    EXPECT_FALSE(Api::is_valid_uuid("not-a-uuid"));
    EXPECT_FALSE(Api::is_valid_uuid("123e4567e89b12d3a456426614174000"));  // no dashes
    EXPECT_FALSE(Api::is_valid_uuid(""));
}

TEST(RequestUtilsTest, ParseIntClamp) {
    EXPECT_EQ(Api::parse_int("42", 7), 42);
    EXPECT_EQ(Api::parse_int("", 7), 7);
    EXPECT_EQ(Api::parse_int("abc", 7), 7);
    EXPECT_EQ(Api::clamp_int("500", 20, 1, 200), 200);
    EXPECT_EQ(Api::clamp_int("-5", 20, 0, 200), 0);
    EXPECT_EQ(Api::clamp_int("", 20, 1, 200), 20);
}

}  // namespace
