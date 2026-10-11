// Power Base management on the hub (docs/power-base-management.md): the
// per-enrollment management credentials, the authenticated "pogo/v1/mgmt"
// exchange with registered Power Bases, and the persistent removal
// (unenroll) jobs behind POST /api/v1/power-base-removals.
//
// Trust: nothing here believes a Thread address, an EUI-64 or a CRC. A
// message from a board is acted on only when its HMAC-SHA-256 verifies with
// the management key this hub issued for exactly that board and enrollment
// generation (components/power_base_mgmt/pb_mgmt_proto.h -- the same C file
// the firmware compiles). The hub never opens a connection to a board: a
// command only ever travels in the answer to that board's own authenticated
// poll, and only to a poll that echoes the nonce of this hub's previous
// answer, so a recorded poll cannot fetch one.
//
// A removal has four different moments and none is taken for another:
//   accepted   the board's authenticated UNENROLL_ACCEPTED: it journaled the
//              command.
//   released   the board's authenticated UNENROLL_DONE with result RELEASED:
//              it stored, for good, that this enrollment is out of use. From
//              here on the hub treats the Base as gone -- its data is
//              blocked, it and its modules leave the Registry, their rooms
//              are cleared -- but the removal is NOT complete: the board's
//              stored join information may still exist.
//
// One message is about neither a removal nor a board's state: LOCATE. A
// board whose stored hub endpoint stopped answering asks whatever it can
// reach in its Thread network "where is my hub now?"; this hub answers, to a
// request that verifies with a credential that is still in use (provisional
// or active -- never a released, superseded or unknown one), with its
// present ingest endpoint under that credential's key. Being told an
// endpoint is not being trusted: the board uses it only after this hub
// confirmed it from that very endpoint (pb_mgmt_client.h). The answer
// reveals the hub's Thread address to a holder of one of its keys and
// changes nothing here.
//
//   completed  an authenticated UNENROLL_DONE with result ERASED: the board
//              erased its stored network, checked that, and only then made
//              this message (its release receipt). It comes in one of two
//              ways, and it is the same message either way: on the
//              management resource, sent by the board itself over one last
//              Thread session that exists in its RAM only (a board that
//              announced PB_MGMT_BOARD_CAP_THREAD_RECEIPT with HELLO), or
//              through the HTTP API, relayed by an app that read it from the
//              board over BLE (SubmitReleaseReceipt()). Nothing else
//              completes a removal: not the released notice, not silence, not
//              a timeout, not the board's announcement that it will send one.
//   cleaned    the room assignments of the modules that left with the Base
//              were cleared in the shared settings. Tracked separately and
//              persistently (see "clean-ups" below): a removal can be
//              released or completed while this is still owed.
//
// A different thing altogether is taking a Power Base OUT OF THIS HUB'S LIST
// (ForgetPowerBase(), section 13 of the document): for a Base that does not
// come back. It says nothing about the board -- its stored join information
// is NOT erased and nobody claims it was -- and it never makes a removal
// "completed". The hub drops what it holds of that registration (the
// credentials), blocks the Base exactly as a released one is blocked, takes
// it and its modules out of the Registry and notes their rooms for the same
// clean-up. Only for a Base that is offline at the moment the request is
// carried out (checked and applied under the Registry's lock), and only for
// the registration the request names (its `record`): a request made for an
// earlier registration, or while the Base is heard, changes nothing.
//
// A removal, too, is asked of ONE registration and of none other
// (RequestRemoval(), section 16 of the document). A Base is known by its
// EUI-64 for as long as the board exists; a registration ends and another
// begins. A request that named the EUI-64 alone -- sent before the Base was
// removed and registered again, and arriving, or sent once more, after --
// would unenroll a registration nobody asked to remove. So a request names the
// registration its device confirmed (`record`), and how many removals had been
// asked of it at that moment (`removal_serial`); it creates a job only while
// both are still what this hub has, and by creating it moves the serial on.
// One request therefore creates at most one job, and only for the registration
// it was made for -- whether or not anything of it is still remembered here.
//
// What is kept, in <--state-dir>/pogohome/power_base_mgmt.json (0600, written
// with WriteFileAtomically):
//   credentials  per Power Base EUI-64: provisional (issued with an
//                enrollment session, the board has not used it yet), active
//                (the board's first authenticated poll arrived), or released
//                (the board said "released"; kept to verify its receipt).
//                Dropped when the receipt completed the removal, or when a
//                new generation of the board becomes active. With it: the
//                capabilities the board announced (HELLO), 0 when it never
//                did; and how many removals were asked of this registration
//                ("removal_serial", optional: a file without it says 0);
//   removals     the jobs, each bound to one EUI-64 and one generation; a
//                completed one keeps the MAC of the receipt that completed
//                it and of the acknowledgement it was given, so that the
//                same receipt gets the same answer after the key is gone;
//                and the id of the request that created it ("request_id",
//                optional: a job stored by an earlier hub has none);
//   blocked      EUI-64s whose data the ingest path refuses
//                (ingest::Registry::set_power_base_blocked());
//   cleanups     per released removal (and per list removal), the modules
//                whose room assignment still has to be cleared, with the
//                retry state -- and, when the settings could be read at that
//                moment, the room each of them was in ("rooms", an optional
//                list): the clean-up clears THAT assignment, not whatever
//                room the module has when it finally runs;
//   members      per listed Power Base, the modules the Registry last had
//                under it (an optional list: a file without it has none).
//                This is what is known, after a restart, of a Base that has
//                not reported again -- so that taking such a Base out of the
//                list can still name the modules whose rooms are to be
//                cleared. A module belongs to the Base it was reported under
//                last: it moves here as soon as another Base reports it, and
//                it leaves when its Base says it was detached. Evidence, and
//                nothing more: no member is ever made up, and what is not
//                written here is not cleaned;
//   forgets      the list removals (ForgetPowerBase()), for as long as a
//                finished removal is kept -- and the last one of a Base for
//                as long as that Base stays out of the list: what a device
//                that lost the answer (or was away) reads, and what makes the
//                same request the same answer. An
//                optional list: a file without it has none. The EFFECTS of a
//                list removal are not here -- they are the block, the dropped
//                credentials and the clean-up above.
//
// Blocks are never dropped to make room. A block ends in exactly one way: a
// new enrollment generation of that board becomes active. The list is
// bounded (kMaxBlockedPowerBases), so room for a block is reserved when a
// removal is REQUESTED: a request is refused (CapacityFull) unless the
// existing blocks plus the removals that may still lead to one leave a free
// slot. What was reserved cannot be missing when the board reports.
// A file that exists but cannot be read or validated makes the whole feature
// unavailable (requests answer 503) and is never replaced. The keys are
// secrets: they are never logged, never put into an error, and leave this
// class only through IssueCredential() (once, for the enrollment session's
// POST answer).
//
// Threading: one mutex guards everything. HandleDatagram() runs on the
// ingest receive thread; the request methods on HTTP worker threads; the
// clean-up worker on its own thread. The Registry is called with that mutex
// held (it never calls back). The hooks in PowerBaseManagementConfig are
// called with NO lock held -- in particular the settings write of a clean-up
// never happens under this class's mutex -- with two exceptions that must not
// block and say so: `ingest_endpoint` and `read_rooms`.
//
// The Registry and this class change together in one respect: which Power
// Bases are listed. This class takes a Base (and its modules) OUT of the
// Registry -- a release, a list removal -- with its mutex held, and
// DescribeForSnapshot() reads this class's state and makes sure the Registry
// copy it is given is not from before the last of those, in the same critical
// section. A snapshot therefore never shows a Base together with the record
// that took it out of the list. (A new generation becoming active takes
// nothing out of the Registry: a blocked Base has no entry in it.)
#pragma once

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "ingest/registry.hpp"
#include "ingest/session.hpp"
#include "pogohome_api/room_cleanup.hpp"

