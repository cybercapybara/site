/**
 * @file test_uploads_api.cpp
 * @brief Integration tests for UploadController — admin upload/list/delete
 *        over the local storage backend (storage.backend=local; no MinIO
 *        dependency, the storage layer abstracts it — see Storage.hpp), plus
 *        the Core::content_enabled() module gate shared with
 *        PostsController/ContentPagesController.
 *
 * Fixture pattern mirrors test_posts_api.cpp (PostsApiTest): CoreBackedTest
 * with auth.mode=jwt, direct controller invocation (no HTTP layer, no
 * router), and a seed_user() helper that mints a user + matching
 * AuthPrincipal for a given role. storage.backend is driven through the same
 * config path Core::initialize() uses in production (Storage::initialize),
 * rooted at a per-suite temp directory that's wiped before/after each test.
 */

#include <cstddef>
#include <filesystem>
#include <string>
#include <system_error>

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "api/UploadController.hpp"
#include "domain/Role.hpp"
#include "repositories/RoleRepository.hpp"
#include "repositories/UserRepository.hpp"
#include "security/Auth.hpp"
#include "storage/Storage.hpp"
#include "test_helpers.hpp"

using json = nlohmann::json;
using namespace drogon;

namespace {

constexpr const char* kSecret = "test-jwt-secret-for-uploads-api-flow-pad";

// Signature prefixes the sniff looks for (see Api::image_bytes_match).
// std::string(ptr, len) — the const char* ctor would stop at the PNG NUL.
const std::string kPngMagic("\x89PNG\r\n\x1a\n", 8);

// Build the multipart/form-data POST the admin editor's fetch() sends.
HttpRequestPtr upload_request(const std::string& filename,
                              const std::string& part_content_type,
                              const std::string& bytes) {
    const std::string boundary = "----uploadsApiTestBoundary";
    std::string body;
    body += "--" + boundary + "\r\n";
    body += "Content-Disposition: form-data; name=\"file\"; filename=\"" + filename + "\"\r\n";
    body += "Content-Type: " + part_content_type + "\r\n\r\n";
    body += bytes + "\r\n";
    body += "--" + boundary + "--\r\n";

    auto req = TestHelpers::make_request(Post);
    req->setBody(body);
    req->addHeader("Content-Type", "multipart/form-data; boundary=" + boundary);
    req->setContentTypeString("multipart/form-data; boundary=" + boundary);
    return req;
}

// ── Fixture: content module ENABLED, storage.backend=local ─────────────────
class UploadsApiTest : public TestHelpers::CoreBackedTest {
protected:
    Api::UploadController controller;
    std::filesystem::path storage_root_ = std::filesystem::temp_directory_path() / "uploads-api-test-storage";

    std::string config_file_name() const override { return "uploads_api_test_config.json"; }

    void config_overrides(json& cfg) override {
        cfg["auth"]["mode"] = "jwt";
        cfg["auth"]["jwt"]["secret"] = kSecret;
        cfg["mail"]["enabled"] = false;
        cfg["database"]["migrations_enabled"] = true;
        cfg["database"]["migrations_dir"] = "migrations";
        cfg["content"]["enabled"] = true;
        // Local backend, no CDN base — exercises the same-origin /uploads/
        // serving path UploadController::serveUpload answers.
        cfg["storage"]["backend"] = "local";
        cfg["storage"]["local"]["root"] = storage_root_.string();
        cfg["storage"]["public_base_url"] = "";
    }

    void SetUp() override {
        std::error_code ec;
        std::filesystem::remove_all(storage_root_, ec);
        TestHelpers::CoreBackedTest::SetUp();
        if (::testing::Test::IsSkipped())
            return;
        Database::get().execute_write([](auto& txn) {
            txn.exec("TRUNCATE TABLE users CASCADE");
            txn.exec("DELETE FROM roles WHERE name NOT IN ('User', 'Administrator')");
            return 0;
        });
    }

