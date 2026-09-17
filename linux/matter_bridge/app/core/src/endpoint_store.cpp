#include "matter_bridge/endpoint_store.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

extern "C" {
#include "pogo_proto/pogo_crc16.h"
}

namespace matter_bridge {

namespace {
constexpr char kMagic[4] = {'P', 'M', 'E', 'P'};
constexpr uint8_t kFormatVersion = 1;
constexpr size_t kHeaderLen = 4 + 1 + 3 + 4; // magic, version, reserved, entry_count
constexpr size_t kEntryLen = 8 + 2;          // uuid[8], endpoint_id (u16 BE)
constexpr size_t kCrcLen = 2;

void put_u32_be(std::vector<uint8_t> &out, uint32_t v)
{
    out.push_back(static_cast<uint8_t>(v >> 24));
    out.push_back(static_cast<uint8_t>(v >> 16));
    out.push_back(static_cast<uint8_t>(v >> 8));
    out.push_back(static_cast<uint8_t>(v));
}

void put_u16_be(std::vector<uint8_t> &out, uint16_t v)
{
    out.push_back(static_cast<uint8_t>(v >> 8));
    out.push_back(static_cast<uint8_t>(v));
}

uint32_t get_u32_be(const uint8_t *p)
{
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) | (static_cast<uint32_t>(p[2]) << 8) |
           static_cast<uint32_t>(p[3]);
}

uint16_t get_u16_be(const uint8_t *p)
{
    return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | static_cast<uint16_t>(p[1]));
}
} // namespace

EndpointStore::EndpointStore(std::string state_dir) : state_dir_(std::move(state_dir)) {}

std::string EndpointStore::path() const
{
    return state_dir_ + "/endpoint_map.bin";
}

LoadResult EndpointStore::load()
{
    std::lock_guard<std::mutex> lock(mutex_);
    entries_.clear();
    loaded_ = true;

    FILE *f = std::fopen(path().c_str(), "rb");
    if (f == nullptr) {
        return LoadResult::NotFound;
    }

    std::vector<uint8_t> buf;
    uint8_t chunk[4096];
    size_t n;
    while ((n = std::fread(chunk, 1, sizeof(chunk), f)) > 0) {
        buf.insert(buf.end(), chunk, chunk + n);
    }
    bool read_error = std::ferror(f) != 0;
    std::fclose(f);
    if (read_error) {
        return LoadResult::Corrupt;
    }

    if (buf.size() < kHeaderLen + kCrcLen) {
        return LoadResult::Corrupt;
    }
    if (std::memcmp(buf.data(), kMagic, sizeof(kMagic)) != 0) {
        return LoadResult::Corrupt;
    }
    if (buf[4] != kFormatVersion) {
        return LoadResult::Corrupt;
    }
    uint32_t entry_count = get_u32_be(&buf[8]);
    size_t expected_len = kHeaderLen + static_cast<size_t>(entry_count) * kEntryLen + kCrcLen;
    if (entry_count > kMaxEndpoints || buf.size() != expected_len) {
        return LoadResult::Corrupt;
    }

    uint16_t stored_crc = get_u16_be(&buf[buf.size() - kCrcLen]);
    uint16_t computed_crc = pogo_crc16_ccitt(buf.data(), buf.size() - kCrcLen);
    if (stored_crc != computed_crc) {
        return LoadResult::Corrupt;
    }

    entries_.reserve(entry_count);
    for (uint32_t i = 0; i < entry_count; i++) {
        const uint8_t *p = &buf[kHeaderLen + static_cast<size_t>(i) * kEntryLen];
        Entry e;
        std::memcpy(e.uuid.data(), p, 8);
        e.endpoint_id = get_u16_be(p + 8);
        entries_.push_back(e);
    }

    // Defensive re-validation of invariants the write side always upholds
    // (unique uuids, unique endpoint ids, ids within the valid dynamic
    // range) -- a file that passed its CRC but violates these could only
    // come from a version of this program with a bug, never from disk bit
    // rot (that's what the CRC already catches); treat it exactly the same
    // as a corrupt file rather than loading state this program's own
    // invariants say can't be trusted.
    for (size_t i = 0; i < entries_.size(); i++) {
        if (entries_[i].endpoint_id < kFirstDynamicEndpointId) {
            return LoadResult::Corrupt;
        }
        for (size_t j = i + 1; j < entries_.size(); j++) {
            if (entries_[i].uuid == entries_[j].uuid || entries_[i].endpoint_id == entries_[j].endpoint_id) {
                return LoadResult::Corrupt;
            }
        }
    }

    return LoadResult::Ok;
}

