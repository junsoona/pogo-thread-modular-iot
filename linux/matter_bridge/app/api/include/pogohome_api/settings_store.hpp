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
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "pogohome_api/home_settings.hpp"
#include "pogohome_api/room_cleanup.hpp"

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

enum class RoomUnassignOutcome {
    Nothing,     // no settings yet, or none of the modules had a room
    Saved,       // a new revision without those room assignments
    Unavailable, // the settings file is damaged: nothing was changed
    Failed,      // could not be saved (write failure, or other writers kept winning)
};

// Power Base management (docs/power-base-management.md): after a Power Base
// left this hub's list (unenrolled, or taken out of the list), the modules
// that left with it are moved to "no room" in the shared settings. Their
// custom names stay: a module is keyed by its own UUID, so it gets its name
// back when it is docked again. Rooms themselves, the home name and every
// other module are untouched. This is an ordinary write through
// SettingsStore::Update() -- based on the current revision, bumping it, and
// retried a few times if a device saved in between -- so every device sees it
// as a normal new revision and nothing a device saved meanwhile is overwritten.
//
// Which assignments are cleared is decided for every attempt of the write,
// from the settings that attempt read (room_cleanup.hpp):
//   - a target whose room is known is cleared only while the module is still
//     in exactly that room -- a room it was given since is not this clean-up's;
//   - the write itself is made through `still_owed.commit()`: which of the
//     remaining targets are still the clean-up's is decided, and the settings
//     are written, in one section in which no report can list a module under
//     a Power Base. A module that is listed under one by then -- also one
//     that was listed between this function's read and its write -- is left
//     out there and keeps the room it has, whether or not anybody saved the
//     settings in between (when nobody did, the write is no conflict).
//     (`still_owed`'s plain question, when it has one, is asked before that:
//     a module it refuses is no candidate at all.)
// A write that loses against another writer (a revision conflict) starts
// again from that writer's settings and asks all of this again: an assignment
// saved in between is seen, never overwritten.
// `before_write` (tests only) runs after the settings were read and the
// candidates were found, and before the section of the write is entered:
// the place where another writer, or a report to the Registry, can get in
// between.
RoomUnassignOutcome UnassignModuleRooms(SettingsStore &store, const std::vector<RoomCleanupTarget> &targets,
                                        int64_t now_unix_ms, const RoomCleanupGuard &still_owed = nullptr,
                                        const std::function<void()> &before_write = nullptr);

// The clean-up hook Power Base management retries until it reports success
// (PowerBaseManagementConfig::unassign_rooms): true when the rooms are
// cleared or there was nothing to clear, false when the settings could not
// be written now -- a damaged file, a failed write, or other writers winning
// every attempt. `store` may be null (no settings sharing): then there is
// nothing to clear. `now_unix_ms` is read for every call.
RoomUnassigner MakeRoomUnassigner(std::shared_ptr<SettingsStore> store, std::function<int64_t()> now_unix_ms);

// What Power Base management reads when a Base leaves the list
// (PowerBaseManagementConfig::read_rooms): the room each module is in at that
// moment. It only copies the settings held in memory. `store` may be null:
// then no module has a room.
RoomReader MakeRoomReader(std::shared_ptr<SettingsStore> store);

// The GET /api/v1/settings body (also the `current` of a 409 and the body of
// a successful PUT).
std::string BuildSettingsResponseJson(const SettingsSnapshot &snapshot, const std::string &hub_id);

} // namespace pogohome_api
