/**
 * @file test_content_pages.cpp
 * @brief Integration tests for ContentPagesController — Markdown-by-slug and
 *        the dynamic sitemap.xml, plus the Core::content_enabled() gate.
 *
 * Fixture pattern mirrors test_posts_api.cpp (PostsApiTest): CoreBackedTest
 * with migrations on, content.enabled toggled per fixture. Posts are seeded
 * directly via Repositories::PostRepository — these two handlers carry no
 * auth guard (they're the public content surface), so there's no need to go
 * through PostsController's admin API just to create fixture rows. A JWT
 * secret is configured (unrelated to these routes' own auth-free status)
 * purely so Security::Tokens::issue/verify — the preview-token machinery
 * post_markdown() now shares with PostsController — has a master secret to
 * derive from, same as PostsApiTest's kSecret.
 */

#include <chrono>

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "api/ContentPagesController.hpp"
#include "repositories/PostRepository.hpp"
#include "security/Tokens.hpp"
#include "test_helpers.hpp"

using json = nlohmann::json;
using namespace drogon;

namespace {

constexpr const char* kSecret = "test-jwt-secret-for-content-pages-flow-pad";

// ── Fixture: content module ENABLED ─────────────────────────────────────────
class ContentPagesTest : public TestHelpers::CoreBackedTest {
protected:
    Api::ContentPagesController controller;

    std::string config_file_name() const override { return "content_pages_test_config.json"; }

    void config_overrides(json& cfg) override {
        cfg["database"]["migrations_enabled"] = true;
        cfg["database"]["migrations_dir"] = "migrations";
        cfg["content"]["enabled"] = true;
        cfg["auth"]["mode"] = "jwt";
        cfg["auth"]["jwt"]["secret"] = kSecret;
    }

    void SetUp() override {
        TestHelpers::CoreBackedTest::SetUp();
        if (::testing::Test::IsSkipped())
            return;
        Database::get().execute_write([](auto& txn) {
            txn.exec("TRUNCATE TABLE posts");
            return 0;
        });
    }

    template <typename Fn>
    static HttpResponsePtr call(Fn&& fn) {
        HttpResponsePtr resp;
        fn([&](const HttpResponsePtr& r) { resp = r; });
        return resp;
    }

    /// Seed a post straight through the repository (no controller/auth
    /// needed — the two handlers under test are unauthenticated reads).
    static std::string seed_post(const std::string& slug,
                                 const std::string& title,
                                 const std::string& body,
                                 const std::string& status) {
        Repositories::PostRepository repo;
        Repositories::PostInput in;
        in.slug = slug;
        in.title = title;
        in.body = body;
        in.status = status;
        auto created = repo.create(in);
        return created.id;
    }

    HttpResponsePtr getMarkdown(const std::string& slug, const std::string& preview = "") {
        auto req = TestHelpers::make_request(Get);
        if (!preview.empty())
            req->setParameter("preview", preview);
        return call([&](auto cb) { controller.post_markdown(req, std::move(cb), slug); });
    }

    HttpResponsePtr getSitemap() {
        auto req = TestHelpers::make_request(Get);
        return call([&](auto cb) { controller.sitemap(req, std::move(cb)); });
    }
};

// ── Fixture: content module DISABLED (default) ──────────────────────────────
class ContentPagesContentDisabledTest : public ContentPagesTest {
protected:
    std::string config_file_name() const override { return "content_pages_content_disabled_test_config.json"; }

