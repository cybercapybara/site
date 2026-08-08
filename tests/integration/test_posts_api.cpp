/**
 * @file test_posts_api.cpp
 * @brief Integration tests for PostsController — admin CRUD, preview
 *        tokens, public JSON, and the Core::content_enabled() module gate.
 *
 * Needs the posts migration (006, applied unconditionally regardless of the
 * content flag — see test_post_repository.cpp) plus the roles seeded by
 * migration 001 ("User" / "Administrator", ON CONFLICT DO NOTHING).
 *
 * Fixture pattern mirrors test_admin_flow.cpp: CoreBackedTest with
 * auth.mode=jwt, direct controller invocation (no HTTP layer, no router —
 * see TestHelpers::authed/authed_json), and a seed_user() helper that mints
 * a user + matching AuthPrincipal for a given role.
 *
 * Note: PostRepository's own test suite (test_post_repository.cpp, Task 2)
 * substituted CreateRejectsDuplicateSlug for the brief's suggested
 * auto-slug-with-dedup case — there is no slugify/dedup mechanism anywhere in
 * the fork; slug is caller-supplied and validated here at the controller
 * layer (format regex + 409 on duplicate). DuplicateSlugReturns409NotHtml500
 * and RejectsUnsafeSlug below exercise exactly that controller-layer
 * contract, ported from $FORK/tests/integration/test_post.cpp.
 */

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "api/PostsController.hpp"
#include "domain/Role.hpp"
#include "repositories/RoleRepository.hpp"
#include "repositories/UserRepository.hpp"
#include "security/Auth.hpp"
#include "security/Tokens.hpp"
#include "test_helpers.hpp"

using json = nlohmann::json;
using namespace drogon;

namespace {

constexpr const char* kSecret = "test-jwt-secret-for-posts-api-flow-pad";

// ── Fixture: content module ENABLED ─────────────────────────────────────────
class PostsApiTest : public TestHelpers::CoreBackedTest {
protected:
    Api::PostsController controller;

    std::string config_file_name() const override { return "posts_api_test_config.json"; }

    void config_overrides(json& cfg) override {
        cfg["auth"]["mode"] = "jwt";
        cfg["auth"]["jwt"]["secret"] = kSecret;
        cfg["mail"]["enabled"] = false;
        cfg["database"]["migrations_enabled"] = true;
        cfg["database"]["migrations_dir"] = "migrations";
        cfg["content"]["enabled"] = true;
    }

    void SetUp() override {
        TestHelpers::CoreBackedTest::SetUp();
        if (::testing::Test::IsSkipped())
            return;
        Database::get().execute_write([](auto& txn) {
            txn.exec("TRUNCATE TABLE posts");
            txn.exec("TRUNCATE TABLE users CASCADE");
            txn.exec("DELETE FROM roles WHERE name NOT IN ('User', 'Administrator')");
            return 0;
        });
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

    HttpResponsePtr createAs(const Security::Auth::AuthPrincipal& p, const json& body) {
        auto req = TestHelpers::authed_json(p, body, Post);
        return call([&](auto cb) { controller.createPost(req, std::move(cb)); });
    }

    /// Seed helper: create a post via the admin handler and return its id.
    std::string seed_post(const Security::Auth::AuthPrincipal& admin,
                          const std::string& slug,
                          const std::string& title,
                          const std::string& status = "published") {
        json body = {{"slug", slug}, {"title", title}, {"summary", std::string("about ") + title}, {"status", status}};
        auto r = createAs(admin, body);
        EXPECT_EQ(r->statusCode(), k201Created);
        return json::parse(std::string(r->body()))["data"]["id"].get<std::string>();
    }
};

// ── Fixture: content module DISABLED (default) ──────────────────────────────
class PostsApiContentDisabledTest : public PostsApiTest {
protected:
    std::string config_file_name() const override { return "posts_api_content_disabled_test_config.json"; }

