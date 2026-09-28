#include "pogohome_api/settings_store.hpp"

#include <unistd.h>

#include "pogohome_api/atomic_file.hpp"
#include "pogohome_api/crypto_util.hpp"

namespace pogohome_api {

namespace {

constexpr const char kFormatName[] = "pogohome-home-settings";

std::string RevisionString(const std::string &settings_id, uint64_t revision)
{
    return settings_id + "-" + std::to_string(revision);
}

} // namespace

SettingsStore::SettingsStore(std::string directory, Writer writer)
    : directory_(std::move(directory)), writer_(writer ? std::move(writer) : Writer(WriteFileAtomically))
{
}

void SettingsStore::Load()
{
    std::lock_guard<std::mutex> lock(mutex_);
    state_ = SettingsState::Uninitialized;
    settings_id_.clear();
    revision_ = 0;
    updated_at_unix_ms_ = 0;
    settings_ = HomeSettings();
    unavailable_reason_.clear();

    // A temporary file left by a write that was interrupted before its
    // rename never became the settings; the real file is untouched.
    unlink((path() + ".tmp").c_str());

    std::string contents;
    std::string error;
    ReadFileResult read = ReadWholeFile(path(), kMaxSettingsFileBytes, contents, error);
    if (read == ReadFileResult::Missing) {
        return;
    }
    if (read == ReadFileResult::Error) {
        state_ = SettingsState::Unavailable;
        unavailable_reason_ = "io: " + error;
        return;
    }

    auto corrupt = [this](const std::string &why) {
        state_ = SettingsState::Unavailable;
        unavailable_reason_ = "corrupt: " + why;
    };

    JsonValue root;
    if (!ParseJson(contents, root, error)) {
        corrupt("not valid JSON (" + error + ")");
        return;
    }
    const JsonValue *format = root.Find("format");
    const JsonValue *schema = root.Find("schema_version");
    const JsonValue *settings_id = root.Find("settings_id");
    const JsonValue *revision = root.Find("revision");
    const JsonValue *updated_at = root.Find("updated_at_unix_ms");
    const JsonValue *settings = root.Find("settings");
    if (format == nullptr || !format->IsString() || format->string_value != kFormatName) {
        corrupt("not a PogoHome settings file");
        return;
    }
    if (schema == nullptr || !schema->IsInteger() || schema->int_value != kSettingsSchemaVersion) {
        corrupt("unsupported schema_version");
        return;
    }
    if (settings_id == nullptr || !settings_id->IsString() || !IsLowerHex(settings_id->string_value, 16)) {
        corrupt("invalid settings_id");
        return;
    }
    if (revision == nullptr || !revision->IsInteger() || revision->int_value < 1) {
        corrupt("invalid revision");
        return;
    }
    if (updated_at == nullptr || !updated_at->IsInteger() || updated_at->int_value < 0) {
        corrupt("invalid updated_at_unix_ms");
        return;
    }
    HomeSettings parsed;
    SettingsError settings_error;
    if (settings == nullptr || !ParseHomeSettings(*settings, parsed, settings_error)) {
        corrupt("invalid settings (" + (settings == nullptr ? std::string("missing") : settings_error.code) + ")");
        return;
    }

    state_ = SettingsState::Ready;
    settings_id_ = settings_id->string_value;
    revision_ = static_cast<uint64_t>(revision->int_value);
    updated_at_unix_ms_ = updated_at->int_value;
    settings_ = std::move(parsed);
}

SettingsSnapshot SettingsStore::Get() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    SettingsSnapshot out;
    out.state = state_;
    if (state_ == SettingsState::Ready) {
        out.revision = RevisionString(settings_id_, revision_);
        out.updated_at_unix_ms = updated_at_unix_ms_;
        out.settings = settings_;
    }
    out.unavailable_reason = unavailable_reason_;
    return out;
}