uint16_t EndpointStore::next_free_endpoint_id_locked() const
{
    uint16_t candidate = kFirstDynamicEndpointId;
    for (;;) {
        bool used = false;
        for (const auto &e : entries_) {
            if (e.endpoint_id == candidate) {
                used = true;
                break;
            }
        }
        if (!used) {
            return candidate;
        }
        candidate++;
    }
}

bool EndpointStore::persist_locked() const
{
    std::vector<uint8_t> buf;
    buf.reserve(kHeaderLen + entries_.size() * kEntryLen + kCrcLen);
    buf.insert(buf.end(), kMagic, kMagic + sizeof(kMagic));
    buf.push_back(kFormatVersion);
    buf.push_back(0);
    buf.push_back(0);
    buf.push_back(0);
    put_u32_be(buf, static_cast<uint32_t>(entries_.size()));
    for (const auto &e : entries_) {
        buf.insert(buf.end(), e.uuid.begin(), e.uuid.end());
        put_u16_be(buf, e.endpoint_id);
    }
    uint16_t crc = pogo_crc16_ccitt(buf.data(), buf.size());
    put_u16_be(buf, crc);

    std::string final_path = path();
    std::string tmp_path = final_path + ".tmp";

    int fd = ::open(tmp_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        return false;
    }
    const uint8_t *data = buf.data();
    size_t remaining = buf.size();
    while (remaining > 0) {
        ssize_t written = ::write(fd, data, remaining);
        if (written < 0) {
            ::close(fd);
            return false;
        }
        data += written;
        remaining -= static_cast<size_t>(written);
    }
    if (::fsync(fd) != 0) {
        ::close(fd);
        return false;
    }
    if (::close(fd) != 0) {
        return false;
    }
    if (::rename(tmp_path.c_str(), final_path.c_str()) != 0) {
        return false;
    }

    // Fsync the containing directory too -- rename() is atomic w.r.t. a
    // concurrent reader, but the directory ENTRY change itself is not
    // guaranteed durable across a crash until the directory's own fd is
    // fsync'd (POSIX). Without this, a crash right after rename() could
    // still lose the rename on some filesystems/power-loss scenarios.
    int dir_fd = ::open(state_dir_.c_str(), O_RDONLY);
    if (dir_fd < 0) {
        return false;
    }
    bool dir_sync_ok = ::fsync(dir_fd) == 0;
    ::close(dir_fd);
    return dir_sync_ok;
}

std::optional<uint16_t> EndpointStore::get_or_assign(const Uuid &module_uuid)
{
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto &e : entries_) {
        if (e.uuid == module_uuid) {
            return e.endpoint_id;
        }
    }
    if (entries_.size() >= kMaxEndpoints) {
        return std::nullopt;
    }

    Entry e;
    e.uuid = module_uuid;
    e.endpoint_id = next_free_endpoint_id_locked();
    entries_.push_back(e);

    if (!persist_locked()) {
        // Roll back in memory -- an ID that failed to persist must never be
        // handed to a caller (see this header's own top comment), and a
        // subsequent retry must be free to try again from a clean state.
        entries_.pop_back();
        return std::nullopt;
    }

    return e.endpoint_id;
}

std::optional<uint16_t> EndpointStore::lookup(const Uuid &module_uuid) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto &e : entries_) {
        if (e.uuid == module_uuid) {
            return e.endpoint_id;
        }
    }
    return std::nullopt;
}

std::optional<Uuid> EndpointStore::reverse_lookup(uint16_t endpoint_id) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto &e : entries_) {
        if (e.endpoint_id == endpoint_id) {
            return e.uuid;
        }
    }
    return std::nullopt;
}

} // namespace matter_bridge
