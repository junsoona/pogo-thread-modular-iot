// The hub's copy of the shared home settings (home_settings.hpp), kept in
// <--state-dir>/pogohome/settings.json next to hub_id and endpoint_map.bin.
//
// Concurrency: the server owns the revision. Every successful write bumps
// it, and a write names the revision it was based on; a different current
// revision is a conflict and nothing is written, so an edit made against an
// older copy can never overwrite (or undelete) a newer change. Device clocks
// never decide anything.
//
// The revision is opaque to clients: "<settings_id>-<n>", where settings_id
// is random per settings file. A settings file that is deleted and created
// again therefore never repeats an earlier revision string, so a client that
// cached "…-3" of the old file can never mistake "…-3" of the new one for
// it.
//
// Durability and damage:
// - a write goes to a temporary file, is fsync'd and renamed into place
//   (WriteFileAtomically); a failed write leaves the previous file and the
//   in-memory copy untouched and is reported as a failure, never a success;
// - a file that exists but cannot be read, parsed or validated makes the
//   store Unavailable: reads and writes are refused with the reason, and the
//   file is never replaced or reset -- an operator inspects or moves it
//   aside (docs/pogohome-api-v1.md). Sensor data and Matter are unaffected.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>

#include "pogohome_api/home_settings.hpp"

namespace pogohome_api {

constexpr const char kSettingsFileName[] = "settings.json";
// A stored file larger than this is treated as damaged.
constexpr size_t kMaxSettingsFileBytes = 256 * 1024;

enum class SettingsState {
    // No settings file yet: the first device to share uploads one.
    Uninitialized,
    Ready,
    // The file exists but could not be read or is damaged; see `reason`.
    Unavailable,
};

struct SettingsSnapshot {
    SettingsState state = SettingsState::Uninitialized;
    std::string revision; // "" unless Ready
    int64_t updated_at_unix_ms = 0;
    HomeSettings settings;
    std::string unavailable_reason; // "corrupt: …" / "io: …" when Unavailable
};

enum class SettingsUpdateOutcome {
    Saved,
    // base revision differs from the current one (or a create raced another)
    Conflict,
    Unavailable,
    SaveFailed,
};

struct SettingsUpdateResult {
    SettingsUpdateOutcome outcome = SettingsUpdateOutcome::SaveFailed;
    // After Saved: the new state. After Conflict: the current state, so the
    // client can show it without another request.
    SettingsSnapshot current;
    std::string error; // SaveFailed / Unavailable detail (for logs)
};

class SettingsStore {
public:
    // Writes `contents` to `path` durably; WriteFileAtomically in production.
    // Tests substitute a failing writer.
    using Writer = std::function<bool(const std::string &path, const std::string &contents, std::string &error)>;

    explicit SettingsStore(std::string directory, Writer writer = nullptr);

    // Reads the file (once, at startup). Never modifies it.
    void Load();

    SettingsSnapshot Get() const;

    // `base_revision` empty means "create": allowed only while Uninitialized
    // (the first upload). Otherwise it must equal the current revision.
    // `proposed` must already be validated (ParseHomeSettings does).
    SettingsUpdateResult Update(const std::string &base_revision, const HomeSettings &proposed, int64_t now_unix_ms);

    // Bumped after every saved change, for the SSE loop (http_server.cpp).
    uint64_t change_counter() const { return change_counter_.load(std::memory_order_relaxed); }

    std::string path() const { return directory_ + "/" + kSettingsFileName; }

private:
    std::string SerializeLocked(const std::string &settings_id, uint64_t revision, int64_t updated_at,
                                const HomeSettings &settings) const;

    const std::string directory_;
    Writer writer_;
    mutable std::mutex mutex_;
    SettingsState state_ = SettingsState::Uninitialized;
    std::string settings_id_;
    uint64_t revision_ = 0;
    int64_t updated_at_unix_ms_ = 0;
    HomeSettings settings_;
    std::string unavailable_reason_;
    std::atomic<uint64_t> change_counter_{0};
};

// The GET /api/v1/settings body (also the `current` of a 409 and the body of
// a successful PUT).
std::string BuildSettingsResponseJson(const SettingsSnapshot &snapshot, const std::string &hub_id);

} // namespace pogohome_api
