// pogohome_api::SettingsStore -- revisions, conflicts, durable saves, restart,
// write failures and damaged files (settings_store.hpp).
#include <string>

#include "pogohome_api/atomic_file.hpp"
#include "pogohome_api/settings_store.hpp"
#include "test_check.hpp"
#include "test_temp_dir.hpp"

using namespace pogohome_api;
using test_support::MakeTempDir;
using test_support::ReadFile;
using test_support::RemoveTree;
using test_support::WriteFile;

namespace {

HomeSettings TwoRooms(const std::string &home_name)
{
    HomeSettings s;
    s.has_home_name = true;
    s.home_name = home_name;
    RoomSetting living;
    living.id = "8A3B1C10-0001-4000-8000-000000000001";
    living.name_kind = RoomNameKind::Suggestion;
    living.suggestion_symbol = "sofa.fill";
    living.suggestion_ordinal = 1;
    living.symbol = "sofa.fill";
    RoomSetting office;
    office.id = "11111111-2222-4333-8444-555555555555";
    office.name_kind = RoomNameKind::Custom;
    office.name = "Office";
    office.symbol = "desktopcomputer";
    s.rooms = {living, office};
    ModuleSetting module;
    module.uuid = "744dbdfffe65d357";
    module.has_name = true;
    module.name = "Gas";
    module.has_room = true;
    module.room_id = office.id;
    s.modules = {module};
    return s;
}

} // namespace