extern "C" {
#include "power_base_mgmt/pb_mgmt_proto.h"
}

namespace pogohome_api {

constexpr const char kPowerBaseMgmtFileName[] = "power_base_mgmt.json";
// Schema 1 (never released) called a removal "completed" on the board's
// released notice. Such a file is still read: those jobs become "released"
// (the erase was never confirmed), and the next write is schema 2.
constexpr int kPowerBaseMgmtFileSchema = 2;
// A stored file larger than this is treated as damaged.
constexpr size_t kMaxPowerBaseMgmtFileBytes = 256 * 1024;
// Jobs that still wait for the board (pending, sent, accepted). A request
// beyond this is refused, never queued. Released jobs are not counted here:
// each of them has a block, and those are bounded by kMaxBlockedPowerBases.
constexpr size_t kMaxActiveRemovals = 32;
// Final jobs kept so that every device can still read how one ended.
constexpr size_t kMaxFinishedRemovals = 32;
constexpr size_t kMaxManagementCredentials = 96;
// Blocked Power Bases, including the slots reserved by removals that may
// still lead to a block. Never evicted; see the top comment.
constexpr size_t kMaxBlockedPowerBases = 64;
// Modules remembered per clean-up (a Base's chain is far shorter).
constexpr size_t kMaxCleanupModules = 64;
// The "members" list: Power Bases with modules written down, and modules in
// all of them together (the Registry itself holds at most
// ingest::Registry::kMaxNodes nodes). What does not fit is not written down
// -- and is then not cleaned after a restart; nothing is dropped to make room.
constexpr size_t kMaxMemberPowerBases = 96;
constexpr size_t kMaxMemberModules = 256;
// What the enrollment API and the health field report.
constexpr int kPowerBaseManagementVersion = 1;
// The "remove from this hub's list" API (health: "power_base_forget").
constexpr int kPowerBaseForgetVersion = 1;
// What a removal request has to say (health: "power_base_removal_request").
// 2: the registration it was confirmed for (`record`), how many removals had
// been asked of that registration then (`removal_serial`) and an id of the
// request itself -- see RequestRemoval(). 1 was the EUI-64 alone, which names
// no registration: this hub does not carry such a request out.
constexpr int kPowerBaseRemovalRequestVersion = 2;
// List removals kept for reading back. One that still owes its settings
// clean-up is never dropped; when none can be dropped a request is refused.
constexpr size_t kMaxForgetRecords = 64;
// What this hub says of itself in HELLO_ACK. A hub that was told where its
// ingest listener is (PowerBaseManagementConfig::ingest_endpoint) adds
// PB_MGMT_HUB_CAP_ENDPOINT_LOCATE.
constexpr uint32_t kHubManagementCapabilities = PB_MGMT_HUB_CAP_THREAD_RECEIPT;

enum class RemovalState {
    Pending,    // stored; the command was never put into an answer
    Sent,       // the command went out in an answer to an authenticated poll; arrival unknown
    Accepted,   // the board's authenticated UNENROLL_ACCEPTED arrived
    // The board's authenticated "released" notice arrived: its enrollment is
    // out of use for good and the hub has removed it. NOT final: whether the
    // board erased its stored join information is not confirmed.
    Released,
    Completed,  // final: the board's authenticated release receipt (erased and verified) was checked
    Cancelled,  // final: cancelled while Pending
    Superseded, // final: a new enrollment generation of the same board became active first
};

const char *RemovalStateName(RemovalState state);
bool IsFinalRemovalState(RemovalState state);

// How the erase of a removal that is not final yet is expected to be
// confirmed. A hint for what an app shows and offers -- it changes no state,
// and a receipt completes the removal whichever way it comes in.
enum class EraseConfirmation {
    None, // the removal is final: nothing is expected any more
    // The board announced that it sends its receipt itself, and the time it
    // needs for that has not run out: nothing for anyone to do but wait.
    Automatic,
    // The board never announced that (older firmware), or it did and the
    // receipt did not arrive in time: someone has to collect the receipt from
    // the board over BLE (or enroll the board again).
    Manual,
};

const char *EraseConfirmationName(EraseConfirmation confirmation);

// The credential of one enrollment, as the session's POST answer carries it.
// `key` is a secret.
struct ManagementGrant {
    int version = kPowerBaseManagementVersion;
    std::string hub_id;     // 32 lowercase hex
    std::string generation; // 32 lowercase hex
    std::string key;        // 64 lowercase hex
};

struct RemovalJob {
    std::string removal_id;      // 32 lowercase hex
    std::string power_base_uuid; // 16 lowercase hex
    std::string generation;      // 32 lowercase hex
    RemovalState state = RemovalState::Pending;
    // Sent or Accepted without progress for longer than
    // PowerBaseManagementConfig::result_unknown_after. A display hint: the
    // state is unchanged and a late proof still completes the job.
    bool result_unknown = false;
    bool cancellable = false; // exactly while Pending
    int64_t created_at_unix_ms = 0;
    int64_t updated_at_unix_ms = 0;
    int64_t completed_at_unix_ms = 0; // 0 unless Completed
    std::string requested_by_device_id;
    // Modules registered under the Base when the job was created
    // (informational; what is removed is decided when the board reports).
    int affected_module_count = 0;
    // The room assignments of the modules that left with the Base are still
    // to be cleared in the shared settings (retried until done).
    bool settings_cleanup_pending = false;
    // Filled in on copy, like result_unknown. None exactly for a final job.
    EraseConfirmation erase_confirmation = EraseConfirmation::None;
    // Filled in on copy. The Base was taken out of this hub's list
    // (ForgetPowerBase()) while this job's command may be with the board (Sent
    // or Accepted): the LIST is settled -- the Base is not listed and its data
    // is refused -- and the BOARD is not: whether it erased anything is as
    // unknown as before. The job, its key and the block are kept, so that the
    // board's proof is still verified should it ever arrive; nothing here
    // turns that into "completed". False for every other job.
    bool power_base_list_removed = false;
    // The id of the request that created this job (32 lowercase hex, chosen
    // by the device that asked): the same request again is this job, whatever
    // has become of it. Empty for a job stored by a hub that did not write it
    // down.
    std::string request_id;
};

enum class RemovalRequestOutcome {
    Created,
    // Nothing was created. Either this very request was made before (its
    // request id): `out` is the job it created then, in whatever state that is
    // now. Or a removal of the registration the request names is under way
    // already, whoever asked for it: that one.
    Existing,
    UnknownPowerBase,
    // The request is not about what this hub lists now: the Base is listed
    // under another registration than `record` (it was registered again, or a
    // registration was started), or a removal was asked of that registration
    // since the device read its `removal_serial` (and has ended: cancelled).
    // Nothing was created and no command will be given -- a request never
    // reaches a registration, or a state of one, that nobody confirmed.
    StaleRecord,
    // The request id was used for another Base.
    RequestConflict,
    Unsupported,          // no active management credential for this Base
    EnrollmentInProgress, // an enrollment session for this EUI-64 is running
    TooMany,
    // No free slot is left for the block this removal would end in
    // (kMaxBlockedPowerBases). Nothing was stored and no command will be
    // given. A slot is freed only when a removed board is enrolled again.
    CapacityFull,
    Unavailable, // the state file is damaged or cannot be written
};

enum class ReceiptOutcome {
    Completed,        // the receipt verified and completed the removal
    AlreadyCompleted, // the same receipt again: nothing changed
    Malformed,        // not an UNENROLL_DONE/ERASED message of this protocol
    // Well formed, but not a receipt this hub can verify for one of its
    // removals (another hub, an unknown or superseded generation, a wrong
    // MAC, a cancelled job). One answer for all of them.
    Rejected,
    Unavailable,
};

enum class RemovalCancelOutcome {
    Cancelled,
    NotCancellable, // the command may already have reached the board, or the job is final
    NotFound,
    Unavailable,
};

// One "removed from this hub's list" (ForgetPowerBase()). Always final: the
// operation is carried out when it is asked for, or refused.
// A Power Base that is NOT in this hub's list now because its registration
// here ended and it has not been registered again: exactly the blocked Bases.
// This is the hub's present state, not a piece of history -- it is in every
// snapshot for as long as that is so and gone with the next registration,
// however long ago the registration ended and whatever happened to the
// removal job or the list-removal record that ended it (those are shown for a
// while and then dropped: how long a finished job is DISPLAYED and whether a
// registration IS over are two different things). A device that was away for
// an hour finds here, in the very snapshot that no longer lists the Base,
// that the Base is out of the list -- and not merely not heard from yet.
struct PowerBaseDeparture {
    std::string power_base_uuid; // 16 lowercase hex
    // The registration that ended (what `record` was for the listed Base; a
    // removal job's `generation`). Empty: not known -- the block was stored
    // by a hub that did not write it down.
    std::string record;
    // "removed": the board released the registration (a removal's notice or
    // receipt). "forgotten": it was taken out of this hub's list while
    // offline; nothing says the board erased anything. Empty: not known.
    std::string reason;
    int64_t since_unix_ms = 0;
};

struct ForgetRecord {
    std::string forget_id;       // 32 lowercase hex, issued by this hub
    std::string request_id;      // 32 lowercase hex, chosen by the device that asked
    std::string power_base_uuid; // 16 lowercase hex
    // The registration the request named (see PowerBaseRecords()).
    std::string record; // 32 lowercase hex
    // The hub held a management credential for it (false: a Base enrolled
    // without one, known from its data only).
    bool managed = false;
    int64_t created_at_unix_ms = 0;
    std::string requested_by_device_id;
    // Modules registered under the Base at that moment.
    int affected_module_count = 0;
    // A removal of this Base that was still Pending (its command never given)
    // ended as Cancelled in the same stored change. Empty: none.
    std::string cancelled_removal_id;
    // A removal whose command may be with the board (Sent or Accepted) was
    // left exactly as it is -- job and key -- so that the board's proof is
    // still verified should it ever arrive. Empty: none.
    std::string kept_removal_id;
    // Filled in on copy: the rooms of the modules that left are still to be
    // cleared in the shared settings (retried until done).
    bool settings_cleanup_pending = false;
};

enum class ForgetOutcome {
    Removed,
    Existing, // the same request (request_id) again: that record, nothing changed
    // This hub does not list such a Base: never heard of, already removed
    // from the list, or released by a removal.
    UnknownPowerBase,
    // The Base is heard from right now (or one of its modules is). Use the
    // removal instead; nothing changed.
    Online,
    // `record` is not the registration this hub lists for the Base now: it
    // was registered again, or a new registration was started, after the
    // device read the list. Nothing changed.
    StaleRecord,
    EnrollmentInProgress, // an enrollment session for this EUI-64 is running
    // The request_id was used for another Base.
    RequestConflict,
    // No block slot is free (kMaxBlockedPowerBases): nothing is evicted to
    // make room, nothing changed.
    CapacityFull,
    TooMany,     // kMaxForgetRecords, none of which can be dropped yet
    Unavailable, // the state file is damaged or cannot be written: nothing changed
};

struct PowerBaseManagementConfig {
    // <--state-dir>/pogohome; must already exist (EnsurePrivateDirectory).
    std::string directory;
    // This hub's identity, 32 lowercase hex (hub_identity.hpp).
    std::string hub_id;
    std::function<int64_t()> now_unix_ms;
    std::chrono::milliseconds result_unknown_after{5 * 60 * 1000};
    std::chrono::milliseconds retain_finished{10 * 60 * 1000};
    std::chrono::milliseconds hub_nonce_ttl{60 * 1000};
    // How long after its "released" notice a board that sends its receipt
    // itself is given before the removal is reported as needing a manual
    // confirmation. The board needs at most about 90 s after the notice was
    // answered (attach 45 s, five attempts with 30 s of backoff), and up to
    // 75 s more when the notice's answer was lost. A display hint only: a
    // receipt that arrives later still completes the removal.
    std::chrono::milliseconds automatic_confirmation_window{4 * 60 * 1000};
    // Whether an enrollment session for this EUI-64 is running right now.
    // Called without this class's lock.
    std::function<bool(const std::string &eui64)> enrollment_in_progress;
    // Clears, in the shared settings, the room assignments a clean-up names
    // (room_cleanup.hpp) -- with the write itself made through
    // `still_owed.commit()`: the section in which this class decides which of
    // them are still that clean-up's, and in which no report can list a
    // module under a Power Base until the write has returned. True when that
    // is done or there was nothing to do; false when it could not be done now
    // (it is retried). Called with no lock of this class held, possibly more
    // than once for the same modules: it must be idempotent.
    //
    // Locks: commit() takes this class's lock and then the Registry's and
    // keeps both while the write it is given runs -- that write may take the
    // settings store's lock (the order is always this class, the Registry,
    // the settings store; `read_rooms` below takes the last under the
    // first). So the hook must not call commit() while it holds the settings
    // store's lock or any lock a settings writer waits for, and the write it
    // hands to commit() must not call into this class or the Registry.
    // Unset: clean-ups stay pending.
    RoomUnassigner unassign_rooms;
    // The room each module is in at the moment its Base leaves the list
    // (room_cleanup.hpp): what the clean-up is then bound to. Called under
    // this class's lock: it must not block (production copies the settings
    // held in memory). Unset, or false: the rooms are not known, and the
    // clean-up clears whatever room the module has -- still only while it is
    // owed.
    RoomReader read_rooms;
    // Retry of a failed clean-up: first after `cleanup_retry_min`, doubling
    // up to `cleanup_retry_max`.
    std::chrono::milliseconds cleanup_retry_min{5 * 1000};
    std::chrono::milliseconds cleanup_retry_max{10 * 60 * 1000};
    // A clean-up found in the state file at startup waits this long before
    // its first attempt: after a restart the Registry is empty until the
    // Power Bases have reported again, and a module that meanwhile moved to
    // another Base must be seen there before its room is touched.
    std::chrono::milliseconds cleanup_startup_grace{120 * 1000};
    // WriteFileAtomically in production; tests substitute a failing writer.
    std::function<bool(const std::string &path, const std::string &contents, std::string &error)> writer;
    // Where this hub's ingest listener is reachable in the Thread network
    // RIGHT NOW: the OTBR's Mesh-Local EID (16 bytes) and the ingest port.
    // Asked for every LOCATE (docs/power-base-management.md, section 12), on
    // the ingest thread and under this class's lock: it must not block
    // (production reads the Thread interface's addresses with getifaddrs()).
    // False when it cannot be told (the interface has no Thread addresses):
    // LOCATE is then answered 5.03. Unset: this hub does not answer LOCATE
    // (5.03) and does not announce PB_MGMT_HUB_CAP_ENDPOINT_LOCATE.
    std::function<bool(std::array<uint8_t, 16> &address, uint16_t &port)> ingest_endpoint;
    // The moment "is this Base heard from?" is asked for (ForgetPowerBase()).
    // Unset: std::chrono::steady_clock::now(). Tests move it.
    std::function<std::chrono::steady_clock::time_point()> steady_now;
};

class PowerBaseManagement {
public:
    PowerBaseManagement(ingest::Registry &registry, PowerBaseManagementConfig config);
    ~PowerBaseManagement();
    PowerBaseManagement(const PowerBaseManagement &) = delete;
    PowerBaseManagement &operator=(const PowerBaseManagement &) = delete;

