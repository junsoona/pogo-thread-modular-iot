#include "power_base_mgmt/pb_mgmt_client.h"

#include <string.h>
#include "esp_log.h"

static const char *TAG = "pb_mgmt";

_Static_assert(PB_MGMT_EUI64_LEN == PB_ENROLL_EUI64_LEN && PB_MGMT_HUB_ID_LEN == PB_ENROLL_HUB_ID_LEN &&
                   PB_MGMT_GENERATION_LEN == PB_ENROLL_GENERATION_LEN && PB_MGMT_KEY_LEN == PB_ENROLL_MGMT_KEY_LEN &&
                   PB_MGMT_COMMAND_ID_LEN == PB_ENROLL_COMMAND_ID_LEN && PB_MGMT_MAC_LEN == PB_ENROLL_MAC_LEN,
               "the enrollment record and the management protocol share these sizes");

static const uint32_t k_proof_backoff_ms[PB_MGMT_PROOF_ATTEMPTS - 1] = PB_MGMT_PROOF_BACKOFF_MS;
static const uint32_t k_receipt_backoff_ms[PB_MGMT_RECEIPT_ATTEMPTS - 1] = PB_MGMT_RECEIPT_BACKOFF_MS;
static const uint32_t k_locate_round_gap_polls[PB_MGMT_LOCATE_ROUND_GAPS] = PB_MGMT_LOCATE_ROUND_GAP_POLLS;
/* The hints of the round that is running: filled and used inside
 * locate_round() only, nothing of it outlives the round. Not on the stack
 * (2 KB) and not per client: a platform has one client and one management
 * task that polls. */
static uint8_t s_locate_hints[PB_MGMT_LOCATE_MAX_HINTS][PB_ENROLL_BRIDGE_ADDR_LEN];

static void fill_identity(const pb_mgmt_client_t *c, pb_mgmt_identity_t *id)
{
    memcpy(id->eui64, c->eui64, PB_MGMT_EUI64_LEN);
    memcpy(id->hub_id, c->rec.mgmt.hub_id, PB_MGMT_HUB_ID_LEN);
    memcpy(id->generation, c->rec.mgmt.generation, PB_MGMT_GENERATION_LEN);
}

/* This board, this hub, this enrollment generation -- as stored. */
static bool identity_is_ours(const pb_mgmt_client_t *c, const pb_mgmt_identity_t *id)
{
    bool eui = pb_mgmt_ct_equal(id->eui64, c->eui64, PB_MGMT_EUI64_LEN);
    bool hub = pb_mgmt_ct_equal(id->hub_id, c->rec.mgmt.hub_id, PB_MGMT_HUB_ID_LEN);
    bool gen = pb_mgmt_ct_equal(id->generation, c->rec.mgmt.generation, PB_MGMT_GENERATION_LEN);
    return eui && hub && gen;
}

/* Forgets where the search stood: the next round starts at the top of the
 * list and has nothing carried over. */
static void forget_locate_progress(pb_mgmt_client_t *c)
{
    c->locate_cursor_valid = false;
    c->locate_cursor_pos = 0;
    memset(&c->locate_cursor, 0, sizeof(c->locate_cursor));
    c->locate_pending_valid = false;
    memset(&c->locate_pending, 0, sizeof(c->locate_pending));
}

void pb_mgmt_client_wipe(pb_mgmt_client_t *c)
{
    pb_enroll_wipe(&c->rec, sizeof(c->rec));
    pb_enroll_wipe(c->hub_nonce, sizeof(c->hub_nonce));
    c->have_hub_nonce = false;
    c->armed = false;
    /* What a search kept belongs to the record that was just dropped. */
    forget_locate_progress(c);
}

void pb_mgmt_client_init(pb_mgmt_client_t *c, const pb_mgmt_ops_t *ops, const uint8_t eui64[PB_ENROLL_EUI64_LEN])
{
    memset(c, 0, sizeof(*c));
    c->ops = *ops;
    memcpy(c->eui64, eui64, PB_ENROLL_EUI64_LEN);
    c->next_locate_at = PB_MGMT_LOCATE_AFTER_SILENT_POLLS;
}

pb_mgmt_boot_t pb_mgmt_client_load(pb_mgmt_client_t *c)
{
    pb_mgmt_client_wipe(c);
    c->notice_acked = false;
    c->receipt_acked = false;
    pb_enroll_record_t rec;
    if (pb_enroll_record_load(c->eui64, &rec) != PB_ENROLL_MARKER_VALID || !rec.has_mgmt) {
        pb_enroll_wipe(&rec, sizeof(rec));
        return PB_MGMT_BOOT_NOTHING;
    }
    if (pb_enroll_record_is_final_release(&rec)) {
        pb_enroll_wipe(&rec, sizeof(rec));
        return PB_MGMT_BOOT_NOTHING;
    }
    c->rec = rec;
    pb_enroll_wipe(&rec, sizeof(rec));
    if (c->rec.mgmt_state == PB_ENROLL_MGMT_ENROLLED) {
        c->armed = true;
        return PB_MGMT_BOOT_ARMED;
    }
    return PB_MGMT_BOOT_RELEASE_PENDING;
}

/* ---- the release, step by step (docs/power-base-management.md, section 4) ---- */

/* Step C: the command is journaled. From here on no boot resumes the
 * enrollment. */
static bool store_journal(pb_mgmt_client_t *c, const uint8_t command_id[PB_MGMT_COMMAND_ID_LEN])
{
    pb_enroll_record_t next = c->rec;
    next.mgmt_state = PB_ENROLL_MGMT_UNENROLLING;
    next.release_flags = 0;
    memcpy(next.command_id, command_id, PB_ENROLL_COMMAND_ID_LEN);
    bool ok = pb_enroll_record_store(c->eui64, &next) == ESP_OK;
    if (ok) {
        c->rec = next;
    }
    pb_enroll_wipe(&next, sizeof(next));
    return ok;
}

static void done_message(const pb_mgmt_client_t *c, uint8_t result, pb_mgmt_msg_t *m)
{
    memset(m, 0, sizeof(*m));
    m->type = PB_MGMT_TYPE_UNENROLL_DONE;
    fill_identity(c, &m->id);
    memcpy(m->command_id, c->rec.command_id, PB_MGMT_COMMAND_ID_LEN);
    m->result = result;
}

static void ack_message(const pb_mgmt_client_t *c, uint8_t acked_type, uint8_t acked_result, pb_mgmt_msg_t *m)
{
    memset(m, 0, sizeof(*m));
    m->type = PB_MGMT_TYPE_RESULT_ACK;
    fill_identity(c, &m->id);
    memcpy(m->command_id, c->rec.command_id, PB_MGMT_COMMAND_ID_LEN);
    m->acked_type = acked_type;
    m->acked_result = acked_result;
}

/* Step D: "released". From this write on the enrollment is out of use for
 * good. The stored network is untouched and so is the key: the record says
 * "erase pending". Counts only once it was read back. */
