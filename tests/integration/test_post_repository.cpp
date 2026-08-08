/**
 * @file test_post_repository.cpp
 * @brief Integration tests for Repositories::PostRepository. Needs Postgres
 *        (the `posts` table from migration 006), so it lives in the
 *        integration bucket alongside the other repository suites
 *        (test_admin_flow.cpp, test_account_flow.cpp) that exercise
 *        UserRepository directly.
 */

#include <gtest/gtest.h>

#include "domain/Post.hpp"
#include "repositories/PostRepository.hpp"
#include "test_helpers.hpp"

using json = nlohmann::json;

namespace {

class PostRepositoryTest : public TestHelpers::CoreBackedTest {
protected:
    Repositories::PostRepository repo;

    std::string config_file_name() const override { return "post_repository_test_config.json"; }

    void config_overrides(json& cfg) override {
        cfg["database"]["migrations_enabled"] = true;
        cfg["database"]["migrations_dir"] = "migrations";
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

    static Repositories::PostInput make_input(const std::string& slug,
                                              const std::string& title,
                                              const std::string& status = "draft",
                                              std::vector<std::string> tags = {}) {
        Repositories::PostInput in;
        in.slug = slug;
        in.title = title;
        in.summary = "summary for " + title;
        in.body = "# " + title + "\n\nbody";
        in.status = status;
        in.topic = "Kubernetes";
        in.tags = std::move(tags);
        return in;
    }
};

TEST_F(PostRepositoryTest, CreateInsertsPostWithGivenFields) {
    auto in = make_input("hello-world", "Hello, world", "draft");
    auto created = repo.create(in);

    EXPECT_EQ(created.slug, "hello-world");
    EXPECT_EQ(created.title, "Hello, world");
    EXPECT_EQ(created.summary, in.summary);
    EXPECT_EQ(created.body, in.body);
    EXPECT_EQ(created.status, "draft");
    EXPECT_EQ(created.topic, "Kubernetes");
    // Drafts don't get a published_at stamp.
    EXPECT_FALSE(created.published_at.has_value());

    auto found = repo.find(created.id);
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found->slug, created.slug);
}

// The fork requires the caller (validated at the controller layer, ported in
// a later task) to supply a unique slug — PostRepository itself does not
// derive or dedupe slugs from the title. This mirrors
// PostsFlowTest.DuplicateSlugReturns409NotHtml500 in the fork's integration
// suite: a second insert with the same slug throws DuplicatePost so the HTTP
// layer can map it to 409 instead of a bare 500.
TEST_F(PostRepositoryTest, CreateRejectsDuplicateSlug) {
    repo.create(make_input("hello-world", "Hello World"));
    EXPECT_THROW(repo.create(make_input("hello-world", "Hello World Again")), Repositories::DuplicatePost);
}

TEST_F(PostRepositoryTest, ListPublishedExcludesDrafts) {
    repo.create(make_input("published-post", "Published Post", "published"));
    repo.create(make_input("draft-post", "Draft Post", "draft"));

    Repositories::PublicListFilter filter;
    auto cards = repo.list_published_cards(filter, /*limit=*/50, /*offset=*/0);

    ASSERT_EQ(cards.size(), 1u);
    EXPECT_EQ(cards[0].slug, "published-post");
    EXPECT_EQ(repo.count_published(filter), 1);
}

TEST_F(PostRepositoryTest, PublishSetsPublishedAtOnce) {
    auto draft = repo.create(make_input("re-publish-me", "Re-publish Me", "draft"));
    EXPECT_FALSE(draft.published_at.has_value());

    auto in = make_input("re-publish-me", "Re-publish Me", "published");
    auto first_publish = repo.update(draft.id, in);
    ASSERT_TRUE(first_publish.published_at.has_value());
    const std::string stamped_at = *first_publish.published_at;

    // Re-publishing (still status=published) must not move published_at —
    // update() COALESCEs against the existing value.
    in.title = "Re-publish Me (edited)";
    auto second_publish = repo.update(draft.id, in);
    ASSERT_TRUE(second_publish.published_at.has_value());
    EXPECT_EQ(*second_publish.published_at, stamped_at);
    EXPECT_EQ(second_publish.title, "Re-publish Me (edited)");
}

TEST_F(PostRepositoryTest, TagsRoundTripCommaJoined) {
    auto created = repo.create(make_input("tagged-post", "Tagged Post", "draft", {"k8s", "cpp"}));
    EXPECT_EQ(created.tags, (std::vector<std::string>{"k8s", "cpp"}));

    auto raw = Database::get().execute_read([&](auto& txn) {
        auto r = txn.exec_params("SELECT tags FROM posts WHERE id = $1", created.id);
        return r[0]["tags"].template as<std::string>();
    });
    EXPECT_EQ(raw, "k8s,cpp");

    auto found = repo.find(created.id);
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found->tags, (std::vector<std::string>{"k8s", "cpp"}));
}

}  // namespace