    // Reads the state file once, at startup, and applies its blocked list to
    // the Registry. A missing file is an empty state. False (with `error`)
    // when the file exists but is damaged: the feature is then unavailable
    // and the file is left alone.
    bool Load(std::string &error);
    bool available() const;
    std::string path() const { return config_.directory + "/" + kPowerBaseMgmtFileName; }

    // ---- enrollment sessions ----

    // A new generation and key for `eui64` (16 lowercase hex), stored as its
    // provisional credential (replacing an earlier provisional one) before
    // this returns. False when unavailable, when randomness fails or when
    // the state cannot be stored: then nothing was issued.
    bool IssueCredential(const std::string &eui64, ManagementGrant &out);
    // The Base was unenrolled and no new generation of it is active yet.
    bool IsBlocked(const std::string &eui64) const;

    // ---- the ingest receive thread ----

    // One "pogo/v1/mgmt" request payload. See the top comment.
    ingest::ManagementReply HandleDatagram(const std::vector<uint8_t> &request);
    // The same for a request that arrived on the locate port (the hub's
    // ingest listener is on another port; ingest/server.hpp): LOCATE is
    // handled exactly as by HandleDatagram(); for anything else the reply is
    // `silent` -- nothing is answered, nothing is looked at, nothing changes.
    ingest::ManagementReply HandleLocateDatagram(const std::vector<uint8_t> &request);

