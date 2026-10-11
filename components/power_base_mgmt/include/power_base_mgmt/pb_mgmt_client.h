/*
 * Board side of Power Base management (docs/power-base-management.md,
 * sections 4 and 6): the authenticated poll of the hub, the checks an
 * "unenroll" command must pass, and the release itself, in the one order
 * that survives a power cut at any point.
 *
 * This component decides and stores; it never calls OpenThread, CoAP or
 * FreeRTOS itself. power_base/main/enrollment_mgmt.c supplies those through
 * pb_mgmt_ops_t, so the same code runs unmodified in the host tests
 * (tools/test_power_base_mgmt.c against the NVS simulator and a scripted hub;
 * linux/matter_bridge/app/api/tests against the real hub code).
 *
 * Where it runs: one task at a time. pb_mgmt_client_poll() is called from the
 * management task (never from a CoAP or OpenThread callback: `exchange`
 * blocks, and the release writes flash); pb_mgmt_client_resume_release() from
 * the boot path, before any service exists.
 *
 * Four different things happen in a release, and none is taken for another:
 *
 *   accepted   the command passed every check and is journaled
 *              (UNENROLLING). The hub is told with UNENROLL_ACCEPTED.
 *   released   the record says this enrollment is out of use for good
 *              (RELEASED, erase pending): no boot ever resumes it. The hub
 *              is told over Thread, which still works, with
 *              UNENROLL_DONE/RELEASED. That is a notice, NOT a completion:
 *              the stored join information (the Thread Dataset) is still
 *              there.
 *   erased     Thread is stopped, OpenThread's persistent store is erased,
 *              the store is read back and found empty, and only then the
 *              final record is written and read back. A failed erase, a
 *              store that is not empty afterwards or a final record that
 *              cannot be written all leave "erase pending": the next boot
 *              does the step again, and nothing claims it happened.
 *   receipt    the UNENROLL_DONE/ERASED message. It is computed only inside
 *              the "erased" step, after the checks, and exists nowhere
 *              before. It stays in the final record (the key is wiped in
 *              the same write) and is handed out over BLE
 *              (pb_mgmt_receipt_load(), pb_ble_proto.h) for an app to relay.
 *              The record says "erased" about the moment it was written; a
 *              receipt leaves the board -- over BLE or in the delivery below
 *              -- only when the network stores, read back at THAT moment,
 *              hold no network. A final record on top of a stored network,
 *              or of a store that cannot be read, hands out no receipt.
 *   delivery   after the final record is stored and read back, and only
 *              then, the board tries to hand the receipt to the hub itself.
 *              By then nothing of the network is left in its flash, and
 *              attaching the usual way would put it back (OpenThread stores
 *              the Dataset and its keys again on every attach). So the
 *              platform brings Thread up once more from a copy it kept in
 *              RAM, with OpenThread's persistent stores switched to RAM
 *              (ops.receipt_network_up; components/power_base_ot_store).
 *              That the switch held is not taken on trust: once attached,
 *              and again before every transmission, the stores are read
 *              back (ops.network_erased), and the receipt is sent only
 *              while they are empty -- the hub completes the removal on the
 *              first copy it takes, so nothing is sent on a store that is
 *              not. Then the platform takes Thread down and reads the
 *              stores back once more (ops.receipt_network_down), and the
 *              chip restarts. A delivery that fails, or a power cut
 *              anywhere in it, leaves a board that is released and erased
 *              with its receipt in the final record -- exactly what a board
 *              without this step leaves -- and an app collects the receipt
 *              over BLE instead.
 *              When a read-back in or after the session says "not empty",
 *              the stores are erased again (ops.network_erase, which must
 *              reach the flash although the switch is on) and read back;
 *              if that does not check out either, the receipt is taken
 *              back: the record goes back to "erase pending". Should the
 *              power go first, the next boot finds a final record on a
 *              store that is not empty: pb_mgmt_client_check_final_release()
 *              erases and checks again, and until that checks out
 *              pb_mgmt_receipt_load() hands out nothing.
 *              What no order can rule out: a store that is written AFTER
 *              the last read-back before a transmission and then cannot be
 *              erased. The hub has completed by then; the board says
 *              "erase pending" and erases at every boot until it checks
 *              out.
 *
 * The record states that go with it (power_base_enrollment.h):
 *
 *   ENROLLED --authenticated command--> UNENROLLING --> RELEASED (erase pending)
 *            --erase verified--> RELEASED (final, holds the receipt)
 *
 * and each arrow is one committed write of the whole record; nothing is
 * written before the command passed every check. cfg (wiring, sensors, the
 * BLE setup credential) is never touched.
 *
 * Finding the hub again ("locate", docs/power-base-management.md section 12).
 * The record holds where the hub's ingest listener was when this board was
 * enrolled. When that endpoint stops giving authenticated answers -- the
 * hub's Thread address changed, its listener moved to another port, or the
 * hub is simply down -- pb_mgmt_client_poll() itself looks for the hub, in
 * the same task and between two polls:
 *
 *   hint       an address in this board's own Thread network where the hub's
 *              host MAY be reachable now (ops.locate_hints; the platform
 *              takes them from the Thread Network Data and the router
 *              table). Not trusted for anything: every node of the network
 *              can be behind such an address.
 *   claim      the endpoint in a LOCATE_ACK that carries this enrollment's
 *              MAC, this board, hub and generation, and the nonce of the
 *              LOCATE it answers. That is the hub's word, and still not
 *              used:
 *   confirmed  a claim counts only when it came FROM the endpoint it names
 *              -- the LOCATE was sent to exactly that address and port. A
 *              claim that names another endpoint is asked once more, there.
 *              An endpoint is never taken over from a hint, from an answer
 *              to another probe, or from an endpoint the board could not
 *              reach itself.
 *   stored     the confirmed endpoint replaces the one in the record: the
 *              record as it is in flash is read again and must still be this
 *              very enrollment (ENROLLED, same network, hub, generation and
 *              key), it is written as one blob with nothing but the endpoint
 *              changed, and read back. Only then does this client use the
 *              new endpoint and tell the platform (ops.endpoint_changed) to
 *              move everything else that sends to the hub. A write that
 *              fails leaves every user on the old endpoint; a power cut
 *              leaves the old or the new record, both complete.
 *
 * What it never does: follow a hub with another id, another generation or
 * another key (those cannot make a LOCATE_ACK this board accepts); leave the
 * record's Thread network (a claim outside its Mesh-Local Prefix is not an
 * endpoint); run for a record without a credential (versions 1 and 2), or in
 * any state but ENROLLED; write anything for an endpoint that is the stored
 * one. Rounds are bounded (PB_MGMT_LOCATE_MAX_PROBES requests each) and
 * spaced out (PB_MGMT_LOCATE_ROUND_GAP_POLLS); hints are collected anew for
 * every round and kept for none.
 *
 * Progress from round to round. A round cannot ask everywhere when there are
 * more candidates than requests, so the search goes on where the round
 * before it stopped:
 *
 *   - the platform gives ALL its hints every round (up to
 *     PB_MGMT_LOCATE_MAX_HINTS, which no Thread network exceeds), most
 *     likely first. The candidates are, in that order, every distinct hint
 *     inside the record's network on the stored port and on the locate port;
 *   - every round first asks the stored address on the locate port, then
 *     walks on through the candidates from the one after the last it asked
 *     (found again by its address and port, so a list that grew or shrank in
 *     front of it does not send the walk back to the start), around the end
 *     to the first, at most once around per round. A list that fits into one
 *     round is therefore asked in full, in order, every round; a longer one
 *     is covered in consecutive slices: with C candidates every one of them
 *     is asked at least once in any ceil(C / (PB_MGMT_LOCATE_MAX_PROBES - 2))
 *     rounds in a row while the list stays the same and no answer in them
 *     needs a second request (see docs, section 12, for what that is in
 *     time);
 *   - an authenticated answer that names another endpoint when the round has
 *     no request left to ask there is not lost: the endpoint it named (and
 *     nothing else of the answer) is kept as the first place the next round
 *     asks, with a new nonce, under exactly the rules of any other
 *     candidate. It is used for that one round only. The same is done with
 *     an endpoint that was confirmed and could not be stored.
 *
 * What is kept between rounds is two addresses -- where the walk stands and,
 * at most, one endpoint still to be asked. Neither is trusted for anything:
 * both are only places to send the next LOCATE to. Any authenticated answer
 * to a poll, a move, and a (re)load of the record forget both.
 *
 * Secrets: the management key lives in pb_mgmt_client_t and in the record
 * until the final record is written; for the delivery it stays in
 * pb_mgmt_client_t (RAM) until that step ends. Nothing here logs a key, a
 * nonce, a MAC, a command id or an address.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "power_base_enrollment/power_base_enrollment.h"
#include "power_base_mgmt/pb_mgmt_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The board polls this often while attached (power_base/main). */
#define PB_MGMT_POLL_INTERVAL_MS 10000u
/* How many times one boot sends its "released" notice, and how long it waits
 * before each attempt after the first. */
