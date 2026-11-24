#ifndef KV_TYPES_H
#define KV_TYPES_H

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

enum class Status { Ok, NotFound, Conflict, IOError, InvalidArgument };

struct ReadResult {
    Status status = Status::NotFound;
    std::string value;
    std::uint64_t version = 0;
};

struct Mutation {
    enum class Kind { Put, Delete };
    Kind kind;
    int key;
    std::string value;

    static Mutation put(int key, std::string value) {
        return {Kind::Put, key, std::move(value)};
    }
    static Mutation del(int key) { return {Kind::Delete, key, {}}; }
};

struct CommitOptions {
    bool durable = true;
    std::size_t max_group_size = 32;
    std::chrono::microseconds group_delay{0};
};

struct CommitStats {
    std::uint64_t committed_requests = 0;
    std::uint64_t sync_calls = 0;
};

#endif