    void TearDown() override {
        TestHelpers::CoreBackedTest::TearDown();
        std::error_code ec;
        std::filesystem::remove_all(storage_root_, ec);
    }

    struct Pair {
        Domain::User user;
        Security::Auth::AuthPrincipal principal;
    };

    Pair seed_user(const std::string& email, const std::string& role_name) {
        Repositories::RoleRepository roles;
        Repositories::UserRepository users;
        auto role = roles.find_by_name(role_name);
        if (!role) {
            ADD_FAILURE() << "role " << role_name << " missing — seed migration?";
            throw std::runtime_error("seed role missing: " + role_name);
        }
        auto created = users.create(
            email, std::string("$argon2id$placeholder"), std::nullopt, std::nullopt, role->id, /*confirmed=*/true);
        auto fresh = users.find(created.id);
        Pair p;
        p.user = fresh ? *fresh : created;
        p.principal.subject = p.user.id;
        if (p.user.role)
            p.principal.roles.push_back(p.user.role->name);
        p.principal.raw_claims = json{{"sub", p.user.id}, {"permissions", p.user.role ? p.user.role->permissions : 0u}};
        return p;
    }

    Pair seed_admin() { return seed_user("admin@example.com", "Administrator"); }
    Pair seed_regular() { return seed_user("user@example.com", "User"); }

    template <typename Fn>
    static HttpResponsePtr call(Fn&& fn) {
        HttpResponsePtr resp;
        fn([&](const HttpResponsePtr& r) { resp = r; });
        return resp;
    }

    HttpResponsePtr uploadAs(const Security::Auth::AuthPrincipal& p,
                             const std::string& filename,
                             const std::string& part_content_type,
                             const std::string& bytes) {
        auto req = TestHelpers::with_principal(upload_request(filename, part_content_type, bytes), p);
        return call([&](auto cb) { controller.upload(req, std::move(cb)); });
    }

    HttpResponsePtr listAs(const Security::Auth::AuthPrincipal& p) {
        return call([&](auto cb) { controller.listUploads(TestHelpers::authed(p, Get), std::move(cb)); });
    }

    HttpResponsePtr deleteAs(const Security::Auth::AuthPrincipal& p, const std::string& name) {
        return call([&](auto cb) { controller.deleteUpload(TestHelpers::authed(p, Delete), std::move(cb), name); });
    }
};

// ── Fixture: content module DISABLED (default) ──────────────────────────────
class UploadsApiContentDisabledTest : public UploadsApiTest {
protected:
    std::string config_file_name() const override { return "uploads_api_content_disabled_test_config.json"; }

    void config_overrides(json& cfg) override {
        UploadsApiTest::config_overrides(cfg);
        cfg["content"]["enabled"] = false;
    }
};

// ── Admin upload / list / delete roundtrip ──────────────────────────────────

TEST_F(UploadsApiTest, UploadListDeleteRoundtrip) {
    auto admin = seed_admin();

    // Upload: stored under posts/ with a random key, never the client filename.
    auto up_resp = uploadAs(admin.principal, "pic.png", "image/png", kPngMagic + "payload bytes");
    ASSERT_EQ(up_resp->statusCode(), k201Created) << up_resp->body();
    auto up_body = json::parse(std::string(up_resp->body()));
    const std::string key = up_body["data"]["key"].get<std::string>();
    EXPECT_EQ(key.rfind("posts/", 0), 0u) << key;
    EXPECT_NE(key.find(".png"), std::string::npos) << key;
    const std::string name = key.substr(key.rfind('/') + 1);
    // Local backend, no public_base_url configured — url() is the same-origin
    // /uploads/<key> path serveUpload answers (Storage.hpp's kLocalPublicPrefix).
    EXPECT_EQ(up_body["data"]["url"].get<std::string>(), std::string("/uploads/") + key);
    EXPECT_TRUE(Storage::get().exists(key));

    // List: contains the uploaded key, with the admin media-library envelope.
    auto list_resp = listAs(admin.principal);
    ASSERT_EQ(list_resp->statusCode(), k200OK);
    auto list_body = json::parse(std::string(list_resp->body()));
    EXPECT_EQ(list_body["total"], 1);
    ASSERT_EQ(list_body["data"].size(), 1u);
    EXPECT_EQ(list_body["data"][0]["key"], key);
    EXPECT_EQ(list_body["data"][0]["content_type"], "image/png");

    // Delete by basename → gone from storage and from the listing.
    auto del_resp = deleteAs(admin.principal, name);
    ASSERT_EQ(del_resp->statusCode(), k200OK);
    EXPECT_FALSE(Storage::get().exists(key));

    auto list_after = listAs(admin.principal);
    ASSERT_EQ(list_after->statusCode(), k200OK);
    EXPECT_EQ(json::parse(std::string(list_after->body()))["total"], 0);

    // Deleting an already-gone key is a 404, not a silent success.
    auto del_again = deleteAs(admin.principal, name);
    EXPECT_EQ(del_again->statusCode(), k404NotFound);
}

TEST_F(UploadsApiTest, NonAdminGets403OnUpload) {
    auto regular = seed_regular();
    auto resp = uploadAs(regular.principal, "pic.png", "image/png", kPngMagic + "payload");
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->statusCode(), k403Forbidden);
}