#define PB_MGMT_PROOF_ATTEMPTS 6
#define PB_MGMT_PROOF_BACKOFF_MS                                                                                     \
    {                                                                                                                \
        2000u, 4000u, 8000u, 16000u, 30000u                                                                          \
    }
/* The same for the receipt, in its one delivery session. Shorter: the board
 * answers nothing else (not even BLE) until that session ends. */
#define PB_MGMT_RECEIPT_ATTEMPTS 5
#define PB_MGMT_RECEIPT_BACKOFF_MS                                                                                   \
    {                                                                                                                \
        2000u, 4000u, 8000u, 16000u                                                                                  \
    }
/* How often one boot announces its capabilities (HELLO) without an answer
 * before it stops trying. */
#define PB_MGMT_HELLO_ATTEMPTS 3

/* Locate. Counted in polls (PB_MGMT_POLL_INTERVAL_MS apart, made only while
 * attached), so nothing here needs a clock.
 * The first round starts once this many polls in a row brought no
 * authenticated answer: about a minute. */
#define PB_MGMT_LOCATE_AFTER_SILENT_POLLS 6u
/* Polls between one round and the next while the hub stays lost: 1, 2, 4 and
 * 8 minutes, then every 15. Any authenticated answer starts this over. */
#define PB_MGMT_LOCATE_ROUND_GAP_POLLS                                                                               \
    {                                                                                                                \
        6u, 12u, 24u, 48u, 90u                                                                                       \
    }