    // ---- the HTTP API ----

    // Asks for the removal (unenroll) of ONE registration of `eui64`: the one
    // a device was shown and confirmed.
    //   record          what the hub listed for the Base then (RemovalBinding(),
    //                   the snapshot's `record`).
    //   removal_serial  how many removals had been asked of that registration
    //                   then (RemovalBinding(), the snapshot's `removal_serial`).
    //   request_id      32 lowercase hex the device chose for this confirmation.
    //                   The same request again -- its answer was lost, or it is
    //                   delivered twice or late -- is the same id.
    //
    // A job is created only when all of this is still so: the Base is listed
    // under `record`, no removal of it is under way, and `removal_serial` is
    // the present one. Then the serial moves on, in the same stored change.
    // So a request can never create a job for a registration other than the
    // one it names (a Base that was removed and registered again meanwhile:
    // StaleRecord), and it can create at most one job, ever: once it has, the
    // serial it carries is spent. That holds without the job being remembered
    // -- after the finished job was dropped from the list, after a restart --
    // because it rests on the registration and its serial, which are kept for
    // as long as the registration is, and not on a list of past requests.
    //
    // While the job a request created is still stored (not final, or final and
    // kept for display), the same request id is answered with that job
    // (Existing): cancelled, completed or superseded as it may be, and nothing
    // new.
    RemovalRequestOutcome RequestRemoval(const std::string &device_id, const std::string &eui64,
                                         const std::string &record, uint32_t removal_serial,
                                         const std::string &request_id, RemovalJob &out);
    // What a removal request for `eui64` has to name right now: the
    // registration this hub lists for it and that registration's removal
    // serial. False when this hub cannot remove the Base (it lists none, or
    // holds no active credential for it).
    bool RemovalBinding(const std::string &eui64, std::string &record, uint32_t &removal_serial);
    bool GetRemoval(const std::string &removal_id, RemovalJob &out);
    // Not final first (oldest first), then final (most recent first).
    std::vector<RemovalJob> ListRemovals();
    RemovalCancelOutcome CancelRemoval(const std::string &removal_id, RemovalJob &out);
    // The board's release receipt (the complete UNENROLL_DONE/ERASED
    // message, PB_MGMT_UNENROLL_DONE_LEN bytes), as an app read it from the
    // board over BLE. The only thing that completes a removal. `out` is the
    // job when the outcome is Completed or AlreadyCompleted.
    ReceiptOutcome SubmitReleaseReceipt(const std::vector<uint8_t> &receipt, RemovalJob &out);