TEST_F(UploadsApiTest, ServeUploadReadsBackStoredBytesOnLocalBackend) {
    auto admin = seed_admin();
    auto up_resp = uploadAs(admin.principal, "pic.png", "image/png", kPngMagic + "payload bytes");
    ASSERT_EQ(up_resp->statusCode(), k201Created) << up_resp->body();
    const std::string key = json::parse(std::string(up_resp->body()))["data"]["key"].get<std::string>();

    HttpResponsePtr serve_resp;
    controller.serveUpload(
        TestHelpers::make_request(Get), [&](const HttpResponsePtr& r) { serve_resp = r; }, key);
    ASSERT_EQ(serve_resp->statusCode(), k200OK);
    EXPECT_EQ(std::string(serve_resp->body()), kPngMagic + "payload bytes");
    // Direct controller invocation never serializes to the wire, so
    // setContentTypeString()'s value doesn't land in the headers_ map
    // getHeader() reads — it's only materialized into a real "content-type"
    // header during renderToBuffer(). contentTypeString() is the accessor
    // that works for direct invocation (this suite reads the response
    // straight from the controller callback, no HTTP client involved).
    EXPECT_EQ(serve_resp->contentTypeString(), "image/png");
}

// ── Module gate ────────────────────────────────────────────────────────────

TEST_F(UploadsApiContentDisabledTest, AllRoutes404WhenContentDisabled) {
    // The content_enabled() guard is the FIRST check in every handler — same
    // contract PostsController/ContentPagesController use: it fires before
    // the admin guard, so even an anonymous request to an admin-only route
    // gets a uniform 404 instead of a 403 that would reveal the route exists.
    auto up_resp = call([&](auto cb) {
        controller.upload(upload_request("pic.png", "image/png", kPngMagic + "payload"), std::move(cb));
    });
    EXPECT_EQ(up_resp->statusCode(), k404NotFound);

    auto list_resp = call([&](auto cb) { controller.listUploads(TestHelpers::make_request(Get), std::move(cb)); });
    EXPECT_EQ(list_resp->statusCode(), k404NotFound);

    auto del_resp = call(
        [&](auto cb) { controller.deleteUpload(TestHelpers::make_request(Delete), std::move(cb), "whatever.png"); });
    EXPECT_EQ(del_resp->statusCode(), k404NotFound);

    // The public serving route is gated too — nothing is reachable while the
    // module is off, not even a previously-stored object.
    auto serve_resp =
        call([&](auto cb) { controller.serveUpload(TestHelpers::make_request(Get), std::move(cb), "posts/x.png"); });
    EXPECT_EQ(serve_resp->statusCode(), k404NotFound);
}

}  // namespace