std::string SettingsStore::SerializeLocked(const std::string &settings_id, uint64_t revision, int64_t updated_at,
                                           const HomeSettings &settings) const
{
    std::string out = "{\"format\":\"";
    out += kFormatName;
    out += "\",\"schema_version\":";
    out += std::to_string(kSettingsSchemaVersion);
    out += ",\"settings_id\":";
    AppendJsonStringLiteral(out, settings_id);
    out += ",\"revision\":";
    out += std::to_string(revision);
    out += ",\"updated_at_unix_ms\":";
    out += std::to_string(updated_at);
    out += ",\"settings\":";
    AppendHomeSettingsJson(out, settings);
    out += "}\n";
    return out;
}

SettingsUpdateResult SettingsStore::Update(const std::string &base_revision, const HomeSettings &proposed,
                                           int64_t now_unix_ms)
{
    SettingsUpdateResult result;
    std::unique_lock<std::mutex> lock(mutex_);

    auto current = [this]() {
        SettingsSnapshot snapshot;
        snapshot.state = state_;
        if (state_ == SettingsState::Ready) {
            snapshot.revision = RevisionString(settings_id_, revision_);
            snapshot.updated_at_unix_ms = updated_at_unix_ms_;
            snapshot.settings = settings_;
        }
        snapshot.unavailable_reason = unavailable_reason_;
        return snapshot;
    };

    if (state_ == SettingsState::Unavailable) {
        result.outcome = SettingsUpdateOutcome::Unavailable;
        result.error = unavailable_reason_;
        result.current = current();
        return result;
    }

    std::string next_id = settings_id_;
    uint64_t next_revision = revision_ + 1;
    if (state_ == SettingsState::Uninitialized) {
        if (!base_revision.empty()) {
            result.outcome = SettingsUpdateOutcome::Conflict;
            result.current = current();
            return result;
        }
        next_id = RandomHex(8);
        if (next_id.empty()) {
            result.outcome = SettingsUpdateOutcome::SaveFailed;
            result.error = "random number generator failed";
            result.current = current();
            return result;
        }
        next_revision = 1;
    } else if (base_revision.empty() || base_revision != RevisionString(settings_id_, revision_)) {
        result.outcome = SettingsUpdateOutcome::Conflict;
        result.current = current();
        return result;
    }

    // A clock that moved backwards never makes the stored time go back.
    int64_t updated_at = now_unix_ms > updated_at_unix_ms_ ? now_unix_ms : updated_at_unix_ms_;
    std::string contents = SerializeLocked(next_id, next_revision, updated_at, proposed);
    std::string write_error;
    if (!writer_(path(), contents, write_error)) {
        result.outcome = SettingsUpdateOutcome::SaveFailed;
        result.error = write_error;
        result.current = current();
        return result;
    }

    state_ = SettingsState::Ready;
    settings_id_ = next_id;
    revision_ = next_revision;
    updated_at_unix_ms_ = updated_at;
    settings_ = proposed;
    change_counter_.fetch_add(1, std::memory_order_relaxed);
    result.outcome = SettingsUpdateOutcome::Saved;
    result.current = current();
    return result;
}

std::string BuildSettingsResponseJson(const SettingsSnapshot &snapshot, const std::string &hub_id)
{
    std::string out = "{\"schema_version\":";
    out += std::to_string(kSettingsSchemaVersion);
    out += ",\"hub_id\":";
    AppendJsonStringLiteral(out, hub_id);
    if (snapshot.state == SettingsState::Ready) {
        out += ",\"initialized\":true,\"revision\":";
        AppendJsonStringLiteral(out, snapshot.revision);
        out += ",\"updated_at_unix_ms\":";
        out += std::to_string(snapshot.updated_at_unix_ms);
        out += ",\"settings\":";
        AppendHomeSettingsJson(out, snapshot.settings);
    } else {
        out += ",\"initialized\":false,\"revision\":null,\"settings\":null";
    }
    out += '}';
    return out;
}

} // namespace pogohome_api