#define PB_MGMT_LOCATE_ROUND_GAPS 5
/* How many hints the platform may give for one round: all it has. 128 is
 * more than any Thread network yields -- at most 32 routers, plus the nodes
 * its Network Data can name as border router or server, at most 82 (254
 * bytes of Network Data, 3 bytes for the smallest entry after a 6 byte
 * prefix). The client takes no more than this many. */
#define PB_MGMT_LOCATE_MAX_HINTS 128
/* LOCATE requests one round may send in all (each waits one CoAP response
 * timeout at most): the stored address, a carried-over endpoint if there is
 * one, and the walk through the candidates with what is left -- at least
 * PB_MGMT_LOCATE_MAX_PROBES - 2 of them when no answer needs a second
 * request. */
#define PB_MGMT_LOCATE_MAX_PROBES 12

typedef enum {
    PB_MGMT_XFER_ANSWERED = 0, /* a 2.xx answer; its payload (possibly empty) is in `resp` */
    PB_MGMT_XFER_REJECTED,     /* a 4.xx answer: the hub parsed the request and refused it */
    PB_MGMT_XFER_NO_ANSWER,    /* timeout, send error, 5.xx, not attached */
} pb_mgmt_xfer_t;

typedef struct {
    pb_mgmt_hmac_fn hmac;
    /* Cryptographically strong random bytes. */
    bool (*random)(uint8_t *buf, size_t len, void *ctx);
    /* One Confirmable POST of `req` to PB_MGMT_URI_PATH at `endpoint`; blocks
     * until the answer or its timeout. Never retries by itself. */
    pb_mgmt_xfer_t (*exchange)(const pb_enroll_bridge_endpoint_t *endpoint, const uint8_t *req, size_t req_len,
                               uint8_t *resp, size_t resp_cap, size_t *resp_len, void *ctx);
    /* Stop taking new work, before anything is written: no new downstream
     * provisioning, no collector, exporter stopped, POGO downstream port
     * off. Only called while the services run (never on the boot path). */
    void (*quiesce)(void *ctx);
    void (*delay_ms)(uint32_t ms, void *ctx);
    /* Boot path only: enable Thread on the stored Dataset just to deliver
     * the "released" notice, and wait (bounded) until attached. False = no
     * network (also: nothing stored any more); no service is ever started
     * for this. */
    bool (*network_up_for_proof)(void *ctx);
    /* Disable Thread and erase OpenThread's persistent store (the Dataset
     * and everything else it keeps). ESP_OK = the calls were made without
     * an error. That alone is not taken as "erased": see network_erased.
     * Also called after receipt_network_up, when the store was found not
     * empty: it must erase the store itself then as well, not what the
     * session keeps in RAM in its place. */
    esp_err_t (*network_erase)(void *ctx);
    /* Reads OpenThread's persistent store back -- the store itself, also
     * during the delivery session. True only when it holds nothing any more;
     * false also when it could not be read. Asked after every network_erase
     * that returned ESP_OK, before anything says "erased", and in the
     * delivery session before every transmission of the receipt: always
     * right after an erase or while nothing may have been written since,
     * never as a question about a boot whose network stack has started on
     * its own (the stack may put something of its own into the store when
     * it starts; for that there is pb_mgmt_store_check_fn). Changes nothing
     * in the store. */
    bool (*network_erased)(void *ctx);
    /* The receipt's delivery session; both NULL on a platform without one
     * (the receipt is then collected over BLE only, and the board does not
     * announce PB_MGMT_BOARD_CAP_THREAD_RECEIPT).
     *
     * up: called once, after the final record was stored and read back.
     * Brings Thread up from what the platform kept in RAM, in a way that
     * cannot write to the persistent store, and waits (bounded) until
     * attached. False = no session (nothing was kept, the store could not be
     * switched, no attach); nothing is sent then. Never forms a network and
     * never starts a service. */
    bool (*receipt_network_up)(void *ctx);
    /* down: called after every `up`, whatever it returned. Takes Thread
     * down, forgets what was kept in RAM and reads the persistent store back
     * (as network_erased does): true only when it still holds nothing. */
    bool (*receipt_network_down)(void *ctx);
    /* Locate (see the top comment); a platform without it leaves
     * locate_hints or endpoint_changed NULL and nothing below ever runs.
     *
     * hints: EVERY IPv6 address inside this board's Thread network at which
     * the hub's host may answer now (up to `cap`, which is
     * PB_MGMT_LOCATE_MAX_HINTS) -- unauthenticated, most likely first, and
     * in the same order from call to call while the network stays as it is:
     * the client walks through this list over several rounds. Read fresh at
     * every call; must not block for long and must not send anything.
     * Returns how many were written. */
    size_t (*locate_hints)(uint8_t (*out)[PB_ENROLL_BRIDGE_ADDR_LEN], size_t cap, void *ctx);
    /* changed: called once, right after a new endpoint was stored and read
     * back, from the management task. Everything else on the platform that
     * sends to the hub moves to `endpoint` now and sends its full state
     * there again. */
    void (*endpoint_changed)(const pb_enroll_bridge_endpoint_t *endpoint, void *ctx);
    /* allowed (optional, NULL = yes): false while the platform is doing
     * something that ends this enrollment (a local reset, an unenroll) --
     * asked right before the record is rewritten. */
    bool (*locate_allowed)(void *ctx);
    void *ctx;
} pb_mgmt_ops_t;