static bool store_released(pb_mgmt_client_t *c)
{
    pb_enroll_record_t next = c->rec;
    next.mgmt_state = PB_ENROLL_MGMT_RELEASED;
    next.release_flags = PB_ENROLL_RELEASE_F_ERASE_PENDING;
    bool ok = false;
    if (pb_enroll_record_store(c->eui64, &next) == ESP_OK) {
        pb_enroll_record_t back;
        ok = pb_enroll_record_load(c->eui64, &back) == PB_ENROLL_MARKER_VALID && back.has_mgmt &&
             back.mgmt_state == PB_ENROLL_MGMT_RELEASED && back.release_flags == PB_ENROLL_RELEASE_F_ERASE_PENDING &&
             pb_mgmt_ct_equal(back.command_id, next.command_id, PB_ENROLL_COMMAND_ID_LEN) &&
             pb_mgmt_ct_equal(back.mgmt.key, next.mgmt.key, PB_ENROLL_MGMT_KEY_LEN);
        pb_enroll_wipe(&back, sizeof(back));
    }
    if (ok) {
        c->rec = next;
        /* A released board does not poll. */
        c->armed = false;
    }
    pb_enroll_wipe(&next, sizeof(next));
    return ok;
}

/* Step E: the "released" notice, a bounded number of times, while Thread is
 * still up. NOT a completion and never worded as one: it says the record of
 * step D is stored, nothing about the stored network. True when the hub's
 * acknowledgement of exactly this notice was verified. */
static bool deliver_released_notice(pb_mgmt_client_t *c)
{
    uint8_t notice[PB_MGMT_MAX_MESSAGE_LEN];
    uint8_t expected[PB_MGMT_MAX_MESSAGE_LEN];
    uint8_t resp[PB_MGMT_MAX_MESSAGE_LEN];
    pb_mgmt_msg_t m;

    done_message(c, PB_MGMT_RESULT_RELEASED, &m);
    size_t notice_len = pb_mgmt_pack(&m, c->rec.mgmt.key, c->ops.hmac, c->ops.ctx, notice, sizeof(notice));
    ack_message(c, PB_MGMT_TYPE_UNENROLL_DONE, PB_MGMT_RESULT_RELEASED, &m);
    size_t expected_len = pb_mgmt_pack(&m, c->rec.mgmt.key, c->ops.hmac, c->ops.ctx, expected, sizeof(expected));
    bool acked = false;
    if (notice_len == 0 || expected_len == 0) {
        goto out;
    }

    for (int attempt = 0; attempt < PB_MGMT_PROOF_ATTEMPTS && !acked; attempt++) {
        if (attempt > 0) {
            c->ops.delay_ms(k_proof_backoff_ms[attempt - 1], c->ops.ctx);
        }
        c->notice_attempts++;
        size_t resp_len = 0;
        pb_mgmt_xfer_t x =
            c->ops.exchange(&c->rec.bridge_endpoint, notice, notice_len, resp, sizeof(resp), &resp_len, c->ops.ctx);
        if (x == PB_MGMT_XFER_REJECTED) {
            /* The hub read the notice and refused it (it no longer knows this
             * generation, or it is not the hub of this enrollment). The same
             * bytes will not fare better. */
            ESP_LOGW(TAG, "release: the hub refused the released notice -- not sent again");
            break;
        }
        if (x == PB_MGMT_XFER_ANSWERED && resp_len == expected_len && pb_mgmt_ct_equal(resp, expected, expected_len)) {
            acked = true;
        }
    }

out:
    pb_enroll_wipe(notice, sizeof(notice));
    pb_enroll_wipe(expected, sizeof(expected));
    pb_enroll_wipe(resp, sizeof(resp));
    return acked;
}

/* Step G: the receipt, to the hub, over one last Thread session that the
 * platform keeps out of its persistent store (pb_mgmt_client.h, "delivery").
 * Called only after step F stored the final record and read it back; `c->rec`
 * is still the "erase pending" record with the key and the hub's address, in
 * RAM.
 *
 * The receipt says "nothing of the network is stored", and the hub completes
 * the removal on the first copy it takes. Attaching is exactly what would
 * store the network again, so the store is read back once the session is up
 * and before EVERY transmission, and the receipt goes out only on a store
 * that is empty then. It is read back again when the session is down.
 *
 * Best effort: FINAL comes back whether or not the hub answered. The only
 * other outcome is the one that must not be passed over -- a read-back said
 * "not empty" and the store cannot be erased again -- and then the record is
 * put back to "erase pending": no receipt is handed out for a board whose
 * erase does not check out. */
static pb_mgmt_release_result_t deliver_receipt(pb_mgmt_client_t *c, const uint8_t receipt[PB_MGMT_RECEIPT_LEN])
{
    c->receipt_acked = false;
    if (c->ops.receipt_network_up == NULL || c->ops.receipt_network_down == NULL || !c->rec.has_bridge_endpoint) {
        return PB_MGMT_RELEASE_FINAL;
    }

    uint8_t expected[PB_MGMT_MAX_MESSAGE_LEN];
    uint8_t resp[PB_MGMT_MAX_MESSAGE_LEN];
    pb_mgmt_msg_t m;
    ack_message(c, PB_MGMT_TYPE_UNENROLL_DONE, PB_MGMT_RESULT_ERASED, &m);
    size_t expected_len = pb_mgmt_pack(&m, c->rec.mgmt.key, c->ops.hmac, c->ops.ctx, expected, sizeof(expected));

    bool store_clean = true;
    if (c->ops.receipt_network_up(c->ops.ctx)) {
        for (int attempt = 0; attempt < PB_MGMT_RECEIPT_ATTEMPTS && !c->receipt_acked; attempt++) {
            if (attempt > 0) {
                c->ops.delay_ms(k_receipt_backoff_ms[attempt - 1], c->ops.ctx);
            }
            if (!c->ops.network_erased(c->ops.ctx)) {
                store_clean = false;
                ESP_LOGE(TAG, "release: the network store is NOT empty in the receipt session -- the receipt is "
                              "not sent%s",
                         attempt > 0 ? " again" : "");
                break;
            }
            c->receipt_attempts++;
            size_t resp_len = 0;
            pb_mgmt_xfer_t x = c->ops.exchange(&c->rec.bridge_endpoint, receipt, PB_MGMT_RECEIPT_LEN, resp, sizeof(resp),
                                               &resp_len, c->ops.ctx);
            if (x == PB_MGMT_XFER_REJECTED) {
                /* The hub read it and refused it: it no longer holds this
                 * enrollment's key (also: it completed the removal on an
                 * earlier copy whose answer was lost, and predates
                 * re-acknowledging). The same bytes will not fare better. */
                ESP_LOGW(TAG, "release: the hub refused the receipt -- not sent again");
                break;
            }
            if (x == PB_MGMT_XFER_ANSWERED && expected_len != 0 && resp_len == expected_len &&
                pb_mgmt_ct_equal(resp, expected, expected_len)) {
                c->receipt_acked = true;
            }
        }
    } else {
        ESP_LOGW(TAG, "release: no network session for the receipt -- it waits in the final record for a client");
    }
    /* Always, and always read: a store that was written after the last
     * read-back above is found here. */
    bool empty_after = c->ops.receipt_network_down(c->ops.ctx);
    pb_enroll_wipe(expected, sizeof(expected));
    pb_enroll_wipe(resp, sizeof(resp));
    if (store_clean && empty_after) {
        return PB_MGMT_RELEASE_FINAL;
    }

    /* Thread is down. The erase has to reach the store itself here: what the
     * session kept in RAM in its place is not what was found. */
    ESP_LOGE(TAG, "release: a read-back of the network store in the receipt session did not say \"empty\" -- "
                  "erasing again");
    if (c->ops.network_erase(c->ops.ctx) == ESP_OK && c->ops.network_erased(c->ops.ctx)) {
        ESP_LOGW(TAG, "release: the network store is erased and verified again");
        return PB_MGMT_RELEASE_FINAL;
    }
    ESP_LOGE(TAG, "release: the store could not be erased again -- back to \"erase pending\": the next boot erases "
                  "and checks before any receipt is handed out");
    if (pb_enroll_record_store(c->eui64, &c->rec) != ESP_OK) {
        /* The final record stays, on a store that is not empty: it hands out
         * no receipt (pb_mgmt_receipt_load() reads the store), and the next
         * boot erases (pb_mgmt_client_check_final_release()). */
        ESP_LOGE(TAG, "release: the record could not be put back either -- no receipt is handed out while the "
                      "store is not empty");
    }
    return PB_MGMT_RELEASE_INCOMPLETE;
}