void RunSettingsStoreTests(int *failures)
{
    printf("\n-- scenario: first upload, then revisioned updates, and a stale update is a conflict --\n");
    {
        std::string dir = MakeTempDir();
        SettingsStore store(dir);
        store.Load();
        API_CHECK(store.Get().state == SettingsState::Uninitialized, "a store without a file is uninitialized");

        SettingsUpdateResult stale_create = store.Update("abc-1", TwoRooms("A"), 1000);
        API_CHECK(stale_create.outcome == SettingsUpdateOutcome::Conflict,
                  "a write naming a revision while nothing exists yet is a conflict");

        SettingsUpdateResult created = store.Update("", TwoRooms("A"), 1000);
        API_CHECK(created.outcome == SettingsUpdateOutcome::Saved, "the first upload (no base revision) is saved");
        const std::string rev1 = created.current.revision;
        API_CHECK(rev1.size() == 18 && rev1.substr(16) == "-1", "the first revision is <settings_id>-1");
        API_CHECK(store.change_counter() == 1, "a saved change bumps the change counter");

        SettingsUpdateResult second_create = store.Update("", TwoRooms("B"), 1100);
        API_CHECK(second_create.outcome == SettingsUpdateOutcome::Conflict &&
                      second_create.current.settings.home_name == "A",
                  "a second 'first upload' (another device racing) is a conflict and returns the settings already "
                  "there");

        SettingsUpdateResult updated = store.Update(rev1, TwoRooms("B"), 2000);
        API_CHECK(updated.outcome == SettingsUpdateOutcome::Saved, "an update based on the current revision is saved");
        const std::string rev2 = updated.current.revision;
        API_CHECK(rev2 != rev1 && rev2.substr(0, 16) == rev1.substr(0, 16) && rev2.substr(16) == "-2",
                  "the revision advances to -2 under the same settings_id");

        HomeSettings deleted = TwoRooms("C");
        deleted.rooms.pop_back();
        deleted.modules[0].has_room = false;
        SettingsUpdateResult stale = store.Update(rev1, deleted, 3000);
        API_CHECK(stale.outcome == SettingsUpdateOutcome::Conflict, "an update based on an older revision is refused");
        API_CHECK(stale.current.revision == rev2 && stale.current.settings.home_name == "B" &&
                      stale.current.settings.rooms.size() == 2,
                  "the conflict leaves the newer settings untouched and reports them");
        API_CHECK(store.change_counter() == 2, "a refused write does not bump the change counter");

        SettingsUpdateResult earlier_clock = store.Update(rev2, TwoRooms("D"), 5);
        API_CHECK(earlier_clock.outcome == SettingsUpdateOutcome::Saved &&
                      earlier_clock.current.updated_at_unix_ms == 2000,
                  "a device clock that is behind never moves the stored time backwards (and decides nothing)");
        RemoveTree(dir);
    }

    printf("\n-- scenario: settings survive a restart (a new store over the same directory) --\n");
    {
        std::string dir = MakeTempDir();
        std::string revision;
        {
            SettingsStore store(dir);
            store.Load();
            revision = store.Update("", TwoRooms("Home"), 1234).current.revision;
        }
        SettingsStore reopened(dir);
        reopened.Load();
        SettingsSnapshot s = reopened.Get();
        API_CHECK(s.state == SettingsState::Ready && s.revision == revision, "the revision is the same after a restart");
        API_CHECK(s.settings.home_name == "Home" && s.settings.rooms.size() == 2 &&
                      s.settings.rooms[1].name == "Office" && s.settings.modules.size() == 1 &&
                      s.settings.modules[0].room_id == "11111111-2222-4333-8444-555555555555",
                  "rooms, their order and module placement are the same after a restart");
        API_CHECK(test_support::FileMode(reopened.path()) == 0600, "settings.json is only readable by its owner");
        RemoveTree(dir);
    }

    printf("\n-- scenario: a failed write is reported and changes nothing --\n");
    {
        std::string dir = MakeTempDir();
        bool fail = false;
        SettingsStore store(dir, [&fail](const std::string &path, const std::string &contents, std::string &error) {
            if (fail) {
                error = "No space left on device";
                return false;
            }
            return WriteFileAtomically(path, contents, error);
        });
        store.Load();
        std::string rev1 = store.Update("", TwoRooms("Before"), 1).current.revision;
        std::string before_bytes;
        ReadFile(store.path(), before_bytes);

        fail = true;
        SettingsUpdateResult failed = store.Update(rev1, TwoRooms("After"), 2);
        API_CHECK(failed.outcome == SettingsUpdateOutcome::SaveFailed, "a write failure is SaveFailed, never Saved");
        API_CHECK(store.Get().revision == rev1 && store.Get().settings.home_name == "Before",
                  "after the failure the store still serves the previous settings");
        std::string after_bytes;
        ReadFile(store.path(), after_bytes);
        API_CHECK(before_bytes == after_bytes, "the file on disk is unchanged");
        API_CHECK(store.change_counter() == 1, "no change is announced");

        fail = false;
        API_CHECK(store.Update(rev1, TwoRooms("After"), 3).outcome == SettingsUpdateOutcome::Saved,
                  "the same update succeeds once writing works again");
        RemoveTree(dir);
    }

    printf("\n-- scenario: a missing state directory makes the first upload fail, not succeed --\n");
    {
        SettingsStore store("/nonexistent/pogohome-test-dir");
        store.Load();
        API_CHECK(store.Update("", TwoRooms("X"), 1).outcome == SettingsUpdateOutcome::SaveFailed,
                  "the real atomic writer reports the failure");
        API_CHECK(store.Get().state == SettingsState::Uninitialized, "the store stays uninitialized");
    }

    printf("\n-- scenario: a damaged settings file is never reset or overwritten --\n");
    {
        const char *damaged[] = {
            "{\"format\":\"pogohome-home-settings\",\"schema_version\":1,\"settings_id\":\"0123456789abcdef\","
            "\"revision\":3,\"updated_at_unix_ms\":1,\"settings\":{\"home_name\":null,\"rooms\":[",
            "not json at all",
            "{\"format\":\"pogohome-home-settings\",\"schema_version\":2,\"settings_id\":\"0123456789abcdef\","
            "\"revision\":3,\"updated_at_unix_ms\":1,\"settings\":{\"home_name\":null,\"rooms\":[],\"modules\":[]}}",
            "{\"format\":\"pogohome-home-settings\",\"schema_version\":1,\"settings_id\":\"0123456789abcdef\","
            "\"revision\":3,\"updated_at_unix_ms\":1,\"settings\":{\"home_name\":null,\"rooms\":[],\"modules\":"
            "[{\"uuid\":\"744dbdfffe65d357\",\"name\":null,\"room_id\":\"11111111-2222-4333-8444-555555555555\"}]}}",
        };
        const char *labels[] = {"a truncated file", "a file that is not JSON", "an unknown schema_version",
                                "a module in a room that does not exist"};
        for (size_t i = 0; i < sizeof(damaged) / sizeof(damaged[0]); i++) {
            std::string dir = MakeTempDir();
            SettingsStore store(dir);
            WriteFile(store.path(), damaged[i]);
            store.Load();
            SettingsSnapshot s = store.Get();
            std::string label = std::string(labels[i]) + ": the store is Unavailable (corrupt)";
            API_CHECK(s.state == SettingsState::Unavailable && s.unavailable_reason.rfind("corrupt", 0) == 0,
                      label.c_str());
            SettingsUpdateResult r = store.Update("", TwoRooms("New"), 1);
            label = std::string(labels[i]) + ": a write is refused (not treated as a first upload)";
            API_CHECK(r.outcome == SettingsUpdateOutcome::Unavailable, label.c_str());
            std::string bytes;
            ReadFile(store.path(), bytes);
            label = std::string(labels[i]) + ": the file keeps its exact bytes";
            API_CHECK(bytes == damaged[i], label.c_str());
            RemoveTree(dir);
        }
    }

    printf("\n-- scenario: a leftover temporary file from an interrupted write is ignored --\n");
    {
        std::string dir = MakeTempDir();
        std::string rev;
        {
            SettingsStore store(dir);
            store.Load();
            rev = store.Update("", TwoRooms("Kept"), 1).current.revision;
        }
        WriteFile(dir + "/settings.json.tmp", "{half a write");
        SettingsStore reopened(dir);
        reopened.Load();
        API_CHECK(reopened.Get().state == SettingsState::Ready && reopened.Get().revision == rev &&
                      reopened.Get().settings.home_name == "Kept",
                  "the last complete settings are loaded");
        std::string leftover;
        API_CHECK(!ReadFile(dir + "/settings.json.tmp", leftover), "the stale temporary file is removed");
        RemoveTree(dir);
    }

    printf("\n-- scenario: the settings response JSON --\n");
    {
        SettingsSnapshot empty;
        std::string json = BuildSettingsResponseJson(empty, "3f0c9a4e8b1d4c2a9e7f6b5a4d3c2b1a");
        API_CHECK(json == "{\"schema_version\":1,\"hub_id\":\"3f0c9a4e8b1d4c2a9e7f6b5a4d3c2b1a\",\"initialized\":false,"
                          "\"revision\":null,\"settings\":null}",
                  "an uninitialized hub says so, with a null revision");
    }
}
