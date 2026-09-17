// Covers this repo's test list items 7-8 (a restarted process hands out the
// SAME endpoint ID for the same module_uuid; different module_uuids get
// different endpoint IDs) plus corruption handling, against the production
// matter_bridge::EndpointStore -- see include/matter_bridge/endpoint_store.hpp.
#include "matter_bridge/endpoint_store.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <unistd.h>

#include "test_check.hpp"

using matter_bridge::EndpointStore;
using matter_bridge::LoadResult;
using matter_bridge::Uuid;

namespace {

std::string make_temp_dir()
{
    char tmpl[] = "/tmp/mb_endpoint_store_test_XXXXXX";
    char *dir = mkdtemp(tmpl);
    return std::string(dir);
}

Uuid make_uuid(uint8_t tag)
{
    Uuid u{};
    u.fill(tag);
    return u;
}

} // namespace

void RunEndpointStoreTests(int *failures)
{
    printf("\n-- scenario: a fresh state-dir has no mapping file yet --\n");
    {
        std::string dir = make_temp_dir();
        EndpointStore store(dir);
        MB_CHECK(store.load() == LoadResult::NotFound, "load() on an empty state-dir returns NotFound, not Corrupt");
        MB_CHECK(store.size() == 0, "a NotFound store starts empty");
    }

    printf("\n-- scenario: get_or_assign hands out increasing IDs starting at kFirstDynamicEndpointId, and the "
           "SAME uuid always gets the SAME id back --\n");
    std::string dir_a = make_temp_dir();
    Uuid uuid1 = make_uuid(0x01);
    Uuid uuid2 = make_uuid(0x02);
    uint16_t id1, id2;
    {
        EndpointStore store(dir_a);
        store.load();
        auto a1 = store.get_or_assign(uuid1);
        MB_CHECK(a1.has_value(), "assigning a brand-new uuid succeeds");
        id1 = *a1;
        MB_CHECK(id1 == EndpointStore::kFirstDynamicEndpointId, "the first-ever uuid gets kFirstDynamicEndpointId");

        auto a1_again = store.get_or_assign(uuid1);
        MB_CHECK(a1_again.has_value() && *a1_again == id1, "asking for the same uuid again returns the SAME id, "
                                                            "never a second one");

        auto a2 = store.get_or_assign(uuid2);
        MB_CHECK(a2.has_value(), "assigning a second, different uuid succeeds");
        id2 = *a2;
        MB_CHECK(id2 != id1, "a different module_uuid always gets a DIFFERENT endpoint id");
    }

    printf("\n-- scenario: a brand-new EndpointStore instance pointed at the SAME state-dir (simulating a process "
           "restart) loads the SAME uuid -> endpoint id mapping --\n");
    {
        EndpointStore restarted(dir_a);
        MB_CHECK(restarted.load() == LoadResult::Ok, "load() on a previously-persisted state-dir returns Ok");
        MB_CHECK(restarted.size() == 2, "both previously-assigned uuids are present after reload");

        auto looked_up1 = restarted.lookup(uuid1);
        MB_CHECK(looked_up1.has_value() && *looked_up1 == id1,
                 "after a restart, the first uuid gets back the EXACT SAME endpoint id it had before -- this is "
                 "the restart-persistence guarantee this whole class exists for");
        auto looked_up2 = restarted.lookup(uuid2);
        MB_CHECK(looked_up2.has_value() && *looked_up2 == id2, "the second uuid's id also survives the restart");

        // get_or_assign() on an already-known uuid after reload must not
        // silently hand out a NEW id either.
        auto reassigned = restarted.get_or_assign(uuid1);
        MB_CHECK(reassigned.has_value() && *reassigned == id1,
                 "get_or_assign() on an already-known uuid after a restart still returns the original id");

        auto rev = restarted.reverse_lookup(id1);
        MB_CHECK(rev.has_value() && *rev == uuid1, "reverse_lookup() recovers the original uuid from its endpoint id");
    }

    printf("\n-- scenario: a corrupted mapping file is detected and reported, never silently treated as empty --\n");
    {
        std::string dir = make_temp_dir();
        {
            EndpointStore store(dir);
            store.load();
            store.get_or_assign(make_uuid(0xAA));
        }
        // Corrupt the persisted file in place (flip bytes in the middle --
        // this must fail the CRC check regardless of which bytes it hits).
        std::string path = dir + "/endpoint_map.bin";
        std::fstream f(path, std::ios::in | std::ios::out | std::ios::binary);
        f.seekp(5);
        char garbage[3] = {'\x00', '\x01', '\x02'};
        f.write(garbage, sizeof(garbage));
        f.close();

        EndpointStore reopened(dir);
        MB_CHECK(reopened.load() == LoadResult::Corrupt,
                 "a mapping file that fails its CRC is reported as Corrupt, never silently loaded as if it were "
                 "empty or valid (see this class's own doc comment on why that matters: silently losing this "
                 "mapping would let a module_uuid get reassigned to a different endpoint id)");
    }

    printf("\n-- scenario: capacity is bounded at kMaxEndpoints --\n");
    {
        std::string dir = make_temp_dir();
        EndpointStore store(dir);
        store.load();
        int successes = 0;
        for (int i = 0; i < (int)EndpointStore::kMaxEndpoints; i++) {
            if (store.get_or_assign(make_uuid((uint8_t)i)).has_value()) {
                successes++;
            }
        }
        MB_CHECK(successes == (int)EndpointStore::kMaxEndpoints, "filling the store: every distinct uuid up to "
                                                                   "kMaxEndpoints is accepted");
        auto overflow = store.get_or_assign(make_uuid(0xFE));
        MB_CHECK(!overflow.has_value(), "a brand-new uuid past capacity is refused, not silently overwriting an "
                                          "existing entry");
    }
}