    // ---- removing a Power Base from this hub's list ----

    // The registration this hub lists for each of these Bases (EUI-64, 16
    // lowercase hex), as (EUI-64, record): 32 lowercase hex that name one
    // registration and are no secret. For a Base with a credential it is the
    // generation of its newest one (a registration that was started counts);
    // for a Base known from its data only, a value this process made up when
    // it first listed it (gone with a restart, like that entry itself). A
    // blocked Base has none. What a device shows in its confirmation and
    // sends back with ForgetPowerBase().
    std::vector<std::pair<std::string, std::string>> PowerBaseRecords(const std::vector<std::string> &eui64s);
    // Takes `eui64` out of this hub's list: see the top comment. `record` is
    // what PowerBaseRecords() gave for it, `request_id` 32 lowercase hex the
    // caller chose (the same id again is the same answer). `out` is the
    // record when the outcome is Removed or Existing.
    //
    // A removal job of that Base that is not final:
    //   Pending             becomes Cancelled in the same stored change (its
    //                       command was never given: this is that cancel).
    //   Sent, Accepted      is left as it is, with its key: the command may be
    //                       with the board, and nothing here may say it is
    //                       not. The Base leaves the list all the same; the
    //                       job stays listed until the board's proof arrives
    //                       or the board is registered again.
    //   Released            the Base is not listed any more (UnknownPowerBase).
    ForgetOutcome ForgetPowerBase(const std::string &device_id, const std::string &eui64, const std::string &record,
                                  const std::string &request_id, ForgetRecord &out);
    bool GetForget(const std::string &forget_id, ForgetRecord &out);
    // Most recent first: those still owed their clean-up, those as recent as
    // a finished removal that is kept, and for every Base that is still out
    // of the list the one that took it out.
    std::vector<ForgetRecord> ListForgets();