/* Step F: the stored network is erased, the store is read back, and only
 * then is there a receipt and a final record. In this order:
 *   1. network_erase            a failure ends the step: still "erase pending"
 *   2. network_erased           the store is not empty: the same
 *   3. the receipt is computed  (UNENROLL_DONE/ERASED; the first and only
 *                               place that message is made)
 *   4. the final record         receipt in, key and network out, one write,
 *                               read back; a failure: still "erase pending"
 *                               (the erase is simply done and checked again
 *                               by the next boot)
 * On FINAL the receipt is in `receipt` and `c` still holds the record as it
 * was before (key and hub address, in RAM) for step G; the caller wipes it. */
static pb_mgmt_release_result_t erase_and_store_final(pb_mgmt_client_t *c, bool notice_acked,
                                                      uint8_t receipt[PB_MGMT_RECEIPT_LEN])
{
    c->notice_acked = notice_acked;
    if (c->ops.network_erase(c->ops.ctx) != ESP_OK) {
        ESP_LOGE(TAG, "release: the stored network could not be erased -- the next boot tries again (this board "
                      "stays released; nothing reports the erase as done)");
        return PB_MGMT_RELEASE_INCOMPLETE;
    }
    if (!c->ops.network_erased(c->ops.ctx)) {
        ESP_LOGE(TAG, "release: the erase returned no error but the network store is not empty -- the next boot "
                      "tries again (nothing reports the erase as done)");
        return PB_MGMT_RELEASE_INCOMPLETE;
    }

    uint8_t wire[PB_MGMT_MAX_MESSAGE_LEN];
    pb_mgmt_msg_t m;
    pb_enroll_record_t final_rec;
    pb_mgmt_release_result_t result = PB_MGMT_RELEASE_INCOMPLETE;
    memset(&final_rec, 0, sizeof(final_rec));

    done_message(c, PB_MGMT_RESULT_ERASED, &m);
    size_t n = pb_mgmt_pack(&m, c->rec.mgmt.key, c->ops.hmac, c->ops.ctx, wire, sizeof(wire));
    if (n != PB_MGMT_UNENROLL_DONE_LEN) {
        ESP_LOGE(TAG, "release: the receipt could not be computed -- the next boot tries again");
        goto out;
    }
    final_rec.has_mgmt = true;
    final_rec.mgmt_state = PB_ENROLL_MGMT_RELEASED;
    final_rec.release_flags =
        (uint8_t)(PB_ENROLL_RELEASE_F_ERASED | (notice_acked ? PB_ENROLL_RELEASE_F_NOTICE_ACKED : 0));
    memcpy(final_rec.mgmt.hub_id, c->rec.mgmt.hub_id, PB_ENROLL_HUB_ID_LEN);
    memcpy(final_rec.mgmt.generation, c->rec.mgmt.generation, PB_ENROLL_GENERATION_LEN);
    memcpy(final_rec.command_id, c->rec.command_id, PB_ENROLL_COMMAND_ID_LEN);
    memcpy(final_rec.done_mac, pb_mgmt_mac_of(wire, n), PB_ENROLL_MAC_LEN);
    if (pb_enroll_record_store(c->eui64, &final_rec) != ESP_OK) {
        ESP_LOGE(TAG, "release: the final record could not be written -- the next boot tries again");
        goto out;
    }
    pb_enroll_record_t back;
    bool same = pb_enroll_record_load(c->eui64, &back) == PB_ENROLL_MARKER_VALID &&
                pb_enroll_record_is_final_release(&back) && back.release_flags == final_rec.release_flags &&
                pb_mgmt_is_zero(back.mgmt.key, sizeof(back.mgmt.key)) &&
                pb_mgmt_ct_equal(back.done_mac, final_rec.done_mac, PB_ENROLL_MAC_LEN) &&
                pb_mgmt_ct_equal(back.command_id, final_rec.command_id, PB_ENROLL_COMMAND_ID_LEN);
    pb_enroll_wipe(&back, sizeof(back));
    if (!same) {
        ESP_LOGE(TAG, "release: the final record did not read back -- the next boot tries again");
        goto out;
    }
    ESP_LOGW(TAG, "release: stored network erased and verified, final record stored (the hub %s the released "
                  "notice)",
             notice_acked ? "acknowledged" : "did not acknowledge");
    memcpy(receipt, wire, PB_MGMT_RECEIPT_LEN);
    result = PB_MGMT_RELEASE_FINAL;

out:
    pb_enroll_wipe(wire, sizeof(wire));
    pb_enroll_wipe(&final_rec, sizeof(final_rec));
    return result;
}

/* Steps F and G. One after the other, not one inside the other: the buffers
 * of F are gone from the stack before G's exchanges start. */
static pb_mgmt_release_result_t erase_and_finalize(pb_mgmt_client_t *c, bool notice_acked)
{
    uint8_t receipt[PB_MGMT_RECEIPT_LEN];
    pb_mgmt_release_result_t result = erase_and_store_final(c, notice_acked, receipt);
    if (result == PB_MGMT_RELEASE_FINAL) {
        result = deliver_receipt(c, receipt);
        pb_mgmt_client_wipe(c);
        if (result == PB_MGMT_RELEASE_FINAL) {
            ESP_LOGW(TAG, "release complete: %s",
                     c->receipt_acked ? "the hub acknowledged the receipt"
                                      : "the hub did not acknowledge the receipt; it stays in the final record for a "
                                        "client to collect");
        }
    }
    pb_enroll_wipe(receipt, sizeof(receipt));
    return result;
}