typedef enum {
    /* Version 1 or 2 record, none, damaged, foreign, or a final RELEASED
     * record: nothing for management to do. */
    PB_MGMT_BOOT_NOTHING = 0,
    /* A version 3 record in state ENROLLED: poll once the services run. */
    PB_MGMT_BOOT_ARMED,
    /* UNENROLLING, or RELEASED with the proof pending: the caller must run
     * pb_mgmt_client_resume_release() and must not start any service. */
    PB_MGMT_BOOT_RELEASE_PENDING,
} pb_mgmt_boot_t;

typedef enum {
    PB_MGMT_POLL_NOT_ARMED = 0,
    /* No answer, a refusal, or an answer that is not this poll's
     * authenticated answer. Nothing changed. */
    PB_MGMT_POLL_NO_ANSWER,
    PB_MGMT_POLL_IDLE,
    /* The hub has a command: poll again now (the answer's nonce is kept). */
    PB_MGMT_POLL_COMMAND_PENDING,
    /* The command was executed to its end: the stored network was erased,
     * that was verified, and the final record (with the receipt) is stored.
     * The caller restarts the chip. */
    PB_MGMT_POLL_UNENROLLED,
    /* The enrollment is out of use for good (the RELEASED record is stored),
     * but the stored network is NOT confirmed erased: the erase failed, the
     * store was not empty afterwards, or the final record could not be
     * written. The record says "erase pending"; the caller restarts the chip
     * and the next boot does that step again. Not a completed unenroll. */
    PB_MGMT_POLL_RELEASED_NOT_ERASED,
    /* The command was accepted but the journal or the release record could
     * not be written. The stored record is still ENROLLED or UNENROLLING;
     * the services were already stopped, so the caller restarts the chip and
     * the next boot carries on from what is stored. */
    PB_MGMT_POLL_UNENROLL_INTERRUPTED,
    /* The poll itself brought no authenticated answer, and the locate round
     * it then ran found the hub at another endpoint, which is stored and in
     * use: poll again now. */
    PB_MGMT_POLL_ENDPOINT_MOVED,
} pb_mgmt_poll_result_t;