    // ---- what a snapshot says of management ----

    // Everything GET /api/v1/snapshot and the SSE snapshot say of management,
    // read in ONE critical section.
    struct Description {
        // ManagedPowerBases() and SelfConfirmingPowerBases().
        std::vector<std::string> managed_power_bases;
        std::vector<std::string> self_confirming_power_bases;
        std::vector<RemovalJob> removals; // ListRemovals()
        // PowerBaseRecords() of every Base that is listed: the managed ones
        // and those in `view`.
        std::vector<std::pair<std::string, std::string>> records;
        // The removal serial of every managed Base (RemovalBinding()).
        std::vector<std::pair<std::string, uint32_t>> removal_serials;
        std::vector<ForgetRecord> forgets; // ListForgets()
        // The Bases that are out of the list (blocked), each with the
        // registration that ended. None of them is in `view` or managed.
        std::vector<PowerBaseDeparture> departures;
    };
    // Describes this class's state for a snapshot that lists what `view`
    // holds, and makes the two agree: a copy that was made before this class
    // last took a Base out of the Registry (or before the last copy it
    // looked at) is replaced by one made now, under this class's lock -- so
    // `view` never shows a Base that a record in `out` took out of the list.
    // Also notes where every module is (the "members" list): what a snapshot
    // shows is written down first.
    // False when management is unavailable: nothing was touched.
    bool DescribeForSnapshot(ingest::RegistryView &view, Description &out);

    // The Power Bases that are out of this hub's list (see
    // PowerBaseDeparture), oldest first. At most kMaxBlockedPowerBases.
    std::vector<PowerBaseDeparture> ListDepartures() const;

    // ---- clean-ups of the shared settings ----

    // One pass over the clean-ups that are due. First where every module is
    // NOW: one that is listed under a Base at this moment -- it moved before
    // the removal, was docked elsewhere since, or its Base was registered
    // again -- leaves every clean-up for good (its room is that
    // registration's). So does one that was listed under a Base at any time
    // since the clean-up was noted, whether or not this class looked while it
    // was (the Registry remembers when each module was last newly listed).
    // What is left is handed to `unassign_rooms` together with a guard
    // (room_cleanup.hpp): its section decides the same again and lets the
    // settings be written while the Registry is held still -- a module cannot
    // be listed under a Base between that decision and the write. The outcome is
    // stored. Returns how many clean-ups are still pending. Called by the
    // worker; tests call it directly.
    size_t ProcessSettingsCleanups();
    size_t PendingSettingsCleanups() const;
    // Starts a thread that calls ProcessSettingsCleanups() every `interval`
    // (and right after a removal was released). Stopped by the destructor.
    void StartCleanupWorker(std::chrono::milliseconds interval = std::chrono::milliseconds(5000));

    // How many more removals can be requested before CapacityFull.
    size_t FreeBlockSlots() const;

    // EUI-64s (16 lowercase hex) this hub holds an ACTIVE credential for and
    // lists: the Bases that can be unenrolled remotely. Not one that was
    // taken out of the list while a removal's command may be with its board
    // (its key is kept for that job alone).
    std::vector<std::string> ManagedPowerBases() const;
    bool IsManaged(const std::string &eui64) const;
    // Those of them whose board announced (HELLO) that it sends its release
    // receipt itself.
    std::vector<std::string> SelfConfirmingPowerBases() const;

private:
    enum class CredentialState { Provisional, Active, Released };

    struct Credential {
        ingest::Uuid eui{};
        std::array<uint8_t, 16> generation{};
        std::array<uint8_t, 32> key{};
        CredentialState state = CredentialState::Provisional;
        int64_t issued_at_unix_ms = 0;
        int64_t activated_at_unix_ms = 0;
        // What the board last announced with HELLO (PB_MGMT_BOARD_CAP_*);
        // 0 for a board that never did. Not a secret.
        uint32_t board_capabilities = 0;
        // How many removal jobs were created for this registration. A request
        // names the value it was confirmed against, and a job is created only
        // for the present one (RequestRemoval()). Kept with the credential, so
        // for exactly as long as the registration exists; 0 in a file written
        // by a hub that did not count.
        uint32_t removal_serial = 0;
    };

    // A credential copied out of state_ for signing an answer, wiped when it
    // goes out of scope (state_ may be replaced while a handler runs).
    struct SigningCopy {
        explicit SigningCopy(const Credential &c) : credential(c) {}
        ~SigningCopy();
        SigningCopy(const SigningCopy &) = delete;
        SigningCopy &operator=(const SigningCopy &) = delete;
        Credential credential;
    };

    struct Job {
        RemovalJob job; // result_unknown / cancellable / settings_cleanup_pending are filled in on copy
        ingest::Uuid eui{};
        std::array<uint8_t, 16> generation{};
        std::array<uint8_t, 16> command_id{};
        int64_t progress_at_unix_ms = 0; // last state change
        // Completed only: the MAC of the receipt that completed it, so the
        // same receipt is recognised again after the key was dropped.
        std::array<uint8_t, 32> receipt_mac{};
        // Completed only: the MAC of the acknowledgement that receipt was
        // given (RESULT_ACK of UNENROLL_DONE/ERASED), computed while the key
        // still existed. Zeros for a job completed by a hub that did not keep
        // it: the same receipt on the management resource is then refused.
        std::array<uint8_t, 32> receipt_ack_mac{};
    };