pb_mgmt_prestart_t pb_mgmt_released_prestart(const uint8_t eui64[PB_ENROLL_EUI64_LEN], const pb_mgmt_prestart_ops_t *ops)
{
    pb_enroll_record_t rec;
    bool final = eui64 != NULL && pb_enroll_record_load(eui64, &rec) == PB_ENROLL_MARKER_VALID &&
                 pb_enroll_record_is_final_release(&rec);
    pb_enroll_wipe(&rec, sizeof(rec));
    if (!final) {
        return PB_MGMT_PRESTART_NONE;
    }
    if (ops == NULL || ops->erase == NULL || ops->erased == NULL) {
        return PB_MGMT_PRESTART_NOT_CLEARED;
    }
    /* The read-back is the proof, whatever the erase returned: an erase that
     * reported no error and left something is not passed, and one that
     * reported an error on stores that read back empty has nothing left to
     * erase. It is the same read-back the platform's store check goes by. */
    esp_err_t err = ops->erase(ops->ctx);
    if (ops->erased(ops->ctx)) {
        return PB_MGMT_PRESTART_CLEARED;
    }
    ESP_LOGE(TAG, "boot: this board's record is a final release, but its network store did not read back empty "
                  "before the network stack starts (the erase %s) -- no receipt is handed out in this boot",
             err == ESP_OK ? "reported no error" : "failed");
    return PB_MGMT_PRESTART_NOT_CLEARED;
}

pb_mgmt_final_check_t pb_mgmt_client_check_final_release(pb_mgmt_client_t *c, pb_mgmt_store_check_fn no_network_stored,
                                                         void *ctx)
{
    pb_enroll_record_t rec;
    bool final = pb_enroll_record_load(c->eui64, &rec) == PB_ENROLL_MARKER_VALID &&
                 pb_enroll_record_is_final_release(&rec);
    pb_enroll_wipe(&rec, sizeof(rec));
    if (!final) {
        return PB_MGMT_FINAL_NONE;
    }
    if (no_network_stored != NULL && no_network_stored(ctx)) {
        return PB_MGMT_FINAL_VERIFIED;
    }
    ESP_LOGE(TAG, "boot: this board's record is a final release, but its network store holds a network or cannot "
                  "be read -- erasing and checking (no receipt is handed out until that checks out)");
    if (c->ops.network_erase(c->ops.ctx) == ESP_OK && c->ops.network_erased(c->ops.ctx)) {
        ESP_LOGW(TAG, "boot: the network store is erased and verified again");
        return PB_MGMT_FINAL_ERASED_AGAIN;
    }
    ESP_LOGE(TAG, "boot: the network store could not be erased -- no receipt is handed out; the next boot tries "
                  "again (this board stays released and is never resumed)");
    return PB_MGMT_FINAL_NOT_ERASED;
}

bool pb_mgmt_receipt_load(const uint8_t eui64[PB_ENROLL_EUI64_LEN], pb_mgmt_store_check_fn no_network_stored, void *ctx,
                          uint8_t out[PB_MGMT_RECEIPT_LEN])
{
    pb_enroll_record_t rec;
    bool ok = false;
    if (eui64 == NULL || out == NULL) {
        return false;
    }
    if (pb_enroll_record_load(eui64, &rec) != PB_ENROLL_MARKER_VALID || !pb_enroll_record_is_final_release(&rec) ||
        (rec.release_flags & PB_ENROLL_RELEASE_F_ERASED) == 0) {
        /* No final release: there is no receipt, and the stores are not
         * asked. */
    } else if (no_network_stored == NULL || !no_network_stored(ctx)) {
        /* The record says "erased"; the stores do not, or cannot be read. */
        ESP_LOGE(TAG, "receipt: the record is a final release, but the network store holds a network or cannot be "
                      "read -- no receipt is handed out");
    } else {
        pb_mgmt_msg_t m;
        memset(&m, 0, sizeof(m));
        m.type = PB_MGMT_TYPE_UNENROLL_DONE;
        memcpy(m.id.eui64, eui64, PB_MGMT_EUI64_LEN);
        memcpy(m.id.hub_id, rec.mgmt.hub_id, PB_MGMT_HUB_ID_LEN);
        memcpy(m.id.generation, rec.mgmt.generation, PB_MGMT_GENERATION_LEN);
        memcpy(m.command_id, rec.command_id, PB_MGMT_COMMAND_ID_LEN);
        m.result = PB_MGMT_RESULT_ERASED;
        ok = pb_mgmt_pack_with_mac(&m, rec.done_mac, out, PB_MGMT_RECEIPT_LEN) == PB_MGMT_RECEIPT_LEN;
    }
    pb_enroll_wipe(&rec, sizeof(rec));
    if (!ok) {
        memset(out, 0, PB_MGMT_RECEIPT_LEN);
    }
    return ok;
}

/* What this firmware can do beyond the six basic messages. */
static bool locate_supported(const pb_mgmt_client_t *c)
{
    return c->ops.locate_hints != NULL && c->ops.endpoint_changed != NULL;
}

static uint32_t board_capabilities(const pb_mgmt_client_t *c)
{
    uint32_t caps = 0;
    if (c->ops.receipt_network_up != NULL && c->ops.receipt_network_down != NULL) {
        caps |= PB_MGMT_BOARD_CAP_THREAD_RECEIPT;
    }
    if (locate_supported(c)) {
        caps |= PB_MGMT_BOARD_CAP_ENDPOINT_LOCATE;
    }
    return caps;
}

/* HELLO, once per boot, after the hub answered a poll (so the enrollment is
 * known to be active there). The hub uses it to tell an app whether this
 * board will send its receipt itself; nothing on the board depends on the
 * answer. A hub that does not know the message refuses it like any unknown
 * type, and that ends it for this boot; no answer at all is tried again with
 * a later poll, a bounded number of times. */
static void announce_capabilities(pb_mgmt_client_t *c)
{
    const uint32_t caps = board_capabilities(c);
    if (c->hello_done || caps == 0 || c->hello_attempts >= PB_MGMT_HELLO_ATTEMPTS) {
        return;
    }
    uint8_t wire[PB_MGMT_MAX_MESSAGE_LEN];
    uint8_t resp[PB_MGMT_MAX_MESSAGE_LEN];
    pb_mgmt_msg_t m;
    memset(&m, 0, sizeof(m));
    m.type = PB_MGMT_TYPE_HELLO;
    fill_identity(c, &m.id);
    m.capabilities = caps;
    size_t n = pb_mgmt_pack(&m, c->rec.mgmt.key, c->ops.hmac, c->ops.ctx, wire, sizeof(wire));
    if (n != 0) {
        c->hello_attempts++;
        size_t resp_len = 0;
        pb_mgmt_xfer_t x = c->ops.exchange(&c->rec.bridge_endpoint, wire, n, resp, sizeof(resp), &resp_len, c->ops.ctx);
        pb_mgmt_msg_t a;
        if (x == PB_MGMT_XFER_REJECTED) {
            c->hello_done = true;
        } else if (x == PB_MGMT_XFER_ANSWERED && pb_mgmt_parse(resp, resp_len, &a) &&
                   a.type == PB_MGMT_TYPE_HELLO_ACK &&
                   pb_mgmt_verify(resp, resp_len, c->rec.mgmt.key, c->ops.hmac, c->ops.ctx) &&
                   identity_is_ours(c, &a.id)) {
            c->hello_done = true;
            c->hub_capabilities = a.capabilities;
        }
    }
    pb_enroll_wipe(wire, sizeof(wire));
    pb_enroll_wipe(resp, sizeof(resp));
}

