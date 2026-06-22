#include <gtest/gtest.h>

#include "core/Core.hpp"
#include "test_helpers.hpp"

// These tests exercise Application::register_health_check / health_report
// directly — no subsystems are actually spun up, so we can only assert
// API contract (registration, aggregation, error handling).

class HealthRegistryTest : public ::testing::Test {
protected:
    void TearDown() override { TestHelpers::reset_all_globals(); }
};

TEST_F(HealthRegistryTest, EmptyReportWhenNothingRegistered) {
    Core::Application app;
    auto report = app.health_report();
    EXPECT_TRUE(report.empty());
}

TEST_F(HealthRegistryTest, RegisteredProbeShowsInReport) {
    Core::Application app;
    app.register_health_check("mycomp", [] { return true; });
    auto report = app.health_report();
    ASSERT_EQ(report.size(), 1u);
    EXPECT_EQ(report[0].name, "mycomp");
    EXPECT_TRUE(report[0].healthy);
}

TEST_F(HealthRegistryTest, FailingProbeMarksUnhealthy) {
    Core::Application app;
    app.register_health_check("bad", [] { return false; });
    auto report = app.health_report();
    ASSERT_EQ(report.size(), 1u);
    EXPECT_FALSE(report[0].healthy);
}

TEST_F(HealthRegistryTest, ThrowingProbeIsSurfacedAsUnhealthy) {
    Core::Application app;
    app.register_health_check("flaky", []() -> bool { throw std::runtime_error("db down"); });
    auto report = app.health_report();
    ASSERT_EQ(report.size(), 1u);
    EXPECT_FALSE(report[0].healthy);
}

TEST_F(HealthRegistryTest, MultipleProbesAllEvaluated) {
    Core::Application app;
    int calls = 0;
    app.register_health_check("a", [&] {
        ++calls;
        return true;
    });
    app.register_health_check("b", [&] {
        ++calls;
        return true;
    });
    app.register_health_check("c", [&] {
        ++calls;
        return false;
    });
    auto report = app.health_report();
    EXPECT_EQ(calls, 3);
    EXPECT_EQ(report.size(), 3u);
    EXPECT_FALSE(report[2].healthy);
}
