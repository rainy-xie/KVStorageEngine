#include "engine.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <future>
#include <iomanip>
#include <iostream>
#include <latch>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>

namespace {
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

struct Config {
    fs::path directory;
    unsigned keys = 10'000'000;
    unsigned producers = 16;
    unsigned workers = 16;
    unsigned seconds = 43'200;
    unsigned seed = 42;
    std::optional<std::uint64_t> operations;
    bool group = true;
    std::optional<unsigned> group_delay_us;
};

std::uint64_t integer(const std::string &text, std::uint64_t maximum) {
    std::uint64_t n = 0;
    const auto end = text.data() + text.size();
    auto [p, error] = std::from_chars(text.data(), end, n);
    if (error != std::errc{} || p != end || n > maximum)
        throw std::invalid_argument("invalid integer: " + text);
    return n;
}

Config parse(int argc, char **argv) {
    Config config;
    bool duration_set = false;
    for (int i = 1; i < argc; ++i) {
        std::string opt = argv[i];
        if (opt == "--help") {
            std::cout << "stress_test [--dir NEW_DIRECTORY] [--keys N] [--producers N] [--workers N]\n"
                         "            [--seconds N | --operations N] [--mode group|single] [--group-delay-us N] [--seed N]\n"
                         "Defaults: 10,000,000 unique keys, 16 producers/16 workers, 43,200 mixed-workload seconds,\n"
                         "          group commit (200 us; single: 0 us), seed 42, new data/stress-* directory. Both modes use durable writes.\n";
            std::exit(EXIT_SUCCESS);
        }
        if (++i == argc) throw std::invalid_argument("missing value for " + opt);
        std::string val = argv[i];
        if (opt == "--dir") config.directory = val;
        else if (opt == "--keys") config.keys = integer(val, std::numeric_limits<int>::max());
        else if (opt == "--producers") config.producers = integer(val, 1024);
        else if (opt == "--workers") config.workers = integer(val, 1024);
        else if (opt == "--group-delay-us") config.group_delay_us = integer(val, std::numeric_limits<unsigned>::max());
        else if (opt == "--seed") config.seed = integer(val, std::numeric_limits<unsigned>::max());
        else if (opt == "--seconds") {
            config.seconds = integer(val, std::numeric_limits<unsigned>::max());
            duration_set = true;
        } else if (opt == "--operations") {
            config.operations = integer(val, std::numeric_limits<std::int64_t>::max());
        } else if (opt == "--mode" && (val == "group" || val == "single")) {
            config.group = val == "group";
        } else throw std::invalid_argument("unrecognized option or value: " + opt + " " + val);
    }
    if (!config.group && config.group_delay_us.value_or(0) != 0)
        throw std::invalid_argument("single mode requires --group-delay-us 0");
    if (!config.workers || !config.producers || config.keys < config.producers ||
        !config.seconds || (config.operations && (!*config.operations || duration_set)))
        throw std::invalid_argument("need keys >= producers >= 1, workers >= 1 and exactly one positive duration or operation budget");
    return config;
}

fs::path newDirectory(fs::path destination) {
    if (destination.empty()) {
        const auto stamp = std::chrono::system_clock::now().time_since_epoch().count();
        destination = fs::path("data") / ("stress-" + std::to_string(stamp) + "-" + std::to_string(::getpid()));
    }
    if (destination.has_parent_path()) fs::create_directories(destination.parent_path());
    // Atomically reserve a new directory. Never remove or open an existing database.
    if (!fs::create_directory(destination))
        throw std::runtime_error("run directory already exists: " + destination.string());
    return fs::absolute(destination);
}

std::string payload(unsigned key, std::uint64_t revision) {
    return "k" + std::to_string(key) + "-v" + std::to_string(revision);
}

template <typename Result, typename Submit>
Result waitForCallback(Submit submit) {
    // The callback owns the promise until set_value has returned on the pool worker.
    auto promise = std::make_shared<std::promise<Result>>();
    auto future = promise->get_future();
    submit([promise](Result result) { promise->set_value(std::move(result)); });
    return future.get();
}

struct WorkerStats {
    std::atomic<std::uint64_t> completed{0};
    std::uint64_t puts = 0, gets = 0, deletes = 0, missing_deletes = 0;
    // Powers of two in microseconds. Output explicitly reports the bucket's upper bound.
    std::array<std::uint64_t, 64> latency{};
};

void recordLatency(WorkerStats &stats, Clock::duration latency) {
    auto us = std::chrono::ceil<std::chrono::microseconds>(latency).count();
    std::uint64_t bound = 1;
    std::size_t bucket = 0;
    while (bound < static_cast<std::uint64_t>(us)) {
        bound <<= 1;
        ++bucket;
    }
    ++stats.latency[bucket];
    ++stats.completed;
}

template <class Work>
double runProducers(unsigned count, Work work, std::vector<WorkerStats> *stats = nullptr) {
    std::latch ready(count);
    std::promise<Clock::time_point> go;
    auto start_signal = go.get_future().share();
    std::atomic<bool> failed{false};
    std::vector<std::exception_ptr> errors(count);
    std::vector<std::thread> threads;
    std::mutex progress_lock;
    std::condition_variable progress;
    unsigned active = count;
    // Release and drain already-created threads even if OS thread creation fails.
    try {
        for (unsigned i = 0; i < count; ++i) {
            threads.emplace_back([&, i] {
                ready.count_down();
                const auto start = start_signal.get();
                try {
                    if (!failed) work(i, start, failed);
                } catch (...) {
                    errors[i] = std::current_exception();
                    failed = true;
                }
                std::lock_guard<std::mutex> lock(progress_lock);
                if (--active == 0) progress.notify_one();
            });
        }
    } catch (...) {
        failed = true;
        go.set_value(Clock::now());
        for (auto &thread : threads) thread.join();
        throw;
    }
    ready.wait();
    const auto start = Clock::now();
    go.set_value(start);
    if (stats) {
        std::unique_lock<std::mutex> lock(progress_lock);
        auto previous = start;
        std::uint64_t previous_count = 0;
        do {
            progress.wait_for(lock, 60s, [&] { return active == 0; });
            const auto now = Clock::now();
            std::uint64_t total = 0;
            for (auto &worker : *stats) total += worker.completed.load();
            const auto count_delta = total - previous_count;
            const double window_seconds = std::chrono::duration<double>(now - previous).count();
            std::cout << "[Window] elapsed_s=" << std::chrono::duration<double>(now - start).count()
                      << " window_s=" << window_seconds << " operations=" << count_delta
                      << " ops_per_sec=" << (window_seconds ? count_delta / window_seconds : 0) << std::endl;
            previous = now;
            previous_count = total;
        } while (active > 0);
    }
    for (auto &thread : threads) thread.join();
    for (auto &error : errors) if (error) std::rethrow_exception(error);
    return std::chrono::duration<double>(Clock::now() - start).count();
}

std::uint64_t p99UpperBound(const std::vector<WorkerStats> &stats, std::uint64_t count) {
    const auto rank = count - count / 100; // ceil(0.99 * count)
    std::uint64_t hits = 0;
    for (unsigned i = 0; i < 64; ++i) {
        for (const auto &worker : stats) hits += worker.latency[i];
        if (hits >= rank) return std::uint64_t{1} << i;
    }
    throw std::logic_error("incomplete latency histogram");
}

int run(const Config &config) {
    const auto directory = newDirectory(config.directory);
    const auto db = (directory / "store.dat").string();
    CommitOptions options;
    options.durable = true;
    options.max_group_size = config.group ? 32 : 1;
    options.group_delay = std::chrono::microseconds(config.group_delay_us.value_or(config.group ? 200u : 0u));

    // Producer i owns keys i, i+producers, ... throughout both phases.
    // Per-key expected revisions can be read/written without a second concurrent oracle.
    std::vector<std::uint64_t> revisions(config.keys);
    std::cout << std::fixed << std::setprecision(3)
              << "run_dir=" << directory << " mode=" << (config.group ? "group" : "single")
              << " durable=true keys=" << config.keys << " producers=" << config.producers
              << " workers=" << config.workers << " seed=" << config.seed
              << " max_group_size=" << options.max_group_size << " group_delay_us=" << options.group_delay.count()
              << (config.operations ? " requested_operations=" : " requested_seconds=")
              << (config.operations ? *config.operations : config.seconds) << std::endl;
    {
        StorageEngine engine(db, config.workers, 200'000, 32, options);
        auto before = engine.commitStats();
        auto fill_seconds = runProducers(config.producers, [&](unsigned id, Clock::time_point, auto &failed) {
            constexpr std::size_t batch_limit = 256;
            std::vector<Mutation> batch;
            batch.reserve(batch_limit);
            for (std::uint64_t key = id; key < config.keys && !failed; key += config.producers) {
                batch.push_back(Mutation::put(key, payload(key, 1)));
                if (batch.size() == batch_limit || key + config.producers >= config.keys) {
                    if (engine.writeBatch(batch) != Status::Ok)
                        throw std::runtime_error("prefill batch failed near key " + std::to_string(key));
                    batch.clear();
                }
                revisions[key] = 1;
            }
        });
        auto filled = engine.commitStats();
        std::cout << "phase=prefill batch_limit=256 distinct_inserted=" << config.keys << " seconds=" << fill_seconds
                  << " sync_calls=" << filled.sync_calls - before.sync_calls << std::endl;

        std::vector<WorkerStats> stats(config.producers);
        auto elapsed = runProducers(config.producers, [&](unsigned id, auto start, auto &failed) {
            std::seed_seq seed{config.seed, id};
            std::mt19937 rng(seed);
            std::uniform_int_distribution<unsigned> slot(0, (config.keys - 1 - id) / config.producers);
            std::uniform_int_distribution<unsigned> kind(0, 2);
            const auto budget = config.operations
                ? *config.operations / config.producers + (id < *config.operations % config.producers)
                : std::numeric_limits<std::uint64_t>::max();
            const auto deadline = start + std::chrono::seconds(config.seconds);
            auto &worker = stats[id];
            for (std::uint64_t i = 0; i < budget && !failed; ++i) {
                if (!config.operations && Clock::now() >= deadline) break;
                const unsigned key = id + slot(rng) * config.producers;
                auto &expected = revisions[key];
                bool matches = false;
                const auto op = kind(rng);
                const auto begin = Clock::now();
                if (op == 0) {
                    const auto revision = i + 2;
                    matches = waitForCallback<bool>([&](auto callback) {
                        engine.asyncPut(key, payload(key, revision), std::move(callback));
                    });
                    if (matches) expected = revision;
                    ++worker.puts;
                } else if (op == 1) {
                    auto value = waitForCallback<std::string>([&](auto callback) {
                        engine.asyncGet(key, std::move(callback));
                    });
                    matches = value == (expected ? payload(key, expected) : std::string{});
                    ++worker.gets;
                } else {
                    const bool removed = waitForCallback<bool>([&](auto callback) {
                        engine.asyncDel(key, std::move(callback));
                    });
                    matches = removed == (expected != 0);
                    if (!expected) ++worker.missing_deletes;
                    if (removed) expected = 0;
                    ++worker.deletes;
                }
                recordLatency(worker, Clock::now() - begin);
                if (!matches)
                    throw std::runtime_error("mixed operation " + std::to_string(op) +
                                             " disagreed with oracle at key " + std::to_string(key));
            }
        }, &stats);
        std::uint64_t puts = 0, gets = 0, dels = 0, missing_dels = 0;
        for (auto &worker : stats) {
            puts += worker.puts;
            gets += worker.gets;
            dels += worker.deletes;
            missing_dels += worker.missing_deletes;
        }
        const auto operations = puts + gets + dels;
        const auto after = engine.commitStats();
        const auto commits = after.committed_requests - filled.committed_requests;
        const auto syncs = after.sync_calls - filled.sync_calls;
        if (commits != puts + dels - missing_dels || syncs > commits ||
            (commits && !syncs) || (!config.group && syncs != commits))
            throw std::runtime_error("commit or durability counters disagree with acknowledged operations");
        std::cout << "phase=mixed operations=" << operations << " seconds=" << elapsed
                  << " ops_per_sec=" << operations / elapsed
                  << " p99_us_le=" << p99UpperBound(stats, operations)
                  << " puts=" << puts << " gets=" << gets << " deletes=" << dels
                  << " deletes_already_absent=" << missing_dels
                  << " committed_requests=" << commits
                  << " sync_calls=" << syncs << std::endl;
    }
    StorageEngine reopened(db, 1, 0, 1, options);
    std::uint64_t live = 0;
    for (unsigned key = 0; key < config.keys; ++key) {
        auto result = reopened.read(key);
        if (revisions[key]) {
            if (result.status != Status::Ok || result.value != payload(key, revisions[key]))
                throw std::runtime_error("reopened value mismatch at key " + std::to_string(key));
            ++live;
        } else if (result.status != Status::NotFound)
            throw std::runtime_error("deleted key present or unreadable on reopen: " + std::to_string(key));
    }
    std::cout << "verification=passed reopened_keys=" << config.keys << " live=" << live
              << " deleted=" << config.keys - live << "\n";
    return EXIT_SUCCESS;
}
} // namespace

int main(int argc, char **argv) {
    try {
        return run(parse(argc, argv));
    } catch (const std::exception &error) {
        std::cerr << "stress_test: " << error.what() << std::endl;
        return EXIT_FAILURE;
    }
}