/* An authenticated command for this board, this hub and this generation,
 * answering this poll. Runs in the management task. */
static pb_mgmt_poll_result_t execute_unenroll(pb_mgmt_client_t *c, const uint8_t command_id[PB_MGMT_COMMAND_ID_LEN])
{
    ESP_LOGW(TAG, "authenticated unenroll command accepted -- stopping services and releasing this enrollment");
    /* B: no new work, before anything is written. */
    c->ops.quiesce(c->ops.ctx);

    /* C */
    if (!store_journal(c, command_id)) {
        ESP_LOGE(TAG, "release: the command could not be journaled -- still enrolled");
        return PB_MGMT_POLL_UNENROLL_INTERRUPTED;
    }

    /* "I have your command and will carry it out" -- never the proof. One
     * attempt; its answer is not needed. */
    uint8_t wire[PB_MGMT_MAX_MESSAGE_LEN];
    uint8_t resp[PB_MGMT_MAX_MESSAGE_LEN];
    pb_mgmt_msg_t m;
    memset(&m, 0, sizeof(m));
    m.type = PB_MGMT_TYPE_UNENROLL_ACCEPTED;
    fill_identity(c, &m.id);
    memcpy(m.command_id, c->rec.command_id, PB_MGMT_COMMAND_ID_LEN);
    size_t n = pb_mgmt_pack(&m, c->rec.mgmt.key, c->ops.hmac, c->ops.ctx, wire, sizeof(wire));
    if (n != 0) {
        size_t resp_len = 0;
        (void)c->ops.exchange(&c->rec.bridge_endpoint, wire, n, resp, sizeof(resp), &resp_len, c->ops.ctx);
    }
    pb_enroll_wipe(wire, sizeof(wire));

    /* D */
    if (!store_released(c)) {
        ESP_LOGE(TAG, "release: the release record could not be stored -- the next boot carries on");
        return PB_MGMT_POLL_UNENROLL_INTERRUPTED;
    }
    /* E, with Thread still up, then F. What F came to is what is returned:
     * a release whose erase did not finish is not reported as an unenroll. */
    bool notice_acked = deliver_released_notice(c);
    return erase_and_finalize(c, notice_acked) == PB_MGMT_RELEASE_FINAL ? PB_MGMT_POLL_UNENROLLED
                                                                         : PB_MGMT_POLL_RELEASED_NOT_ERASED;
}

/* ---- finding the hub again (see pb_mgmt_client.h, "locate") ---- */

bool pb_mgmt_locator_address(const uint8_t mesh_local_prefix[8], uint16_t rloc16,
                             uint8_t out[PB_ENROLL_BRIDGE_ADDR_LEN])
{
    if (rloc16 >= 0xfffeu) {
        return false;
    }
    memcpy(out, mesh_local_prefix, 8);
    out[8] = 0x00;
    out[9] = 0x00;
    out[10] = 0x00;
    out[11] = 0xff;
    out[12] = 0xfe;
    out[13] = 0x00;
    out[14] = (uint8_t)(rloc16 >> 8);
    out[15] = (uint8_t)rloc16;
    return true;
}

static bool same_endpoint(const pb_enroll_bridge_endpoint_t *a, const pb_enroll_bridge_endpoint_t *b)
{
    return a->port == b->port && memcmp(a->addr, b->addr, PB_ENROLL_BRIDGE_ADDR_LEN) == 0;
}

/* One LOCATE to `dest`. True only for an answer that is the hub's: a
 * LOCATE_ACK made with this enrollment's key, for this board, hub and
 * generation, answering THIS request. *claimed is the endpoint it names --
 * the hub's word about where it is, nothing more yet. */
static bool locate_probe(pb_mgmt_client_t *c, const pb_enroll_bridge_endpoint_t *dest,
                         pb_enroll_bridge_endpoint_t *claimed)
{
    uint8_t wire[PB_MGMT_MAX_MESSAGE_LEN];
    uint8_t resp[PB_MGMT_MAX_MESSAGE_LEN];
    uint8_t nonce[PB_MGMT_NONCE_LEN];
    pb_mgmt_msg_t m;
    bool authentic = false;

    memset(&m, 0, sizeof(m));
    m.type = PB_MGMT_TYPE_LOCATE;
    fill_identity(c, &m.id);
    if (!c->ops.random(nonce, sizeof(nonce), c->ops.ctx) || pb_mgmt_is_zero(nonce, sizeof(nonce))) {
        return false;
    }
    memcpy(m.board_nonce, nonce, sizeof(nonce));
    size_t n = pb_mgmt_pack(&m, c->rec.mgmt.key, c->ops.hmac, c->ops.ctx, wire, sizeof(wire));
    if (n != 0) {
        c->locate_probes++;
        size_t resp_len = 0;
        pb_mgmt_msg_t a;
        if (c->ops.exchange(dest, wire, n, resp, sizeof(resp), &resp_len, c->ops.ctx) == PB_MGMT_XFER_ANSWERED &&
            pb_mgmt_parse(resp, resp_len, &a) && a.type == PB_MGMT_TYPE_LOCATE_ACK &&
            pb_mgmt_verify(resp, resp_len, c->rec.mgmt.key, c->ops.hmac, c->ops.ctx) && identity_is_ours(c, &a.id) &&
            pb_mgmt_ct_equal(a.board_nonce, nonce, sizeof(nonce))) {
            memcpy(claimed->addr, a.endpoint_addr, PB_ENROLL_BRIDGE_ADDR_LEN);
            claimed->port = a.endpoint_port;
            authentic = true;
        }
    }
    pb_enroll_wipe(wire, sizeof(wire));
    pb_enroll_wipe(resp, sizeof(resp));
    pb_enroll_wipe(nonce, sizeof(nonce));
    return authentic;
}

/* An endpoint this board may store for the network it is enrolled in: a
 * Mesh-Local EID inside the record's own Mesh-Local Prefix, a port that is
 * not 0. The same rule an enrollment's endpoint has to pass. */
static bool endpoint_is_storable(const pb_mgmt_client_t *c, const pb_enroll_bridge_endpoint_t *ep)
{
    return pb_enroll_bridge_endpoint_check(ep) == PB_ENROLL_ENDPOINT_OK &&
           pb_enroll_bridge_endpoint_in_network(ep, &c->rec.id);
}

/* The same enrollment, field by field (the struct has padding). `endpoint`
 * says whether the bridge endpoint is part of the comparison. */