    struct Cleanup {
        std::string removal_id;
        // The registration this clean-up belongs to: the Base and the
        // generation (for a list removal, the record) that left the list.
        ingest::Uuid eui{};
        std::array<uint8_t, 16> generation{};
        std::vector<ingest::Uuid> modules;
        // The room each module was in when the Base left the list, in the
        // order of `modules`. Empty: not known (the settings could not be
        // read then, or the entry was stored by a hub that did not write it).
        std::vector<std::string> rooms;
        int attempts = 0;
        int64_t next_attempt_unix_ms = 0;
        // RAM only: the start-up grace applies. Read from the file at
        // startup, or resting on members that were read from the file and
        // that this process has not seen in the Registry itself.
        bool from_file = false;
        // RAM only: ingest::Registry::generation() of the Registry copy this
        // clean-up was noted from; 0 for one read from the file (noted
        // before this process started). A module the Registry newly listed
        // under a Power Base after it is not this clean-up's.
        uint64_t noted_generation = 0;
    };

    // The modules the Registry last had under one listed Power Base.
    struct Member {
        ingest::Uuid eui{};
        std::vector<ingest::Uuid> modules;
        // RAM only: those of them that were read from the file and that this
        // process has not seen under this Base in the Registry (yet).
        std::vector<ingest::Uuid> unconfirmed;
    };

    // Why a Base is blocked: which registration ended, and how. Written down
    // with the block itself (the same file write); "Unknown"/empty for a
    // block a hub stored that did not.
    enum class BlockReason { Unknown, Released, Forgotten };
    struct Blocked {
        ingest::Uuid eui{};
        int64_t since_unix_ms = 0;
        BlockReason reason = BlockReason::Unknown;
        std::string record; // 32 lowercase hex, or empty
    };

    struct HubNonce {
        ingest::Uuid eui{};
        std::array<uint8_t, 16> generation{};
        std::array<uint8_t, 16> nonce{};
        std::chrono::steady_clock::time_point issued_at{};
    };

    struct Forget {
        ForgetRecord record; // settings_cleanup_pending is filled in on copy
        ingest::Uuid eui{};
    };

    struct State {
        std::vector<Credential> credentials;
        std::vector<Job> jobs;
        std::vector<Blocked> blocked;
        std::vector<Cleanup> cleanups;
        std::vector<Forget> forgets;
        std::vector<Member> members;
    };

    int64_t NowUnixMs() const;
    // Caller holds mutex_. Serializes `state` and writes it; false leaves the
    // file as it was. A state that would not fit the file this class reads
    // back (kMaxPowerBaseMgmtFileBytes) is not written.
    bool StoreLocked(const State &state);
    static std::string Serialize(const State &state);
    static bool Parse(const std::string &text, State &out, std::string &error);
    RemovalJob CopyJobLocked(const Job &job) const;
    ForgetRecord CopyForgetLocked(const Forget &forget) const;
    // The registration listed for `eui` (see PowerBaseRecords()); empty when
    // there is none and `issue` is false, or randomness failed.
    std::string RecordLocked(const ingest::Uuid &eui, bool issue);
    static bool IsBlockedLocked(const State &state, const ingest::Uuid &eui);
    static void PruneFinishedLocked(State &state, int64_t now_unix_ms, int64_t retain_ms);
    // Whether `forget` is why its Base is not listed NOW: the Base is still
    // blocked (it was not registered again) and the block says it was made by
    // a list removal of the registration this record is about. For a block
    // that does not say (stored by an earlier hub): this is the last list
    // removal stored for that Base. Such a record is listed and kept for as long as that is
    // so, however old it is: a device that was away when the Base was taken
    // out -- an app in the background for an hour -- still finds, in the very
    // snapshot that no longer lists the Base, the record that says why. At
    // most one per blocked Base, so never more than kMaxBlockedPowerBases.
    static bool ForgetExplainsBlockLocked(const State &state, const Forget &forget);
    // Blocks that exist plus removals that may still end in one.
    static size_t UsedBlockSlots(const State &state);
    // The hub-side effects of "the board released this enrollment", written
    // into `next`: the Base is blocked, the credential marked released, the
    // modules that leave with it noted for the settings clean-up (from
    // `view`, a Registry copy ObserveLocked() has taken in). False when no
    // block slot is left (an invariant violation: nothing is evicted).
    bool PrepareReleaseLocked(State &next, const Job &job, int64_t now_unix_ms, const ingest::RegistryView &view);
    // After `next` was stored: the Registry follows.
    void ApplyReleaseToRegistryLocked(const ingest::Uuid &eui);
    // Caller holds mutex_, after a Base was taken out of the Registry: wakes
    // the Registry's readers and remembers that every Registry copy made
    // before this moment is from before that change.
    void NoteListChangeLocked();

    // ---- where every module belongs ("members") ----