    void config_overrides(json& cfg) override {
        PostsApiTest::config_overrides(cfg);
        cfg["content"]["enabled"] = false;
    }
};

// ── Admin CRUD ───────────────────────────────────────────────────────────────

TEST_F(PostsApiTest, AdminCrudRoundtrip) {
    auto admin = seed_admin();

    // Create.
    json create_body = {{"slug", "crud-post"},
                        {"title", "CRUD Post"},
                        {"summary", "s"},
                        {"body", "# hi"},
                        {"status", "draft"},
                        {"topic", "Kubernetes"},
                        {"tags", {"k8s", "cpp"}}};
    auto created_resp = createAs(admin.principal, create_body);
    ASSERT_EQ(created_resp->statusCode(), k201Created);
    auto created = json::parse(std::string(created_resp->body()))["data"];
    const std::string id = created["id"];
    EXPECT_EQ(created["slug"], "crud-post");
    EXPECT_EQ(created["status"], "draft");

    // Get.
    auto get_resp = call([&](auto cb) { controller.getPost(TestHelpers::authed(admin.principal), std::move(cb), id); });
    ASSERT_EQ(get_resp->statusCode(), k200OK);
    EXPECT_EQ(json::parse(std::string(get_resp->body()))["data"]["slug"], "crud-post");

    // Patch.
    json patch_body = {{"title", "CRUD Post Updated"}, {"status", "published"}};
    auto patch_req = TestHelpers::authed_json(admin.principal, patch_body, Patch);
    auto patch_resp = call([&](auto cb) { controller.updatePost(patch_req, std::move(cb), id); });
    ASSERT_EQ(patch_resp->statusCode(), k200OK);
    auto patched = json::parse(std::string(patch_resp->body()))["data"];
    EXPECT_EQ(patched["title"], "CRUD Post Updated");
    EXPECT_EQ(patched["status"], "published");
    EXPECT_FALSE(patched["published_at"].is_null());

    // Delete.
    auto del_resp =
        call([&](auto cb) { controller.deletePost(TestHelpers::authed(admin.principal, Delete), std::move(cb), id); });
    ASSERT_EQ(del_resp->statusCode(), k200OK);

    // Gone.
    auto gone_resp =
        call([&](auto cb) { controller.getPost(TestHelpers::authed(admin.principal), std::move(cb), id); });
    EXPECT_EQ(gone_resp->statusCode(), k404NotFound);
}

TEST_F(PostsApiTest, NonAdminGets403OnCreate) {
    auto regular = seed_regular();
    json body = {{"slug", "should-not-exist"}, {"title", "Nope"}};
    auto resp = createAs(regular.principal, body);
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->statusCode(), k403Forbidden);
}

TEST_F(PostsApiTest, DuplicateSlugReturns409NotHtml500) {
    auto admin = seed_admin();
    json body = {{"slug", "dup-slug"}, {"title", "First"}, {"status", "draft"}};
    auto r1 = createAs(admin.principal, body);
    ASSERT_EQ(r1->statusCode(), k201Created);
    body["title"] = "Second";
    auto r2 = createAs(admin.principal, body);
    EXPECT_EQ(r2->statusCode(), k409Conflict);  // typed DuplicatePost -> 409, never a bare 500
}

TEST_F(PostsApiTest, RejectsUnsafeSlug) {
    auto admin = seed_admin();
    for (const char* bad : {"has space", "Upper", "slash/here", "has#hash", "-lead", "trail-"}) {
        json body = {{"slug", bad}, {"title", "T"}};
        auto resp = createAs(admin.principal, body);
        EXPECT_EQ(resp->statusCode(), k400BadRequest) << "slug: " << bad;
    }
    json ok = {{"slug", "safe-slug-ok"}, {"title", "T"}};
    auto r = createAs(admin.principal, ok);
    EXPECT_EQ(r->statusCode(), k201Created);
}

// ── Public JSON ──────────────────────────────────────────────────────────────

TEST_F(PostsApiTest, PublicListShowsOnlyPublished) {
    auto admin = seed_admin();
    seed_post(admin.principal, "pub-visible", "Visible", "published");
    seed_post(admin.principal, "pub-hidden", "Hidden", "draft");

    auto resp = call([&](auto cb) { controller.publicListPosts(TestHelpers::make_request(Get), std::move(cb)); });
    ASSERT_EQ(resp->statusCode(), k200OK);
    auto body = json::parse(std::string(resp->body()));
    bool saw_visible = false;
    for (const auto& item : body["data"]) {
        EXPECT_NE(item.value("slug", ""), "pub-hidden");  // draft never leaks into the public list
        if (item.value("slug", "") == "pub-visible") {
            saw_visible = true;
            EXPECT_FALSE(item.contains("body"));  // card projection, no body
        }
    }
    EXPECT_TRUE(saw_visible);

    // Direct slug read on the draft is 404 too — not just absent from the list.
    auto draft_resp =
        call([&](auto cb) { controller.publicGetPost(TestHelpers::make_request(Get), std::move(cb), "pub-hidden"); });
    EXPECT_EQ(draft_resp->statusCode(), k404NotFound);
}

TEST_F(PostsApiTest, PreviewTokenRevealsDraft) {
    auto admin = seed_admin();
    const std::string id = seed_post(admin.principal, "preview-wip", "WIP", "draft");

    // Invisible without a token.
    auto r404 =
        call([&](auto cb) { controller.publicGetPost(TestHelpers::make_request(Get), std::move(cb), "preview-wip"); });
    EXPECT_EQ(r404->statusCode(), k404NotFound);

    // Admin issues a preview token.
    auto issue_resp =
        call([&](auto cb) { controller.previewToken(TestHelpers::authed(admin.principal, Post), std::move(cb), id); });
    ASSERT_EQ(issue_resp->statusCode(), k200OK);
    auto issued = json::parse(std::string(issue_resp->body()));
    const std::string url = issued["data"]["url"];
    ASSERT_NE(url.find("/posts/preview-wip?preview="), std::string::npos);
    const std::string token = url.substr(url.find("preview=") + 8);

    // Visible with the token.
    auto req_ok = TestHelpers::make_request(Get);
    req_ok->setParameter("preview", token);
    auto r_ok = call([&](auto cb) { controller.publicGetPost(req_ok, std::move(cb), "preview-wip"); });
    EXPECT_EQ(r_ok->statusCode(), k200OK);

    // A token bound to a different post id is rejected (behaves like 404).
    const std::string foreign = Security::Tokens::issue(kSecret,
                                                        "00000000-0000-0000-0000-000000000000",
                                                        Security::Tokens::Purpose::Preview,
                                                        std::chrono::seconds(3600));
    auto req_bad = TestHelpers::make_request(Get);
    req_bad->setParameter("preview", foreign);
    auto r_bad = call([&](auto cb) { controller.publicGetPost(req_bad, std::move(cb), "preview-wip"); });
    EXPECT_EQ(r_bad->statusCode(), k404NotFound);

    // Still absent from the public list even with a live preview token.
    auto list_resp = call([&](auto cb) { controller.publicListPosts(TestHelpers::make_request(Get), std::move(cb)); });
    for (const auto& item : json::parse(std::string(list_resp->body()))["data"])
        EXPECT_NE(item.value("slug", ""), "preview-wip");
}

// ── Module gate ────────────────────────────────────────────────────────────

TEST_F(PostsApiContentDisabledTest, AllRoutes404WhenContentDisabled) {
    // The content_enabled() guard is the FIRST check in every handler — it
    // fires before the admin guard, so even an anonymous request to an
    // admin-only route gets a uniform 404 instead of leaking a 403 (which
    // would reveal the route exists) while the module is off.
    auto anon = TestHelpers::make_request(Get);

    auto list_resp = call([&](auto cb) { controller.listPosts(anon, std::move(cb)); });
    EXPECT_EQ(list_resp->statusCode(), k404NotFound);

    auto create_resp = call([&](auto cb) {
        controller.createPost(TestHelpers::make_request(Post, json{{"slug", "x"}, {"title", "x"}}), std::move(cb));
    });
    EXPECT_EQ(create_resp->statusCode(), k404NotFound);

    auto get_resp =
        call([&](auto cb) { controller.getPost(anon, std::move(cb), "00000000-0000-0000-0000-000000000000"); });
    EXPECT_EQ(get_resp->statusCode(), k404NotFound);

    auto patch_resp = call([&](auto cb) {
        controller.updatePost(TestHelpers::make_request(Patch, json{{"title", "x"}}),
                              std::move(cb),
                              "00000000-0000-0000-0000-000000000000");
    });
    EXPECT_EQ(patch_resp->statusCode(), k404NotFound);

    auto delete_resp =
        call([&](auto cb) { controller.deletePost(anon, std::move(cb), "00000000-0000-0000-0000-000000000000"); });
    EXPECT_EQ(delete_resp->statusCode(), k404NotFound);

    auto preview_resp =
        call([&](auto cb) { controller.previewToken(anon, std::move(cb), "00000000-0000-0000-0000-000000000000"); });
    EXPECT_EQ(preview_resp->statusCode(), k404NotFound);

    auto public_list_resp = call([&](auto cb) { controller.publicListPosts(anon, std::move(cb)); });
    EXPECT_EQ(public_list_resp->statusCode(), k404NotFound);

    auto public_get_resp = call([&](auto cb) { controller.publicGetPost(anon, std::move(cb), "whatever"); });
    EXPECT_EQ(public_get_resp->statusCode(), k404NotFound);
}

}  // namespace