static bool same_enrolled_record(const pb_enroll_record_t *a, const pb_enroll_record_t *b, bool endpoint)
{
    bool same = a->has_mgmt && b->has_mgmt && a->mgmt_state == PB_ENROLL_MGMT_ENROLLED &&
                b->mgmt_state == PB_ENROLL_MGMT_ENROLLED && a->release_flags == b->release_flags &&
                memcmp(a->id.ext_pan_id, b->id.ext_pan_id, sizeof(a->id.ext_pan_id)) == 0 &&
                memcmp(a->id.mesh_local_prefix, b->id.mesh_local_prefix, sizeof(a->id.mesh_local_prefix)) == 0 &&
                a->has_bridge_endpoint == b->has_bridge_endpoint;
    /* Evaluated whatever `same` is: no early exit on the secret. */
    bool hub = pb_mgmt_ct_equal(a->mgmt.hub_id, b->mgmt.hub_id, PB_ENROLL_HUB_ID_LEN);
    bool gen = pb_mgmt_ct_equal(a->mgmt.generation, b->mgmt.generation, PB_ENROLL_GENERATION_LEN);
    bool key = pb_mgmt_ct_equal(a->mgmt.key, b->mgmt.key, PB_ENROLL_MGMT_KEY_LEN);
    bool cmd = pb_mgmt_ct_equal(a->command_id, b->command_id, PB_ENROLL_COMMAND_ID_LEN);
    same = same && hub && gen && key && cmd;
    if (endpoint) {
        same = same && same_endpoint(&a->bridge_endpoint, &b->bridge_endpoint);
    }
    return same;
}

/* The hub confirmed `ep`, and it is not the stored endpoint: the record gets
 * it, and only then anything else does. */
static pb_mgmt_locate_result_t adopt_endpoint(pb_mgmt_client_t *c, const pb_enroll_bridge_endpoint_t *ep)
{
    pb_enroll_record_t stored;
    pb_enroll_record_t next;
    pb_enroll_record_t back;
    pb_mgmt_locate_result_t result = PB_MGMT_LOCATE_STORE_FAILED;
    memset(&next, 0, sizeof(next));

    /* What is in flash NOW must still be the enrollment this answer was
     * authenticated for. A record that was released, reset or replaced in
     * the meantime is not turned back into this one. */
    bool intact = (c->ops.locate_allowed == NULL || c->ops.locate_allowed(c->ops.ctx)) &&
                  pb_enroll_record_load(c->eui64, &stored) == PB_ENROLL_MARKER_VALID &&
                  same_enrolled_record(&stored, &c->rec, true);
    if (!intact) {
        ESP_LOGW(TAG, "locate: the stored enrollment is not the one this answer belongs to -- nothing is written");
        pb_enroll_wipe(&stored, sizeof(stored));
        return PB_MGMT_LOCATE_RECORD_CHANGED;
    }

    next = stored;
    next.has_bridge_endpoint = true;
    next.bridge_endpoint = *ep;
    esp_err_t err = pb_enroll_record_store(c->eui64, &next);
    /* A move needs both: the write reported no error AND the new record
     * reads back. Neither alone is taken for it. */
    pb_enroll_marker_status_t back_status = pb_enroll_record_load(c->eui64, &back);
    if (err == ESP_OK && back_status == PB_ENROLL_MARKER_VALID && same_enrolled_record(&back, &next, true)) {
        result = PB_MGMT_LOCATE_MOVED;
    } else if (back_status == PB_ENROLL_MARKER_VALID && same_enrolled_record(&back, &stored, true)) {
        ESP_LOGE(TAG, "locate: the new endpoint could not be stored -- the stored enrollment is unchanged and stays "
                      "in use");
    } else {
        /* The write reported an error and yet something of it is there, or
         * nothing reads back at all. What this boot uses and what the next
         * boot would find must not differ: the previous record is put back,
         * and this boot goes on with it. The move is tried again in a later
         * round. */
        ESP_LOGE(TAG, "locate: the write of the new endpoint did not end cleanly -- restoring the previous record");
        if (pb_enroll_record_store(c->eui64, &stored) != ESP_OK ||
            pb_enroll_record_load(c->eui64, &back) != PB_ENROLL_MARKER_VALID ||
            !same_enrolled_record(&back, &stored, true)) {
            ESP_LOGE(TAG, "locate: the previous record could not be restored either (what is stored is this "
                          "enrollment with the old or the new endpoint, or nothing this board resumes)");
        }
    }

    if (result == PB_MGMT_LOCATE_MOVED) {
        c->rec.has_bridge_endpoint = true;
        c->rec.bridge_endpoint = *ep;
        /* A nonce the hub gave at the old endpoint is not carried over. */
        c->have_hub_nonce = false;
        pb_enroll_wipe(c->hub_nonce, sizeof(c->hub_nonce));
        c->locate_moves++;
        ESP_LOGW(TAG, "locate: the hub confirmed a new endpoint (port %u; address hidden) -- stored, read back and in "
                      "use from now on",
                 (unsigned)ep->port);
        c->ops.endpoint_changed(ep, c->ops.ctx);
    }
    pb_enroll_wipe(&stored, sizeof(stored));
    pb_enroll_wipe(&next, sizeof(next));
    pb_enroll_wipe(&back, sizeof(back));
    return result;
}

/* Leaves in `hints` the distinct ones that lie inside this enrollment's
 * network, in the order they came, and returns how many. A hint outside the
 * record's Mesh-Local Prefix is not one. */
static size_t usable_hints(const pb_mgmt_client_t *c, uint8_t (*hints)[PB_ENROLL_BRIDGE_ADDR_LEN], size_t count)
{
    size_t kept = 0;
    for (size_t i = 0; i < count; i++) {
        if (memcmp(hints[i], c->rec.id.mesh_local_prefix, sizeof(c->rec.id.mesh_local_prefix)) != 0) {
            continue;
        }
        bool twice = false;
        for (size_t k = 0; k < kept && !twice; k++) {
            twice = memcmp(hints[k], hints[i], PB_ENROLL_BRIDGE_ADDR_LEN) == 0;
        }
        if (!twice) {
            if (kept != i) {
                memcpy(hints[kept], hints[i], PB_ENROLL_BRIDGE_ADDR_LEN);
            }
            kept++;
        }
    }
    return kept;
}

/* The candidates of a round, as one list: every usable hint on the stored
 * port, then (when that is another port) on the locate port. */
typedef struct {
    uint8_t (*hints)[PB_ENROLL_BRIDGE_ADDR_LEN];
    size_t hint_count;
    uint16_t ports[2];
    size_t port_count;
} locate_walk_t;

static size_t walk_len(const locate_walk_t *w)
{
    return w->hint_count * w->port_count;
}

static void walk_at(const locate_walk_t *w, size_t pos, pb_enroll_bridge_endpoint_t *out)
{
    memcpy(out->addr, w->hints[pos / w->port_count], PB_ENROLL_BRIDGE_ADDR_LEN);
    out->port = w->ports[pos % w->port_count];
}

/* Where this round's walk starts: behind the candidate the last round asked
 * last. That candidate is looked up by what it is, not by where it was, so
 * that a list which changed in front of it does not move the walk. If it is
 * gone -- its hint is no longer given -- the walk goes on at the place that
 * hint had: what stood behind it has moved up to there. Past the end is the
 * start. */