    // Caller holds mutex_. Takes in a Registry copy: every module it lists
    // under a Base belongs to that Base now (and to no other, and to no
    // clean-up: it leaves every clean-up that names it); a node this process
    // saw under a listed Base and that is gone now was detached there. Works
    // on state_ itself -- these are observations, not decisions -- and is
    // stored by the next write of any kind, or by FlushObservationLocked().
    // A copy older than the last one taken in is ignored. True when a
    // clean-up ended by this (none of its modules is left): its readers are
    // to be told (Registry::notify_external_change()).
    bool ObserveLocked(const ingest::RegistryView &view);
    // Caller holds mutex_. Stores what ObserveLocked() changed, if anything.
    // After a failed write it waits before the next attempt, doubling from
    // `cleanup_retry_min` up to `cleanup_retry_max`.
    void FlushObservationLocked();
    // Caller holds mutex_. The clean-up of `eui`'s leaving the list, written
    // into `next`: the modules the Registry copy has under it and under no
    // other Base, and those written down as its members that no other Base
    // lists; each with the room it is in now when that can be read. The
    // members of `eui` end here. `known_modules`: how many modules were
    // known under the Base (with or without a room).
    void NoteCleanupLocked(State &next, const std::string &removal_id, const ingest::Uuid &eui,
                           const std::array<uint8_t, 16> &generation, const ingest::RegistryView &view,
                           int64_t now_unix_ms, size_t *known_modules);
    // Caller holds mutex_.
    std::vector<std::string> ManagedLocked(bool self_confirming_only) const;
    std::vector<RemovalJob> ListRemovalsLocked() const;
    std::vector<ForgetRecord> ListForgetsLocked() const;
    std::vector<PowerBaseDeparture> ListDeparturesLocked() const;
    // Completes `job_id` with a verified receipt (credential dropped).
    ingest::ManagementReply CompleteWithReceiptLocked(const std::string &removal_id,
                                                      const std::array<uint8_t, 32> &receipt_mac,
                                                      const Credential &cred, bool &completed);
    void WakeCleanupWorker();
    void CleanupWorkerLoop(std::chrono::milliseconds interval);
    Credential *FindCredentialLocked(const ingest::Uuid &eui, const std::array<uint8_t, 16> &generation);
    Job *FindOpenJobLocked(const ingest::Uuid &eui);

    // Each returns the reply. `released` is set when the Base was released
    // by this message (the caller wakes the clean-up after the lock is gone).
    ingest::ManagementReply HandlePollLocked(const pb_mgmt_msg_t &msg, Credential &cred);
    ingest::ManagementReply HandleAcceptedLocked(const pb_mgmt_msg_t &msg, Credential &cred);
    ingest::ManagementReply HandleDoneLocked(const pb_mgmt_msg_t &msg, const uint8_t *wire, size_t wire_len,
                                             Credential &cred, bool &released);
    ingest::ManagementReply HandleHelloLocked(const pb_mgmt_msg_t &msg, Credential &cred);
    // LOCATE: "where does my hub take my messages now?". Answered for a
    // provisional or an active credential with this hub's present ingest
    // endpoint and the request's nonce, under that credential's key. Reads
    // only: no state, no nonce and no file is touched, so a repeated or
    // replayed LOCATE gets a true answer and changes nothing.
    ingest::ManagementReply HandleLocateLocked(const pb_mgmt_msg_t &msg, const Credential &cred) const;
    // The receipt of an already completed removal, on the management
    // resource again (the board did not get the answer). The key is gone;
    // the receipt is known by its MAC and gets the stored acknowledgement.
    ingest::ManagementReply RepeatedReceiptLocked(const pb_mgmt_msg_t &msg, const uint8_t *wire, size_t wire_len) const;
    EraseConfirmation EraseConfirmationLocked(const Job &job, bool result_unknown) const;
    // Caller holds mutex_. A signed answer of `type` for `cred`.
    ingest::ManagementReply SignedReplyLocked(pb_mgmt_msg_t &answer, const Credential &cred) const;

    ingest::Registry &registry_;
    const PowerBaseManagementConfig config_;
    std::array<uint8_t, 16> hub_id_{};
    bool hub_id_ok_ = false;

    mutable std::mutex mutex_;
    bool available_ = false;
    State state_;
    std::vector<HubNonce> nonces_; // RAM only
    // RAM only: the records made up for Bases without any credential.
    std::vector<std::pair<ingest::Uuid, std::string>> unmanaged_records_;
    int64_t loaded_at_unix_ms_ = 0;
    // RAM only. ingest::Registry::generation() right after this class last
    // changed which Bases are listed: a Registry copy with a smaller
    // generation was made before that change.
    uint64_t list_changed_generation_ = 0;
    // RAM only: the generation of the Registry copy ObserveLocked() took in
    // last, and the (Base, module) nodes it had.
    uint64_t observed_generation_ = 0;
    bool observed_any_ = false;
    std::vector<std::pair<ingest::Uuid, ingest::Uuid>> observed_nodes_;
    // RAM only: state_ holds observations that are not in the file yet; how
    // many attempts to write them failed in a row, and when the last one did.
    bool observation_dirty_ = false;
    int observation_store_failures_ = 0;
    int64_t observation_store_failed_at_unix_ms_ = 0;

    // The clean-up worker. `worker_mutex_` guards only its wake-up.
    std::thread worker_;
    std::mutex worker_mutex_;
    std::condition_variable worker_cv_;
    bool worker_stop_ = false;
    bool worker_wake_ = false;
    // One ProcessSettingsCleanups() at a time (worker and callers).
    std::mutex cleanup_run_mutex_;
};

// One job as the API reports it (docs/power-base-management.md, section 7).
std::string RemovalJobJson(const RemovalJob &job);
// {"removals":[...]}
std::string RemovalListJson(const std::vector<RemovalJob> &jobs);
// One list removal as the API reports it (section 13).
std::string ForgetRecordJson(const ForgetRecord &record);
// The JSON array ("[...]") of a snapshot's `power_base_departures`.
std::string DeparturesJson(const std::vector<PowerBaseDeparture> &departures);
// {"forgets":[...]}
std::string ForgetListJson(const std::vector<ForgetRecord> &records);

} // namespace pogohome_api