/* What the last locate round came to (pb_mgmt_client_t.last_locate). */
typedef enum {
    PB_MGMT_LOCATE_NONE = 0,       /* no round has run since the last authenticated answer */
    PB_MGMT_LOCATE_NOT_FOUND,      /* no endpoint was confirmed; the stored one stays */
    /* The hub was found and confirmed the endpoint that is already stored
     * (it answers there again): nothing was written. */
    PB_MGMT_LOCATE_SAME_ENDPOINT,
    PB_MGMT_LOCATE_MOVED,          /* a new endpoint was confirmed, stored, read back and is in use */
    /* A new endpoint was confirmed but the record could not be rewritten
     * (or did not read back): nothing uses it. The stored record is the old
     * one, complete. Tried again in the next round, which asks that endpoint
     * first. */
    PB_MGMT_LOCATE_STORE_FAILED,
    /* The record in flash is no longer this enrollment as this client holds
     * it (or the platform said a reset is under way): nothing was written. */
    PB_MGMT_LOCATE_RECORD_CHANGED,
    /* An authenticated answer named an endpoint, and the round had no
     * request left to ask there: nothing is confirmed, nothing was written.
     * The next round asks that endpoint first (pb_mgmt_client_t.locate_pending). */
    PB_MGMT_LOCATE_UNCONFIRMED,
} pb_mgmt_locate_result_t;

typedef enum {
    PB_MGMT_RELEASE_NOTHING = 0, /* the record needed nothing */
    /* The record is final: the erase was verified, no network, no key; the
     * record holds the receipt. `notice_acked` in pb_mgmt_client_t says
     * whether the hub acknowledged the "released" notice, `receipt_acked`
     * whether it acknowledged the receipt (both: notes, nothing depends on
     * them). */
    PB_MGMT_RELEASE_FINAL,
    /* A write failed, the network erase failed or the store was not empty
     * afterwards; the record still says what is left to do and the next boot
     * tries again. Never resumes enrollment, never claims the erase. */
    PB_MGMT_RELEASE_INCOMPLETE,
} pb_mgmt_release_result_t;

/* What a boot did about a final record before its network stack was started
 * (pb_mgmt_released_prestart). */
typedef enum {
    /* The stored record is not a final release: nothing was read or erased
     * (an enrolled board's network, and a pending release's, are needed). */
    PB_MGMT_PRESTART_NONE = 0,
    /* Final: the network stores were erased, and read back empty. Whatever
     * the stack now stores for itself when it starts is stored on an empty
     * store, where the platform can see it. */
    PB_MGMT_PRESTART_CLEARED,
    /* Final, and after the erase the stores did not read back empty (or
     * could not be read). Nothing was proven: this boot hands out no receipt
     * on the strength of the record (pb_mgmt_store_check_fn says no), and
     * the next boot tries again. */
    PB_MGMT_PRESTART_NOT_CLEARED,
} pb_mgmt_prestart_t;