    void config_overrides(json& cfg) override {
        ContentPagesTest::config_overrides(cfg);
        cfg["content"]["enabled"] = false;
    }
};

// ── Markdown-by-slug ─────────────────────────────────────────────────────────

TEST_F(ContentPagesTest, MarkdownBodyExactMatchForPublishedPost) {
    seed_post("markdown-post", "Markdown Post", "Some **bold** body text.", "published");

    auto resp = getMarkdown("markdown-post");
    ASSERT_EQ(resp->statusCode(), k200OK);
    // Direct controller invocation never serializes to the wire, so the
    // content type set via setContentTypeString() doesn't land in the
    // headers_ map getHeader() reads — it's only materialized into a
    // "content-type" header during renderToBuffer(). contentTypeString()
    // is the accessor that works for both direct-invocation (here/other
    // integration tests) and over-the-wire responses (test_http_e2e.cpp,
    // where getHeader("content-type") does work).
    EXPECT_NE(resp->contentTypeString().find("text/markdown"), std::string::npos);
    EXPECT_EQ(std::string(resp->body()), "# Markdown Post\n\nSome **bold** body text.");
}

TEST_F(ContentPagesTest, MarkdownFourOhFourForDraft) {
    seed_post("draft-post", "Draft Post", "wip", "draft");

    auto resp = getMarkdown("draft-post");
    EXPECT_EQ(resp->statusCode(), k404NotFound);
    EXPECT_EQ(std::string(resp->body()), "# 404\n\nNot found.\n");
}

TEST_F(ContentPagesTest, MarkdownFourOhFourForUnknownSlug) {
    auto resp = getMarkdown("does-not-exist");
    EXPECT_EQ(resp->statusCode(), k404NotFound);
    EXPECT_EQ(std::string(resp->body()), "# 404\n\nNot found.\n");
}

// A valid, matching preview token reveals a draft's Markdown; a garbage
// token behaves exactly like no token at all (404) — same contract
// PostsApiTest.PreviewTokenRevealsDraft exercises against the JSON route,
// now proven against the Markdown route that shares the same resolver.
TEST_F(ContentPagesTest, PreviewTokenServesDraftMarkdown) {
    const std::string id = seed_post("preview-wip", "WIP", "shh, not published yet.", "draft");

    // Invisible without a token — same as any other draft.
    auto r404 = getMarkdown("preview-wip");
    EXPECT_EQ(r404->statusCode(), k404NotFound);
    EXPECT_EQ(std::string(r404->body()), "# 404\n\nNot found.\n");

    // Visible with a valid token bound to this post's id.
    const std::string token =
        Security::Tokens::issue(kSecret, id, Security::Tokens::Purpose::Preview, std::chrono::seconds(3600));
    auto r_ok = getMarkdown("preview-wip", token);
    ASSERT_EQ(r_ok->statusCode(), k200OK);
    // See MarkdownBodyExactMatchForPublishedPost above on why this reads
    // contentTypeString() rather than the getHeader("content-type") the
    // wire-level e2e suite uses.
    EXPECT_NE(r_ok->contentTypeString().find("text/markdown"), std::string::npos);
    EXPECT_EQ(std::string(r_ok->body()), "# WIP\n\nshh, not published yet.");

    // A garbage token is rejected the same as no token — 404, not a 500 or a
    // leak that the slug exists.
    auto r_garbage = getMarkdown("preview-wip", "not-a-real-token");
    EXPECT_EQ(r_garbage->statusCode(), k404NotFound);
    EXPECT_EQ(std::string(r_garbage->body()), "# 404\n\nNot found.\n");

    // A token bound to a different post id is rejected too (foreign token).
    const std::string foreign = Security::Tokens::issue(kSecret,
                                                        "00000000-0000-0000-0000-000000000000",
                                                        Security::Tokens::Purpose::Preview,
                                                        std::chrono::seconds(3600));
    auto r_foreign = getMarkdown("preview-wip", foreign);
    EXPECT_EQ(r_foreign->statusCode(), k404NotFound);
    EXPECT_EQ(std::string(r_foreign->body()), "# 404\n\nNot found.\n");
}

// ── Sitemap ──────────────────────────────────────────────────────────────────

TEST_F(ContentPagesTest, SitemapListsPublishedPostWithLastmod) {
    seed_post("sitemap-post", "Sitemap Post", "body", "published");

    auto resp = getSitemap();
    ASSERT_EQ(resp->statusCode(), k200OK);
    // See MarkdownBodyExactMatchForPublishedPost above on why this reads
    // contentTypeString() rather than the getHeader("content-type") the
    // wire-level e2e suite uses.
    EXPECT_NE(resp->contentTypeString().find("application/xml"), std::string::npos);
    const std::string body(resp->body());
    EXPECT_NE(body.find("<loc>http://localhost:8080/posts/sitemap-post</loc><lastmod>"), std::string::npos) << body;
}

TEST_F(ContentPagesTest, SitemapExcludesDraftsAndEscapesSlug) {
    seed_post("draft-not-in-sitemap", "Draft", "body", "draft");
    // '&' is not valid in the controller's own slug charset, but the sitemap
    // XML-escapes every <loc> unconditionally — cover that directly against
    // a PostRepository-seeded row (bypasses PostsController's slug regex).
    seed_post("sitemap-a&b", "Ampersand Slug", "body", "published");

    auto resp = getSitemap();
    ASSERT_EQ(resp->statusCode(), k200OK);
    const std::string body(resp->body());
    EXPECT_EQ(body.find("draft-not-in-sitemap"), std::string::npos) << body;
    EXPECT_NE(body.find("/posts/sitemap-a&amp;b</loc>"), std::string::npos) << body;
}

// ── Module gate ────────────────────────────────────────────────────────────

TEST_F(ContentPagesContentDisabledTest, MarkdownFourOhFourWhenContentDisabled) {
    // Seeding bypasses the controller, so this only proves the *handler's*
    // own guard fires — same shape as PostsApiContentDisabledTest.
    auto resp = getMarkdown("whatever");
    EXPECT_EQ(resp->statusCode(), k404NotFound);
    EXPECT_EQ(std::string(resp->body()), "# 404\n\nNot found.\n");
}

TEST_F(ContentPagesContentDisabledTest, SitemapRootOnlyWhenContentDisabled) {
    // Seeding bypasses the controller/guard (straight through the
    // repository), so a post surfacing here would mean the handler skipped
    // its own guard and queried the repository anyway — sitemap() degrades
    // to a root-only feed instead of a 404 (unlike post_markdown) because a
    // sitemap is conceptually always present for the site; see the
    // handler's doc comment.
    seed_post("hidden-post", "Hidden", "body", "published");

    auto resp = getSitemap();
    ASSERT_EQ(resp->statusCode(), k200OK);
    const std::string body(resp->body());
    EXPECT_EQ(body.find("hidden-post"), std::string::npos) << body;
    EXPECT_NE(body.find("<loc>http://localhost:8080/</loc>"), std::string::npos) << body;
}

}  // namespace