static size_t walk_start(const pb_mgmt_client_t *c, const locate_walk_t *w)
{
    const size_t len = walk_len(w);
    size_t start = 0;
    if (c->locate_cursor_valid) {
        start = c->locate_cursor_pos - (c->locate_cursor_pos % w->port_count);
        for (size_t pos = 0; pos < len; pos++) {
            pb_enroll_bridge_endpoint_t ep;
            walk_at(w, pos, &ep);
            if (same_endpoint(&ep, &c->locate_cursor)) {
                start = pos + 1;
                break;
            }
        }
    }
    return start < len ? start : 0;
}

/* One candidate: LOCATE to it and, when the answer is the hub's and names
 * another endpoint, LOCATE to that one too. True when the round is over,
 * with what it came to in *result. */
static bool locate_ask(pb_mgmt_client_t *c, const pb_enroll_bridge_endpoint_t *cand, unsigned *probes,
                       pb_mgmt_locate_result_t *result)
{
    pb_enroll_bridge_endpoint_t claimed;
    (*probes)++;
    if (!locate_probe(c, cand, &claimed)) {
        return false;
    }
    if (!endpoint_is_storable(c, &claimed)) {
        ESP_LOGW(TAG, "locate: the hub named an endpoint this board cannot use for its network -- ignored");
        return false;
    }
    if (!same_endpoint(&claimed, cand)) {
        /* The hub answered here and says it is THERE: it has to say so from
         * there. (Also the usual case: a hint is a routing locator, the
         * endpoint is the hub's EID.) */
        pb_enroll_bridge_endpoint_t confirmed;
        if (*probes >= PB_MGMT_LOCATE_MAX_PROBES) {
            /* No request left in this round. Where the hub says it is, is
             * the first place the next round asks -- with a new LOCATE and
             * a new nonce, and with this answer counting for nothing. */
            c->locate_pending = claimed;
            c->locate_pending_valid = true;
            ESP_LOGI(TAG, "locate: the hub answered at the last request of this round -- the endpoint it named is "
                          "asked first in the next round");
            *result = PB_MGMT_LOCATE_UNCONFIRMED;
            return true;
        }
        (*probes)++;
        if (!locate_probe(c, &claimed, &confirmed) || !same_endpoint(&confirmed, &claimed)) {
            /* Not reachable there, or it moved again while we asked. */
            return false;
        }
    }
    if (same_endpoint(&claimed, &c->rec.bridge_endpoint)) {
        ESP_LOGI(TAG, "locate: the hub confirmed the stored endpoint -- nothing to change");
        *result = PB_MGMT_LOCATE_SAME_ENDPOINT;
        return true;
    }
    *result = adopt_endpoint(c, &claimed);
    if (*result == PB_MGMT_LOCATE_STORE_FAILED) {
        /* The walk has passed the place that led here. Storing is tried
         * again in the next round all the same: the endpoint is the first
         * place that round asks, like any other candidate. */
        c->locate_pending = claimed;
        c->locate_pending_valid = true;
    }
    return true;
}

/* One round: at most PB_MGMT_LOCATE_MAX_PROBES requests, then it is over
 * whatever came of it. The next round goes on where this one stopped (see
 * the top comment of pb_mgmt_client.h). */
static pb_mgmt_locate_result_t locate_round(pb_mgmt_client_t *c)
{
    const pb_enroll_bridge_endpoint_t stored = c->rec.bridge_endpoint;
    pb_mgmt_locate_result_t result = PB_MGMT_LOCATE_NOT_FOUND;
    unsigned probes = 0;

    /* The hints, all of them, read once per round and before anything is
     * sent. */
    memset(s_locate_hints, 0, sizeof(s_locate_hints));
    size_t hint_count = c->ops.locate_hints(s_locate_hints, PB_MGMT_LOCATE_MAX_HINTS, c->ops.ctx);
    if (hint_count > PB_MGMT_LOCATE_MAX_HINTS) {
        hint_count = PB_MGMT_LOCATE_MAX_HINTS;
    }
    locate_walk_t walk = {.hints = s_locate_hints, .hint_count = usable_hints(c, s_locate_hints, hint_count)};
    if (stored.port != 0) {
        walk.ports[walk.port_count++] = stored.port;
    }
    if (stored.port != (uint16_t)PB_MGMT_LOCATE_PORT) {
        walk.ports[walk.port_count++] = (uint16_t)PB_MGMT_LOCATE_PORT;
    }

    /* 1. What the last round was told and could not ask. Asked like any
     * other candidate; kept for this one round only, and only while it is
     * still an endpoint of this enrollment's network. */
    pb_enroll_bridge_endpoint_t carried;
    const bool have_carried = c->locate_pending_valid && endpoint_is_storable(c, &c->locate_pending);
    carried = c->locate_pending;
    c->locate_pending_valid = false;
    memset(&c->locate_pending, 0, sizeof(c->locate_pending));
    bool over = have_carried && locate_ask(c, &carried, &probes, &result);

    /* 2. The stored address on the locate port -- a hub whose listener only
     * moved to another port still answers LOCATE there (when that is the
     * stored endpoint itself, the question still makes sense: the poll that
     * went unanswered there was another message). */
    pb_enroll_bridge_endpoint_t head = stored;
    head.port = (uint16_t)PB_MGMT_LOCATE_PORT;
    if (!over && !(have_carried && same_endpoint(&head, &carried)) && probes < PB_MGMT_LOCATE_MAX_PROBES) {
        over = locate_ask(c, &head, &probes, &result);
    }

    /* 3. The walk through the candidates, from where the last round
     * stopped. */
    const size_t len = walk_len(&walk);
    const size_t start = len != 0 ? walk_start(c, &walk) : 0;
    for (size_t step = 0; step < len && probes < PB_MGMT_LOCATE_MAX_PROBES && !over; step++) {
        const size_t pos = (start + step) % len;
        pb_enroll_bridge_endpoint_t cand;
        walk_at(&walk, pos, &cand);
        /* Passed, whether it is asked now or was asked a moment ago. */
        c->locate_cursor = cand;
        c->locate_cursor_pos = (uint16_t)pos;
        c->locate_cursor_valid = true;
        if (same_endpoint(&cand, &head) || (have_carried && same_endpoint(&cand, &carried))) {
            continue; /* nothing twice in one round */
        }
        over = locate_ask(c, &cand, &probes, &result);
    }
    memset(s_locate_hints, 0, sizeof(s_locate_hints));
    return over ? result : PB_MGMT_LOCATE_NOT_FOUND;
}

/* Called for every poll that brought no authenticated answer. */
static pb_mgmt_poll_result_t note_silent_poll(pb_mgmt_client_t *c)
{
    if (c->silent_polls < UINT32_MAX) {
        c->silent_polls++;
    }
    if (!locate_supported(c) || !c->rec.has_bridge_endpoint || c->silent_polls < c->next_locate_at) {
        return PB_MGMT_POLL_NO_ANSWER;
    }
    uint8_t gap = c->locate_rounds < PB_MGMT_LOCATE_ROUND_GAPS ? c->locate_rounds : PB_MGMT_LOCATE_ROUND_GAPS - 1;
    c->next_locate_at = c->silent_polls + k_locate_round_gap_polls[gap];
    if (c->locate_rounds < UINT8_MAX) {
        c->locate_rounds++;
    }
    ESP_LOGW(TAG, "no authenticated answer from the hub for %u polls -- looking for it in this Thread network "
                  "(round %u)",
             (unsigned)c->silent_polls, (unsigned)c->locate_rounds);
    c->last_locate = locate_round(c);
    if (c->last_locate != PB_MGMT_LOCATE_MOVED) {
        return PB_MGMT_POLL_NO_ANSWER;
    }
    c->silent_polls = 0;
    c->next_locate_at = PB_MGMT_LOCATE_AFTER_SILENT_POLLS;
    c->locate_rounds = 0;
    forget_locate_progress(c);
    return PB_MGMT_POLL_ENDPOINT_MOVED;
}