typedef struct {
    /* Erases the network stores themselves. The network stack is not running
     * and must not be needed. Its result is not the proof: */
    esp_err_t (*erase)(void *ctx);
    /* reads them back -- true only when they hold nothing at all; false also
     * when one could not be read. */
    bool (*erased)(void *ctx);
    void *ctx;
} pb_mgmt_prestart_ops_t;

/* What a boot found under a final record (pb_mgmt_client_check_final_release). */
typedef enum {
    /* The stored record is not a final release: nothing was looked at. */
    PB_MGMT_FINAL_NONE = 0,
    /* Final, and the network stores hold no network. Nothing was changed. */
    PB_MGMT_FINAL_VERIFIED,
    /* Final, but the stores held a network, or could not be read (a delivery
     * session wrote to them and the power went before that was dealt with).
     * They were erased and read back empty in this boot. */
    PB_MGMT_FINAL_ERASED_AGAIN,
    /* Final, and the stores could not be erased, or could not be read back
     * empty afterwards. Nothing claims otherwise: pb_mgmt_receipt_load()
     * hands out no receipt while they hold a network, and the next boot
     * tries again. The record is not changed (it holds no key to go back to
     * "erase pending" with) and is never resumed. */
    PB_MGMT_FINAL_NOT_ERASED,
} pb_mgmt_final_check_t;

typedef struct {
    pb_mgmt_ops_t ops;
    uint8_t eui64[PB_ENROLL_EUI64_LEN];
    bool armed;
    /* The record as stored. Holds the management key while `armed`. */
    pb_enroll_record_t rec;
    bool have_hub_nonce;
    uint8_t hub_nonce[PB_MGMT_NONCE_LEN];
    /* After a release: the hub's acknowledgement of the "released" notice
     * was verified. Says nothing about the erase. */
    bool notice_acked;
    /* After a release: the hub's acknowledgement of the receipt was verified
     * in the delivery session. False says nothing about whether the hub has
     * it (the answer may have been lost, or the receipt came in over BLE). */
    bool receipt_acked;
    /* HELLO: this boot's capabilities were announced and answered (or
     * refused, by a hub that does not know the message). */
    bool hello_done;
    uint8_t hello_attempts;
    uint32_t hub_capabilities; /* from the last HELLO_ACK, else 0 */
    /* Counters for logs and tests. */
    uint32_t polls;
    uint32_t unauthentic_answers;
    uint32_t notice_attempts;
    uint32_t receipt_attempts;
    /* Locate. */
    uint32_t silent_polls;      /* polls in a row without an authenticated answer */
    uint32_t next_locate_at;    /* the round runs when silent_polls reaches this */
    uint8_t locate_rounds;      /* rounds since the last authenticated answer */
    pb_mgmt_locate_result_t last_locate;
    uint32_t locate_probes;     /* LOCATE requests sent, ever */
    uint32_t locate_moves;      /* endpoints taken over, ever */
    /* Where the walk through the candidates stands (see the top comment):
     * the candidate asked last and the place it had in the list then. Only
     * ever a place to go on from. */
    bool locate_cursor_valid;
    pb_enroll_bridge_endpoint_t locate_cursor;
    uint16_t locate_cursor_pos;
    /* An endpoint an authenticated answer named when its round had no
     * request left, or one that was confirmed and could not be stored:
     * asked first in the next round, then forgotten. */
    bool locate_pending_valid;
    pb_enroll_bridge_endpoint_t locate_pending;
} pb_mgmt_client_t;

void pb_mgmt_client_init(pb_mgmt_client_t *c, const pb_mgmt_ops_t *ops, const uint8_t eui64[PB_ENROLL_EUI64_LEN]);

/* Reads the stored record (never writes). */
pb_mgmt_boot_t pb_mgmt_client_load(pb_mgmt_client_t *c);

