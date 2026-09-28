// Small-file persistence helpers shared by the settings store and device
// credentials (see settings_store.hpp, device_auth.hpp).
#pragma once

#include <string>

namespace pogohome_api {

// Replaces `path` with `contents` so a reader only ever sees the old file or
// the complete new one: the data goes to `<path>.tmp` (mode 0600), is
// fsync'd, renamed over `path`, and the directory is fsync'd. On failure the
// old file is left in place, the temporary file is removed, and `error` says
// what failed.
bool WriteFileAtomically(const std::string &path, const std::string &contents, std::string &error);

enum class ReadFileResult { Ok, Missing, Error };

// Reads a whole file of at most `max_bytes`. A larger file is an Error (so a
// damaged or hostile file can never make the reader allocate without bound).
ReadFileResult ReadWholeFile(const std::string &path, size_t max_bytes, std::string &out, std::string &error);

// Creates `dir` (one level) with mode 0700 if it does not exist. An existing
// directory is kept as it is, with its permissions tightened to 0700 when it
// is owned by this process's user. False (with `error`) if `dir` exists and is
// not a directory, or cannot be created.
bool EnsurePrivateDirectory(const std::string &dir, std::string &error);

} // namespace pogohome_api