static void note_authenticated_answer(pb_mgmt_client_t *c)
{
    c->silent_polls = 0;
    c->next_locate_at = PB_MGMT_LOCATE_AFTER_SILENT_POLLS;
    c->locate_rounds = 0;
    c->last_locate = PB_MGMT_LOCATE_NONE;
    forget_locate_progress(c);
}

pb_mgmt_poll_result_t pb_mgmt_client_poll(pb_mgmt_client_t *c)
{
    if (!c->armed || c->rec.mgmt_state != PB_ENROLL_MGMT_ENROLLED) {
        return PB_MGMT_POLL_NOT_ARMED;
    }

    uint8_t wire[PB_MGMT_MAX_MESSAGE_LEN];
    uint8_t resp[PB_MGMT_MAX_MESSAGE_LEN];
    uint8_t nonce[PB_MGMT_NONCE_LEN];
    pb_mgmt_msg_t m;
    pb_mgmt_poll_result_t result = PB_MGMT_POLL_NO_ANSWER;
    bool answered = false;

    memset(&m, 0, sizeof(m));
    m.type = PB_MGMT_TYPE_POLL;
    fill_identity(c, &m.id);
    /* A nonce of zeros is not a nonce (and is not a valid POLL). Nothing was
     * sent: not a poll the hub failed to answer. */
    if (!c->ops.random(nonce, sizeof(nonce), c->ops.ctx) || pb_mgmt_is_zero(nonce, sizeof(nonce))) {
        return PB_MGMT_POLL_NO_ANSWER;
    }
    memcpy(m.board_nonce, nonce, sizeof(nonce));
    if (c->have_hub_nonce) {
        memcpy(m.hub_nonce, c->hub_nonce, sizeof(m.hub_nonce));
    }
    m.state = PB_MGMT_STATE_ENROLLED;
    /* The hub's nonce is good for one poll, answered or not. */
    c->have_hub_nonce = false;
    pb_enroll_wipe(c->hub_nonce, sizeof(c->hub_nonce));

    size_t n = pb_mgmt_pack(&m, c->rec.mgmt.key, c->ops.hmac, c->ops.ctx, wire, sizeof(wire));
    if (n == 0) {
        answered = true; /* nothing was sent either */
        goto out;
    }
    c->polls++;
    size_t resp_len = 0;
    pb_mgmt_xfer_t x = c->ops.exchange(&c->rec.bridge_endpoint, wire, n, resp, sizeof(resp), &resp_len, c->ops.ctx);
    if (x != PB_MGMT_XFER_ANSWERED) {
        goto out;
    }

    /* The answer must be: well formed, made with this enrollment's key, for
     * this board, hub and generation, and an answer to THIS poll. Anything
     * else -- another board's or hub's message, an older generation's, a
     * replay from before this boot or this poll, a changed byte -- is
     * dropped without a trace in the stored state. */
    pb_mgmt_msg_t a;
    bool authentic = pb_mgmt_parse(resp, resp_len, &a) &&
                     (a.type == PB_MGMT_TYPE_IDLE || a.type == PB_MGMT_TYPE_UNENROLL) &&
                     pb_mgmt_verify(resp, resp_len, c->rec.mgmt.key, c->ops.hmac, c->ops.ctx) &&
                     identity_is_ours(c, &a.id) && pb_mgmt_ct_equal(a.board_nonce, nonce, sizeof(nonce));
    if (!authentic) {
        c->unauthentic_answers++;
        goto out;
    }
    note_authenticated_answer(c);
    answered = true;

    if (a.type == PB_MGMT_TYPE_IDLE) {
        if (!pb_mgmt_is_zero(a.hub_nonce, sizeof(a.hub_nonce))) {
            memcpy(c->hub_nonce, a.hub_nonce, sizeof(c->hub_nonce));
            c->have_hub_nonce = true;
        }
        result = (a.flags & PB_MGMT_IDLE_F_COMMAND_PENDING) != 0 ? PB_MGMT_POLL_COMMAND_PENDING : PB_MGMT_POLL_IDLE;
        announce_capabilities(c);
        goto out;
    }

    /* UNENROLL. */
    uint8_t command_id[PB_MGMT_COMMAND_ID_LEN];
    memcpy(command_id, a.command_id, sizeof(command_id));
    pb_enroll_wipe(wire, sizeof(wire));
    pb_enroll_wipe(resp, sizeof(resp));
    pb_enroll_wipe(nonce, sizeof(nonce));
    return execute_unenroll(c, command_id);

out:
    pb_enroll_wipe(wire, sizeof(wire));
    pb_enroll_wipe(resp, sizeof(resp));
    pb_enroll_wipe(nonce, sizeof(nonce));
    if (!answered) {
        /* Nothing the hub said reached this board in this poll. Enough of
         * those in a row and it is looked for (bounded, spaced out). */
        result = note_silent_poll(c);
    }
    return result;
}

pb_mgmt_release_result_t pb_mgmt_client_resume_release(pb_mgmt_client_t *c)
{
    if (!c->rec.has_mgmt) {
        return PB_MGMT_RELEASE_NOTHING;
    }
    if (c->rec.mgmt_state == PB_ENROLL_MGMT_UNENROLLING) {
        /* The command was journaled and the power went before the release
         * was stored: store it now. No network is needed for that. */
        ESP_LOGW(TAG, "boot: an unenroll command was journaled -- completing the release (no service is started)");
        if (!store_released(c)) {
            ESP_LOGE(TAG, "boot: the release record could not be stored -- the next boot tries again");
            return PB_MGMT_RELEASE_INCOMPLETE;
        }
    }
    if (c->rec.mgmt_state != PB_ENROLL_MGMT_RELEASED ||
        (c->rec.release_flags & PB_ENROLL_RELEASE_F_ERASE_PENDING) == 0) {
        return PB_MGMT_RELEASE_NOTHING;
    }
    ESP_LOGW(TAG, "boot: this board is released and its stored network is not confirmed erased -- telling the hub "
                  "if a network is left, then erasing and checking");
    bool notice_acked = false;
    if (c->ops.network_up_for_proof(c->ops.ctx)) {
        notice_acked = deliver_released_notice(c);
    } else {
        /* Also the case when an earlier boot erased the store and then could
         * not write the final record. */
        ESP_LOGW(TAG, "boot: no network to send the released notice on");
    }
    return erase_and_finalize(c, notice_acked);
}