/* One poll. See pb_mgmt_poll_result_t. */
pb_mgmt_poll_result_t pb_mgmt_client_poll(pb_mgmt_client_t *c);

/* Boot path, after pb_mgmt_client_load() returned RELEASE_PENDING: finishes
 * what an earlier boot started. No service runs; `quiesce` is not called. */
pb_mgmt_release_result_t pb_mgmt_client_resume_release(pb_mgmt_client_t *c);

/*
 * Boot path, every boot, BEFORE the platform starts its network stack.
 *
 * A final record says "erased" about the boot that wrote it; a later boot
 * cannot read that back, because the network stack keeps a key of its own in
 * the same place where a network's key lies, creates it at a start only when
 * that place is empty, and so leaves a network's key that an erase failed to
 * remove exactly where it is -- looking like its own. So on a final record
 * the stores are not inspected but erased and read back, here, where nothing
 * of the stack exists yet: a key an earlier start made is removed, a
 * network's key is removed, and what this start then stores is its own.
 *
 * Reads the record; never writes it. See pb_mgmt_prestart_t.
 */
pb_mgmt_prestart_t pb_mgmt_released_prestart(const uint8_t eui64[PB_ENROLL_EUI64_LEN], const pb_mgmt_prestart_ops_t *ops);

/* "Is a network stored?", asked of the platform's network stores when
 * "nothing is left" cannot be asked any more (the network stack has started
 * and put a key of its own there). True only when no network is stored, the
 * stores could be read, and whatever lies where the network's key would lie
 * is known to be the stack's own -- known from this boot's history
 * (pb_mgmt_released_prestart, then the stack's start), never from where it
 * lies: power_base_ot_store/pb_stored_network.h. Changes nothing. */
typedef bool (*pb_mgmt_store_check_fn)(void *ctx);

/* Boot path, every boot, when pb_mgmt_client_load() did not return
 * RELEASE_PENDING: a final record is checked against the network stores
 * themselves (`no_network_stored`), and when they hold a network they are
 * erased and read back in this boot (ops.network_erase, ops.network_erased).
 * Reads the record; never writes it. No service runs. See
 * pb_mgmt_final_check_t. */
pb_mgmt_final_check_t pb_mgmt_client_check_final_release(pb_mgmt_client_t *c, pb_mgmt_store_check_fn no_network_stored,
                                                         void *ctx);

/* Wipes the key and everything else held in RAM. */
void pb_mgmt_client_wipe(pb_mgmt_client_t *c);

/* For a platform's locate_hints: the routing locator of the Thread node with
 * short address `rloc16` in the network with this Mesh-Local Prefix
 * (prefix | 0000:00ff:fe00:rloc16). False for a short address that names no
 * node (0xfffe, 0xffff). A locator is somewhere to ASK; it is never stored
 * (pb_enroll_bridge_endpoint_check() refuses one). */
bool pb_mgmt_locator_address(const uint8_t mesh_local_prefix[8], uint16_t rloc16,
                             uint8_t out[PB_ENROLL_BRIDGE_ADDR_LEN]);

/* The release receipt: the complete UNENROLL_DONE/ERASED message of this
 * board's last release, exactly PB_MGMT_RECEIPT_LEN bytes. True only when
 * BOTH hold at the time of the call:
 *   - the stored record is a final release -- written only after the erase
 *     of the stored network was verified; and
 *   - `no_network_stored` says the network stores hold no network NOW. The
 *     record alone is not enough: a delivery session that wrote to the
 *     stores, cut short by a power loss, leaves a final record on a stored
 *     network, and an erase that removed everything but the Network Key
 *     leaves one on that key. A NULL `no_network_stored` is "not verified":
 *     no receipt.
 * No secret: identifiers and a MAC. Reads the record and, through
 * `no_network_stored`, the stores; never writes. */
#define PB_MGMT_RECEIPT_LEN PB_MGMT_UNENROLL_DONE_LEN
bool pb_mgmt_receipt_load(const uint8_t eui64[PB_ENROLL_EUI64_LEN], pb_mgmt_store_check_fn no_network_stored, void *ctx,
                          uint8_t out[PB_MGMT_RECEIPT_LEN]);

#ifdef __cplusplus
}
#endif
