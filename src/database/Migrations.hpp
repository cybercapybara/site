/**
 * @file Migrations.hpp
 * @brief SQL migration runner module
 * @details Scans migrations directory for .sql files, tracks applied migrations
 *          in a schema_migrations table, and applies new ones on startup
 */

#pragma once

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <memory>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <spdlog/spdlog.h>

#include "database/Database.hpp"

namespace Migrations {

namespace fs = std::filesystem;

struct MigrationFile {
    int version;
    std::string name;
    fs::path path;
};

/**
 * @brief Migration runner that applies SQL migrations on startup
 */
class MigrationRunner {
public:
    /**
     * @brief Scan a migrations directory and report which files haven't yet
     *        been applied to the database. Read-only — does not create the
     *        tracking table, does not apply anything. Caller must have
     *        initialized Database. Returns the sorted pending list by version.
     */
    static std::vector<MigrationFile> list_pending(const std::string& dir) {
        MigrationRunner scanner;
        scanner.migrations_dir = dir;
        auto on_disk = scanner.scan_migrations();
        std::set<int> applied;
        try {
            // Read from the primary: --verify-migrations must reflect what has
            // actually been applied, and migrations are written on the primary.
            // A lagging replica would report false-pending (or false-green).
            auto result = Database::get().execute_read_primary(
                [](auto& txn) { return txn.exec("SELECT version FROM schema_migrations ORDER BY version"); });
            for (const auto& row : result) {
                applied.insert(row[0].template as<int>());
            }
        } catch (const std::exception& e) {
            // No tracking table yet → every on-disk migration counts as pending.
            spdlog::debug("list_pending: schema_migrations not readable ({}), treating all as pending", e.what());
        }
        std::vector<MigrationFile> pending;
        for (auto& m : on_disk) {
            if (applied.count(m.version) == 0)
                pending.push_back(std::move(m));
        }
        return pending;
    }

private:
    bool initialized = false;
    std::string migrations_dir;

    void ensure_tracking_table() {
        Database::get().execute_write([](auto& txn) {
            txn.exec(
                "CREATE TABLE IF NOT EXISTS schema_migrations ("
                "  version INTEGER PRIMARY KEY,"
                "  name VARCHAR(255) NOT NULL,"
                "  applied_at TIMESTAMP WITH TIME ZONE DEFAULT CURRENT_TIMESTAMP"
                ")");
            return 0;
        });
        spdlog::debug("schema_migrations table ensured");
    }

    std::vector<MigrationFile> scan_migrations() {
        std::vector<MigrationFile> migrations;
        std::regex pattern(R"(^(\d+)[_-].*\.sql$)");

        if (!fs::exists(migrations_dir) || !fs::is_directory(migrations_dir)) {
            spdlog::warn("Migrations directory '{}' does not exist, skipping", migrations_dir);
            return migrations;
        }

        for (const auto& entry : fs::directory_iterator(migrations_dir)) {
            if (!entry.is_regular_file())
                continue;

            std::string filename = entry.path().filename().string();
            std::smatch match;
            if (std::regex_match(filename, match, pattern)) {
                MigrationFile mf;
                mf.version = std::stoi(match[1].str());
                mf.name = filename;
                mf.path = entry.path();
                migrations.push_back(mf);
            }
        }

        std::sort(migrations.begin(), migrations.end(), [](const MigrationFile& a, const MigrationFile& b) {
            return a.version < b.version;
        });

        return migrations;
    }

    std::set<int> get_applied() {
        std::set<int> applied;
        // Read-only SELECT — use a read txn on the primary (not a write slot),
        // consistent with list_pending. Must be the primary: migrations are
        // written there and a replica could lag.
        auto result = Database::get().execute_read_primary(
            [](auto& txn) { return txn.exec("SELECT version FROM schema_migrations ORDER BY version"); });
        for (const auto& row : result) {
            applied.insert(row[0].template as<int>());
        }
        return applied;
    }

    std::string read_file(const fs::path& path) {
        std::ifstream file(path);
        if (!file.is_open()) {
            throw std::runtime_error("Cannot open migration file: " + path.string());
        }
        std::stringstream buffer;
        buffer << file.rdbuf();
        return buffer.str();
    }

public:
    void initialize(const std::string& dir) {
        if (initialized) {
            throw std::runtime_error("Migration runner already initialized");
        }

        migrations_dir = dir;
        spdlog::info("Running database migrations from '{}'", migrations_dir);

        ensure_tracking_table();

        auto migrations = scan_migrations();
        if (migrations.empty()) {
            spdlog::info("No migration files found in '{}'", migrations_dir);
            initialized = true;
            return;
        }

        auto applied = get_applied();
        int applied_count = 0;
        int skipped_count = 0;

        for (const auto& mf : migrations) {
            if (applied.count(mf.version)) {
                skipped_count++;
                continue;
            }

            spdlog::info("Running migration {}: {}", mf.version, mf.name);
            std::string sql = read_file(mf.path);

            // Hold a transaction-scoped advisory lock while applying so
            // concurrent boots (multiple replicas) serialize here instead of
            // both running the DDL and one crashing on the schema_migrations
            // PK conflict. Re-check applied-state INSIDE the lock: the loser
            // of the race finds the row already present and skips the DDL.
            const bool did_apply = Database::get().execute_write([&](auto& txn) -> bool {
                txn.exec("SELECT pg_advisory_xact_lock(4242424242)");
                auto seen = txn.exec_params("SELECT 1 FROM schema_migrations WHERE version = $1", mf.version);
                if (!seen.empty())
                    return false;  // another booting instance applied it first
                txn.exec(sql);
                txn.exec_params("INSERT INTO schema_migrations (version, name) VALUES ($1, $2)", mf.version, mf.name);
                return true;
            });

            if (did_apply) {
                spdlog::info("Migration {} applied successfully", mf.name);
                applied_count++;
            } else {
                spdlog::info("Migration {} applied concurrently by another instance — skipped", mf.name);
                skipped_count++;
            }
        }

        spdlog::info("Migrations complete: {} applied, {} already up-to-date", applied_count, skipped_count);
        initialized = true;
    }

    void shutdown() {
        if (initialized) {
            spdlog::debug("Migration runner shut down");
            initialized = false;
        }
    }

    bool is_initialized() const { return initialized; }
};

/**
 * @brief Global migration runner instance
 */
inline std::unique_ptr<MigrationRunner> global_runner = nullptr;

inline void initialize(const std::string& migrations_dir) {
    if (global_runner != nullptr) {
        throw std::runtime_error("Migration runner already initialized");
    }
    global_runner = std::make_unique<MigrationRunner>();
    global_runner->initialize(migrations_dir);
}

inline MigrationRunner& get() {
    if (global_runner == nullptr) {
        throw std::runtime_error("Migration runner not initialized");
    }
    return *global_runner;
}

inline bool is_initialized() {
    return global_runner != nullptr && global_runner->is_initialized();
}

inline void shutdown() {
    if (global_runner) {
        global_runner->shutdown();
        global_runner.reset();
    }
}

}  // namespace Migrations
