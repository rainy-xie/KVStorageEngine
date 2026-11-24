#ifndef STORE_LOG_H
#define STORE_LOG_H

#include <cstdint>
#include <string>
#include <string_view>

namespace store_log {
using U64 = std::uint64_t;
using U32 = std::uint32_t;
constexpr unsigned file_header_size = 28;
constexpr unsigned frame_header_size = 32;
constexpr unsigned operation_header_size = 13;
constexpr std::string_view file_magic = "KVLOG002";
constexpr std::string_view frame_magic = "KVB2";

class Descriptor {
public:
    explicit Descriptor(int fd);
    ~Descriptor();
    Descriptor(const Descriptor &) = delete;
    Descriptor &operator=(const Descriptor &) = delete;
    int get() const { return fd_; }
    int release();
private:
    int fd_;
};

class TempFile {
public:
    explicit TempFile(const std::string &target);
    ~TempFile();
    TempFile(const TempFile &) = delete;
    TempFile &operator=(const TempFile &) = delete;
    std::string path;
    Descriptor fd;
};

struct FrameHeader {
    U64 length;
    U64 version;
    U32 operations;
    U32 body_crc;
};

[[noreturn]] void ioError(const char *operation);
void require(bool condition, const char *message);
void lockFile(int fd);
void syncFile(int fd);
void syncParent(const std::string &path);
void truncateFile(int fd, U64 length);
U64 fileSize(int fd);
void readAt(int fd, U64 offset, char *buffer, std::size_t size);
void writeAt(int fd, U64 offset, std::string_view bytes);

U32 checksum(std::string_view bytes);
U64 little(std::string_view bytes, std::size_t offset, unsigned width);
std::string fileHeader(U64 highwater, U64 snapshots);
void verifyFileHeader(std::string_view header);
std::string frameHeader(U64 version, U32 operations, std::string_view body);
FrameHeader parseFrameHeader(std::string_view header);
void addOperation(std::string &body, bool is_put, int key, std::string_view value);
int keyAt(std::string_view bytes, std::size_t offset);
} // namespace store_log

#endif
