#include "engine.h"

#include <gtest/gtest.h>

#include <array>
#include <barrier>
#include <csignal>
#include <sys/resource.h>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <latch>
#include <stdexcept>
#include <thread>
#include <vector>
#include <sys/wait.h>
#include <unistd.h>

namespace {
namespace fs = std::filesystem;
using namespace std::chrono_literals;

class KVv2Test : public ::testing::Test {
protected:
    void SetUp() override {
        std::string pattern = (fs::temp_directory_path() / "kv-v2-XXXXXX").string();
        ASSERT_NE(::mkdtemp(pattern.data()), nullptr);
        directory = pattern;
        db = (directory / "store.dat").string();
    }

    void TearDown() override {
        if (!directory.empty()) {
            std::error_code error;
            fs::remove_all(directory, error);
            EXPECT_FALSE(error) << error.message();
        }
    }

    fs::path directory;
    std::string db;
};

TEST_F(KVv2Test, RecoversAcknowledgedBatchAfterAbruptExit) {
    const pid_t child = ::fork();
    ASSERT_NE(child, -1);
    if (child == 0) {
        // Skip every destructor, including the storage engine's clean shutdown.
        try {
            StorageEngine engine(db, 1, 0, 1);
            const auto result = engine.writeBatch({Mutation::put(1, "left"),
                                                    Mutation::put(2, "right")});
            ::_exit(result == Status::Ok ? 0 : 1);
        } catch (...) {
            ::_exit(2);
        }
    }
    int status = 0;
    ASSERT_EQ(::waitpid(child, &status, 0), child);
    ASSERT_TRUE(WIFEXITED(status));
    ASSERT_EQ(WEXITSTATUS(status), 0);

    StorageEngine reopened(db, 1, 0, 1);
    const auto values = reopened.multiGet({1, 2});
    ASSERT_EQ(values.size(), 2u);
    EXPECT_EQ(values[0].value, "left");
    EXPECT_EQ(values[1].value, "right");
}

TEST_F(KVv2Test, DropsWholeTornBatchAndAcceptsLaterWrites) {
    std::uintmax_t original_size;
    {
        StorageEngine engine(db, 1, 0, 1);
        ASSERT_EQ(engine.writeBatch({Mutation::put(1, "original"), Mutation::put(2, "keep")}), Status::Ok);
        original_size = fs::file_size(db);
        ASSERT_EQ(engine.writeBatch({Mutation::put(1, "partial"), Mutation::del(2),
                                     Mutation::put(3, "incomplete")}), Status::Ok);
    }
    ASSERT_GT(fs::file_size(db), original_size);
    fs::resize_file(db, fs::file_size(db) - 1);
    {
        StorageEngine recovered(db, 1, 0, 1);
        EXPECT_EQ(fs::file_size(db), original_size);
        EXPECT_EQ(recovered.get(1), "original");
        EXPECT_EQ(recovered.get(2), "keep");
        EXPECT_EQ(recovered.read(3).status, Status::NotFound);
        ASSERT_TRUE(recovered.put(4, "after recovery"));
    }
    StorageEngine reopened(db, 1, 0, 1);
    EXPECT_EQ(reopened.get(4), "after recovery");
}

TEST_F(KVv2Test, RejectsCorruptedCompleteRecordWithoutTruncatingIt) {
    {
        StorageEngine engine(db, 1, 0, 1);
        ASSERT_TRUE(engine.put(1, "payload"));
    }
    auto size = fs::file_size(db);
    std::fstream file(db, std::ios::in | std::ios::out | std::ios::binary);
    ASSERT_TRUE(file);
    file.seekg(-1, std::ios::end);
    char byte;
    ASSERT_TRUE(file.get(byte));
    file.seekp(-1, std::ios::end);
    ASSERT_TRUE(file.put(static_cast<char>(byte ^ 1)));
    file.close();
    EXPECT_THROW({ StorageEngine damaged(db, 1, 0, 1); }, std::runtime_error);
    EXPECT_EQ(fs::file_size(db), size);
}

TEST_F(KVv2Test, CompareAndSwapHasOneWinnerAndOldVersionsExpire) {
    std::uint64_t version_before_delete;
    std::uint64_t version_after_create;
    {
        StorageEngine engine(db, 4, 32, 4);
        ASSERT_EQ(engine.read(1).version, 0u);
        ASSERT_EQ(engine.putIfVersion(1, 0, "seed"), Status::Ok);
        const auto seed = engine.read(1).version;
        ASSERT_NE(seed, 0u);
        constexpr int count = 8;
        std::latch start(count);
        std::array<Status, count> results{};
        std::vector<std::thread> writers;
        for (int i = 0; i < count; ++i) {
            writers.emplace_back([&, i] {
                start.arrive_and_wait();
                results[i] = engine.putIfVersion(1, seed, std::to_string(i));
            });
        }
        for (auto &writer : writers) writer.join();
        int wins = 0;
        for (int i = 0; i < count; ++i) {
            if (results[i] == Status::Ok) {
                ++wins;
                EXPECT_EQ(engine.get(1), std::to_string(i));
            } else {
                EXPECT_EQ(results[i], Status::Conflict);
            }
        }
        EXPECT_EQ(wins, 1);
        version_before_delete = engine.read(1).version;
        ASSERT_TRUE(engine.del(1));
        ASSERT_EQ(engine.putIfVersion(1, 0, "new"), Status::Ok);
        version_after_create = engine.read(1).version;
        EXPECT_NE(version_before_delete, version_after_create);
        engine.garbageCollect();
        EXPECT_EQ(engine.read(1).version, version_after_create);
    }
    StorageEngine reopened(db, 1, 0, 1);
    EXPECT_EQ(reopened.read(1).version, version_after_create);
    EXPECT_EQ(reopened.putIfVersion(1, version_before_delete, "stale"), Status::Conflict);
    EXPECT_EQ(reopened.putIfVersion(1, version_after_create, "current"), Status::Ok);
}

TEST_F(KVv2Test, MultiGetReadsOneCompleteBatch) {
    CommitOptions options;
    options.durable = false;
    StorageEngine engine(db, 2, 8, 2, options);
    ASSERT_EQ(engine.writeBatch({Mutation::put(1, "0"), Mutation::put(2, "0")}), Status::Ok);
    std::latch start(2);
    std::atomic<bool> finished{false};
    std::thread writer([&] {
        start.arrive_and_wait();
        for (int i = 1; i <= 500; ++i) {
            auto value = std::to_string(i);
            EXPECT_EQ(engine.writeBatch({Mutation::put(1, value), Mutation::put(2, value)}), Status::Ok);
        }
        finished = true;
    });
    start.arrive_and_wait();
    do {
        // Duplicate keys and a missing key also check ordering and cardinality.
        const auto snapshot = engine.multiGet({2, 9, 1, 2});
        EXPECT_EQ(snapshot.size(), 4u);
        if (snapshot.size() != 4) break;
        EXPECT_EQ(snapshot[0].status, Status::Ok);
        EXPECT_EQ(snapshot[1].status, Status::NotFound);
        EXPECT_EQ(snapshot[2].status, Status::Ok);
        EXPECT_EQ(snapshot[0].value, snapshot[2].value);
        EXPECT_EQ(snapshot[0].value, snapshot[3].value);
    } while (!finished.load());
    writer.join();
    EXPECT_EQ(engine.get(1), "500");
}

TEST_F(KVv2Test, GarbageCollectionPreservesOnlyLiveValuesOnReopen) {
    {
        StorageEngine engine(db, 1, 0, 1);
        ASSERT_TRUE(engine.put(1, std::string(1024, 'x')));
        ASSERT_EQ(engine.writeBatch({Mutation::put(1, "short"), Mutation::put(2, "old")}), Status::Ok);
        ASSERT_TRUE(engine.del(2));
        const auto old_size = fs::file_size(db);
        engine.garbageCollect();
        EXPECT_LT(fs::file_size(db), old_size);
        EXPECT_EQ(engine.get(1), "short");
        EXPECT_EQ(engine.read(2).status, Status::NotFound);
    }
    StorageEngine reopened(db, 1, 0, 1);
    EXPECT_EQ(reopened.get(1), "short");
    EXPECT_EQ(reopened.read(2).status, Status::NotFound);
}

TEST_F(KVv2Test, GroupCommitSharesDurableSyncsAcrossRequests) {
    constexpr unsigned count = 12;
    for (const auto group_size : {1u, count}) {
        CommitOptions options;
        options.durable = true;
        options.max_group_size = group_size;
        options.group_delay = group_size == 1 ? 0ms : 50ms;
        StorageEngine engine(db + std::to_string(group_size), 1, 0, 1, options);
        const auto before = engine.commitStats();
        std::latch start(count);
        std::vector<std::thread> threads;
        for (unsigned i = 0; i < count; ++i) {
            threads.emplace_back([&, i] {
                start.arrive_and_wait();
                EXPECT_TRUE(engine.put(static_cast<int>(i), "v"));
            });
        }
        for (auto &thread : threads) thread.join();
        const auto after = engine.commitStats();
        EXPECT_EQ(after.committed_requests - before.committed_requests, count);
        const auto syncs = after.sync_calls - before.sync_calls;
        EXPECT_GT(syncs, 0u);
        if (group_size == 1) EXPECT_EQ(syncs, count);
        else EXPECT_LT(syncs, count);
    }
}

TEST_F(KVv2Test, UncachedReadsDistinguishEmptyMissingAndBinaryValues) {
    StorageEngine engine(db, 1, 0, 4);
    const std::string binary("a\0b", 3);
    ASSERT_TRUE(engine.put(1, ""));
    ASSERT_TRUE(engine.put(2, binary));
    auto before = engine.getFileStoreReadCount();
    auto empty = engine.read(1);
    EXPECT_EQ(empty.status, Status::Ok);
    EXPECT_TRUE(empty.value.empty());
    EXPECT_NE(empty.version, 0u);
    EXPECT_EQ(engine.read(3).status, Status::NotFound);
    EXPECT_EQ(engine.read(2).value, binary);
    EXPECT_EQ(engine.read(2).value, binary);
    EXPECT_GE(engine.getFileStoreReadCount() - before, 2u);
}

TEST_F(KVv2Test, StoppedEngineCompletesRejectedCallbacks) {
    StorageEngine engine(db, 1, 8, 1);
    engine.stop();
    int callbacks = 0;
    engine.asyncPut(1, "v", [&](bool ok) { EXPECT_FALSE(ok); ++callbacks; });
    engine.asyncDel(1, [&](bool ok) { EXPECT_FALSE(ok); ++callbacks; });
    engine.asyncGet(1, [&](std::string value) { EXPECT_TRUE(value.empty()); ++callbacks; });
    EXPECT_EQ(callbacks, 3);
}

TEST_F(KVv2Test, ShutdownDrainsAcceptedRequestsBeforeClosingTheFile) {
    constexpr int count = 64;
    std::atomic<int> completed{0};
    {
        StorageEngine engine(db, 4);
        for (int key = 0; key < count; ++key) {
            engine.asyncPut(key, std::to_string(key), [&](bool ok) {
                EXPECT_TRUE(ok);
                ++completed;
            });
        }
        engine.stop();
    }
    EXPECT_EQ(completed.load(), count);
    StorageEngine reopened(db);
    for (int key = 0; key < count; ++key) {
        EXPECT_EQ(reopened.get(key), std::to_string(key));
    }
}

TEST_F(KVv2Test, MigratesNativeLegacyIndexToSeparateDestination) {
    const auto source = (directory / "legacy.dat").string();
    { std::ofstream data(source, std::ios::binary); data << "oldkeep"; }
    // Historical Linux x86-64 ABI: uint64 count, then 32-byte little-endian entries.
    std::vector<unsigned char> index(8 + 2 * 32, 0);
    index[0] = 2;
    index[8] = 1;       // Key 1, offset 0, size 3, deleted.
    index[8 + 16] = 3;
    index[8 + 24] = 1;
    index[40] = 2;      // Key 2, offset 3, size 4, live.
    index[40 + 8] = 3;
    index[40 + 16] = 4;
    {
        std::ofstream out(source + ".idx", std::ios::binary);
        out.write(reinterpret_cast<const char *>(index.data()), index.size());
    }
    ASSERT_NO_THROW(FileStore::migrateLegacy(source, db));
    StorageEngine migrated(db, 1, 0, 1);
    EXPECT_EQ(migrated.read(1).status, Status::NotFound);
    EXPECT_EQ(migrated.get(2), "keep");
    EXPECT_NE(migrated.read(2).version, 0u);
    EXPECT_ANY_THROW(FileStore::migrateLegacy(source, db));
    EXPECT_EQ(fs::file_size(source), 7u);
}
TEST_F(KVv2Test, LegacyDatabaseCanContainAnEntireV2FileAsItsValue) {
    const auto inner = (directory / "inner.dat").string();
    {
        StorageEngine nested(inner);
        ASSERT_TRUE(nested.put(99, "inside the value"));
    }
    // v1 stored value bytes verbatim, so file magic alone is ambiguous.
    fs::copy_file(inner, db);
    const std::uint64_t length = fs::file_size(inner);
    std::array<char, 40> old_index{};
    old_index[0] = 1;  // count
    old_index[8] = 7;  // v1 object's key
    for (unsigned i = 0; i < 8; ++i) {
        old_index[24 + i] = static_cast<char>(length >> (8 * i));
    }
    std::ofstream(db + ".idx", std::ios::binary).write(old_index.data(), old_index.size());
    EXPECT_ANY_THROW({ StorageEngine ambiguous(db); });
    const auto destination = (directory / "migrated.dat").string();
    ASSERT_NO_THROW(FileStore::migrateLegacy(db, destination));
    StorageEngine migrated(destination);
    EXPECT_EQ(migrated.read(7).value.size(), length);
    EXPECT_EQ(migrated.read(99).status, Status::NotFound);
}

TEST_F(KVv2Test, EmptyCheckpointDoesNotReuseDeletedVersion) {
    std::uint64_t old_version;
    {
        StorageEngine engine(db);
        ASSERT_EQ(engine.putIfVersion(9, 0, ""), Status::Ok);
        old_version = engine.read(9).version;
        ASSERT_TRUE(engine.del(9));
        engine.garbageCollect();
    }
    StorageEngine reopened(db);
    EXPECT_EQ(reopened.read(9).version, 0u);
    ASSERT_EQ(reopened.putIfVersion(9, 0, "again"), Status::Ok);
    EXPECT_GT(reopened.read(9).version, old_version);
    EXPECT_EQ(reopened.putIfVersion(9, old_version, "outdated"), Status::Conflict);
}

TEST_F(KVv2Test, InvalidBatchDoesNotPublishItsPrefixOrConsumeASync) {
    StorageEngine engine(db);
    auto stats = engine.commitStats();
    EXPECT_EQ(engine.writeBatch({Mutation::put(9, "partial"),
                                 {Mutation::Kind::Delete, 9, "invalid payload"}}),
              Status::InvalidArgument);
    EXPECT_EQ(engine.writeBatch({}), Status::Ok);
    EXPECT_EQ(engine.read(9).status, Status::NotFound);
    EXPECT_EQ(engine.commitStats().sync_calls, stats.sync_calls);
    EXPECT_EQ(engine.writeBatch({Mutation::put(9, "first"), Mutation::del(9),
                                 Mutation::del(9), Mutation::put(9, "last")}), Status::Ok);
    EXPECT_EQ(engine.get(9), "last");
}

TEST_F(KVv2Test, ColdReadRacingWithDeleteNeverResurrectsRemovedKey) {
    CommitOptions options;
    options.durable = false;
    StorageEngine engine(db, 1, 1, 1, options);
    const std::string payload(8192, 'v');
    std::barrier phase(2);
    constexpr unsigned rounds = 96;
    std::thread reader([&] {
        for (unsigned i = 0; i < rounds; ++i) {
            phase.arrive_and_wait();
            auto value = engine.read(1);
            EXPECT_TRUE(value.status == Status::NotFound ||
                        (value.status == Status::Ok && value.value == payload));
            phase.arrive_and_wait();
        }
    });
    for (unsigned i = 0; i < rounds; ++i) {
        EXPECT_TRUE(engine.put(1, payload));
        EXPECT_TRUE(engine.put(2, "evict key 1"));
        phase.arrive_and_wait();
        EXPECT_TRUE(engine.del(1));
        phase.arrive_and_wait();
        EXPECT_EQ(engine.read(1).status, Status::NotFound);
    }
    reader.join();
}

TEST_F(KVv2Test, FailedDiskWriteDoesNotAcknowledgeAPartialGroup) {
    // Limit only a child process so the rest of the suite can write normally.
    const pid_t child = ::fork();
    ASSERT_NE(child, -1);
    if (child == 0) {
        try {
            StorageEngine engine(db);
            if (!engine.put(1, "confirmed")) ::_exit(1);
            struct rlimit limit{};
            if (::getrlimit(RLIMIT_FSIZE, &limit) != 0) ::_exit(2);
            limit.rlim_cur = fs::file_size(db) + 3;
            if (std::signal(SIGXFSZ, SIG_IGN) == SIG_ERR ||
                ::setrlimit(RLIMIT_FSIZE, &limit) != 0) ::_exit(3);
            if (engine.writeBatch({Mutation::put(1, "partial"), Mutation::put(2, "new")})
                != Status::IOError) ::_exit(4);
            if (engine.read(1).status != Status::IOError) ::_exit(5);
            ::_exit(0);
        } catch (...) {
            ::_exit(6);
        }
    }
    int status;
    ASSERT_EQ(::waitpid(child, &status, 0), child);
    ASSERT_TRUE(WIFEXITED(status));
    ASSERT_EQ(WEXITSTATUS(status), 0);
    StorageEngine reopened(db);
    EXPECT_EQ(reopened.get(1), "confirmed");
    EXPECT_EQ(reopened.read(2).status, Status::NotFound);
}
} // namespace
