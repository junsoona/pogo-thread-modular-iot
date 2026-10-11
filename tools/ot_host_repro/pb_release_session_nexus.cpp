/*
 * Minimal reproduction, with the REAL OpenThread core, of the assumptions
 * behind a Power Base sending its release receipt over Thread after it has
 * erased its stored network (docs/power-base-management.md, section 4):
 *
 *   control    after the erase, attaching again stores the Dataset again, and
 *              the next boot resumes that network -- which is why the receipt
 *              was only ever handed out over BLE;
 *   hazard     attaching again in the same boot with the SAME extended
 *              address but fresh frame counters is dropped as a replay by a
 *              router that still knows the board;
 *   session    with components/power_base_ot_store engaged and a new random
 *              extended address the board attaches as a child, exchanges a
 *              confirmable CoAP request with the hub, and its persistent
 *              store stays empty throughout; the next boot has no network.
 *
 * OpenThread is the copy ESP-IDF v6.1 builds the firmware from
 * ($IDF_PATH/components/openthread/openthread), run on its own in-process
 * "Nexus" simulation: two instances, a simulated radio, virtual time. The
 * board's "flash" is that node's settings object (it survives Node::Reset(),
 * as flash survives a restart); components/power_base_ot_store is the very file
 * the firmware links.
 *
 * NOT covered here, and said so in the report: this build has no key
 * references, so the Network Key is part of the stored Dataset and PSA
 * Internal Trusted Storage is never used (tools/ot_host_repro/
 * pb_volatile_store_psa.c covers that store with the real PSA key store);
 * the ESP-IDF settings port, NVS, the radio and the link-time --wrap are not
 * in this program either.
 *
 * Built and run by tools/test_power_base_release_session_openthread.sh.
 */
#include <stdio.h>
#include <string.h>

#include <openthread/coap.h>
#include <openthread/dataset.h>
#include <openthread/instance.h>
#include <openthread/ip6.h>
#include <openthread/link.h>
#include <openthread/thread.h>
#include <openthread/thread_ftd.h>

#include "platform/nexus_core.hpp"
#include "platform/nexus_node.hpp"

extern "C" {
#include "power_base_ot_store/pb_volatile_store.h"
#include "psa_crypto_its.h"

void pb_volatile_store_test_reset(void);

/* components/power_base_ot_store, as the firmware links it. */
otError __wrap_otPlatSettingsGet(otInstance *, uint16_t, int, uint8_t *, uint16_t *);
otError __wrap_otPlatSettingsSet(otInstance *, uint16_t, const uint8_t *, uint16_t);
otError __wrap_otPlatSettingsAdd(otInstance *, uint16_t, const uint8_t *, uint16_t);
otError __wrap_otPlatSettingsDelete(otInstance *, uint16_t, int);
void __wrap_otPlatSettingsWipe(otInstance *);
psa_status_t __wrap_psa_its_get_info(psa_storage_uid_t, struct psa_storage_info_t *);

/* The Nexus platform's own settings functions (nexus_settings.cpp, compiled
 * for this program under these names). */
otError __real_otPlatSettingsGet(otInstance *, uint16_t, int, uint8_t *, uint16_t *);
otError __real_otPlatSettingsSet(otInstance *, uint16_t, const uint8_t *, uint16_t);
otError __real_otPlatSettingsAdd(otInstance *, uint16_t, const uint8_t *, uint16_t);
otError __real_otPlatSettingsDelete(otInstance *, uint16_t, int);
void __real_otPlatSettingsWipe(otInstance *);
}

namespace ot {
namespace Nexus {

/* The one instance whose stores go through the switch. The hub's go straight
 * to its own: on the real hardware the two are different machines. */
static Node *sBoard = nullptr;

static bool IsBoard(otInstance *aInstance)
{
    return sBoard != nullptr && aInstance == static_cast<otInstance *>(&sBoard->GetInstance());
}

static int sFailures = 0;

static void Check(bool aCondition, const char *aWhat)
{
    printf("%s %s\n", aCondition ? "ok:  " : "FAIL:", aWhat);
    if (!aCondition)
    {
        sFailures++;
    }
}

extern "C" {

/* What OpenThread calls. With --wrap the linker does this for the firmware;
 * here it is spelled out, per instance. */
otError otPlatSettingsGet(otInstance *aInstance, uint16_t aKey, int aIndex, uint8_t *aValue, uint16_t *aValueLength)
{
    return IsBoard(aInstance) ? __wrap_otPlatSettingsGet(aInstance, aKey, aIndex, aValue, aValueLength)
                              : __real_otPlatSettingsGet(aInstance, aKey, aIndex, aValue, aValueLength);
}

otError otPlatSettingsSet(otInstance *aInstance, uint16_t aKey, const uint8_t *aValue, uint16_t aValueLength)
{
    return IsBoard(aInstance) ? __wrap_otPlatSettingsSet(aInstance, aKey, aValue, aValueLength)
                              : __real_otPlatSettingsSet(aInstance, aKey, aValue, aValueLength);
}

otError otPlatSettingsAdd(otInstance *aInstance, uint16_t aKey, const uint8_t *aValue, uint16_t aValueLength)
{
    return IsBoard(aInstance) ? __wrap_otPlatSettingsAdd(aInstance, aKey, aValue, aValueLength)
                              : __real_otPlatSettingsAdd(aInstance, aKey, aValue, aValueLength);
}

otError otPlatSettingsDelete(otInstance *aInstance, uint16_t aKey, int aIndex)
{
    return IsBoard(aInstance) ? __wrap_otPlatSettingsDelete(aInstance, aKey, aIndex)
                              : __real_otPlatSettingsDelete(aInstance, aKey, aIndex);
}

void otPlatSettingsWipe(otInstance *aInstance)
{
    if (IsBoard(aInstance))
    {
        __wrap_otPlatSettingsWipe(aInstance);
    }
    else
    {
        __real_otPlatSettingsWipe(aInstance);
    }
}

/* PSA ITS is not part of this build: the component's own check needs the
 * names, and nothing else may call them. */
static unsigned sRealItsCalls = 0;

psa_status_t psa_its_get_info(psa_storage_uid_t aUid, struct psa_storage_info_t *aInfo)
{
    return __wrap_psa_its_get_info(aUid, aInfo);
}

psa_status_t __real_psa_its_get_info(psa_storage_uid_t, struct psa_storage_info_t *)
{
    sRealItsCalls++;
    return PSA_ERROR_DOES_NOT_EXIST;
}

psa_status_t __real_psa_its_get(psa_storage_uid_t, uint32_t, uint32_t, void *, size_t *)
{
    sRealItsCalls++;
    return PSA_ERROR_DOES_NOT_EXIST;
}

psa_status_t __real_psa_its_set(psa_storage_uid_t, uint32_t, const void *, psa_storage_create_flags_t)
{
    sRealItsCalls++;
    return PSA_ERROR_INSUFFICIENT_STORAGE;
}

psa_status_t __real_psa_its_remove(psa_storage_uid_t)
{
    sRealItsCalls++;
    return PSA_ERROR_DOES_NOT_EXIST;
}

} // extern "C"

/* ---- the board's "flash", read directly ---- */

static unsigned StoreEntries(const Node &aNode)
{
    unsigned count = 0;

    for (const Settings::Entry &entry : aNode.mSettings.mEntries)
    {
        OT_UNUSED_VARIABLE(entry);
        count++;
    }
    return count;
}

static bool StoreHasDataset(const Node &aNode)
{
    return aNode.mSettings.mEntries.FindMatching(static_cast<uint16_t>(SettingsBase::kKeyActiveDataset)) != nullptr;
}

/* ---- the hub's management resource and the board's request ---- */

static const char kMgmtPath[]  = "pogo/v1/mgmt";
static const uint16_t kHubPort = 5684;

/* Stand-ins for the 96-byte receipt and its acknowledgement: only their
 * arrival matters here (their MACs are the business of the management code
 * and its own tests). */
static uint8_t sReceipt[96];
static uint8_t sAck[96];

struct Exchange
{
    otInstance *mHub;
    unsigned    mRequests;
    bool        mRequestIntact;
    bool        mAnswered;
    bool        mAnswerIntact;
    otError     mResult;
};

static Exchange sExchange;

static void HandleMgmtRequest(void *aContext, otMessage *aMessage, const otMessageInfo *aMessageInfo)
{
    Exchange  *exchange = static_cast<Exchange *>(aContext);
    uint8_t    payload[sizeof(sReceipt)];
    uint16_t   offset = otMessageGetOffset(aMessage);
    uint16_t   length = static_cast<uint16_t>(otMessageGetLength(aMessage) - offset);
    otMessage *answer;

    exchange->mRequests++;
    exchange->mRequestIntact = length == sizeof(payload) &&
                               otMessageRead(aMessage, offset, payload, sizeof(payload)) == sizeof(payload) &&
                               memcmp(payload, sReceipt, sizeof(payload)) == 0;

    answer = otCoapNewMessage(exchange->mHub, nullptr);
    VerifyOrQuit(answer != nullptr);
    SuccessOrQuit(otCoapMessageInitResponse(answer, aMessage, OT_COAP_TYPE_ACKNOWLEDGMENT, OT_COAP_CODE_CONTENT));
    SuccessOrQuit(otCoapMessageSetPayloadMarker(answer));
    SuccessOrQuit(otMessageAppend(answer, sAck, sizeof(sAck)));
    SuccessOrQuit(otCoapSendResponse(exchange->mHub, answer, aMessageInfo));
}

static void HandleMgmtAnswer(void *aContext, otMessage *aMessage, const otMessageInfo *, otError aResult)
{
    Exchange *exchange = static_cast<Exchange *>(aContext);
    uint8_t   payload[sizeof(sAck)];

    exchange->mAnswered = true;
    exchange->mResult   = aResult;
    if (aResult == OT_ERROR_NONE && aMessage != nullptr)
    {
        uint16_t offset = otMessageGetOffset(aMessage);
        uint16_t length = static_cast<uint16_t>(otMessageGetLength(aMessage) - offset);

        exchange->mAnswerIntact = otCoapMessageGetCode(aMessage) == OT_COAP_CODE_CONTENT &&
                                  length == sizeof(payload) &&
                                  otMessageRead(aMessage, offset, payload, sizeof(payload)) == sizeof(payload) &&
                                  memcmp(payload, sAck, sizeof(payload)) == 0;
    }
}

static void SendReceipt(otInstance *aBoard, const otIp6Address &aHubAddress)
{
    otMessage    *request = otCoapNewMessage(aBoard, nullptr);
    otMessageInfo info;

    VerifyOrQuit(request != nullptr);
    otCoapMessageInit(request, OT_COAP_TYPE_CONFIRMABLE, OT_COAP_CODE_POST);
    SuccessOrQuit(otCoapMessageAppendUriPathOptions(request, kMgmtPath));
    SuccessOrQuit(otCoapMessageSetPayloadMarker(request));
    SuccessOrQuit(otMessageAppend(request, sReceipt, sizeof(sReceipt)));
    memset(&info, 0, sizeof(info));
    info.mPeerAddr = aHubAddress;
    info.mPeerPort = kHubPort;
    SuccessOrQuit(otCoapSendRequest(aBoard, request, &info, HandleMgmtAnswer, &sExchange));
}

/* ---- one run ---- */

enum Variant
{
    kControlNoSwitch,    // attach again without the switch
    kHazardSameAddress,  // the switch, but the board keeps its extended address
    kSession,            // the switch and a new random extended address: what the firmware does
};

static void Run(Variant aVariant)
{
    static const char *const kNames[] = {"control (no switch)", "hazard (same extended address)", "session"};

    Core                     nexus;
    Node                    &hub   = nexus.CreateNode();
    Node                    &board = nexus.CreateNode();
    otInstance              *hubInstance   = &hub.GetInstance();
    otInstance              *boardInstance = &board.GetInstance();
    otOperationalDatasetTlvs tlvs;
    otCoapResource           resource;
    otIp6Address             hubAddress;
    NetworkKey               noKey;
    pb_volatile_store_stats_t stats;
    uint32_t                 attachMs;

    printf("\n==== %s ====\n", kNames[aVariant]);

    pb_volatile_store_test_reset();
    sBoard        = &board;
    sRealItsCalls = 0;
    memset(&sExchange, 0, sizeof(sExchange));
    sExchange.mHub = hubInstance;
    for (size_t i = 0; i < sizeof(sReceipt); i++)
    {
        sReceipt[i] = static_cast<uint8_t>(0x40 + i);
        sAck[i]     = static_cast<uint8_t>(0xc0 - i);
    }

    nexus.AdvanceTime(0);

    /* The hub's border router forms the network; the board joins it as the
     * always-on, router-eligible FTD a Power Base is. */
    hub.Form();
    nexus.AdvanceTime(13 * 1000);
    VerifyOrQuit(hub.Get<Mle::Mle>().IsLeader());
    board.Join(hub);
    nexus.AdvanceTime(300 * 1000);
    VerifyOrQuit(board.Get<Mle::Mle>().IsRouter());
    Check(StoreHasDataset(board), "enrolled: the board's store holds the Active Dataset");

    SuccessOrQuit(otCoapStart(hubInstance, kHubPort));
    memset(&resource, 0, sizeof(resource));
    resource.mUriPath = kMgmtPath;
    resource.mHandler = HandleMgmtRequest;
    resource.mContext = &sExchange;
    otCoapAddResource(hubInstance, &resource);
    hubAddress = *otThreadGetMeshLocalEid(hubInstance);

    /* A network that has been running for a while is past key sequence 0;
     * a board that comes back with a fresh Dataset starts at 0. */
    hub.Get<KeyManager>().SetCurrentKeySequence(3, KeyManager::kForceUpdate);
    nexus.AdvanceTime(120 * 1000);
    VerifyOrQuit(board.Get<KeyManager>().GetCurrentKeySequence() == 3);

    /* What the firmware does in its erase step: the Dataset is read into RAM,
     * Thread goes down, the persistent store is erased and read back. */
    SuccessOrQuit(otDatasetGetActiveTlvs(boardInstance, &tlvs));
    SuccessOrQuit(otThreadSetEnabled(boardInstance, false));
    SuccessOrQuit(otIp6SetEnabled(boardInstance, false));
    SuccessOrQuit(otInstanceErasePersistentInfo(boardInstance));
    Check(StoreEntries(board) == 0, "erased: the board's store is empty");
    /* On ESP-IDF the erase also destroys the Network Key (a persistent PSA
     * key there). This build keeps the key in RAM, so that is done by hand:
     * setting the Dataset below is then a key change, which resets the frame
     * counters and the key sequence -- as it does on the board. */
    noKey.Clear();
    board.Get<KeyManager>().SetNetworkKey(noKey);

    /* The session. */
    if (aVariant != kControlNoSwitch)
    {
        Check(pb_volatile_store_engage(boardInstance), "the switch is engaged and in the path of both stores");
    }
    if (aVariant != kHazardSameAddress)
    {
        otExtAddress fresh;

        SuccessOrQuit(Random::Crypto::Fill(fresh));
        SuccessOrQuit(otLinkSetExtendedAddress(boardInstance, &fresh));
    }
    SuccessOrQuit(otThreadSetRouterEligible(boardInstance, false));
    SuccessOrQuit(otDatasetSetActiveTlvs(boardInstance, &tlvs));
    SuccessOrQuit(otIp6SetEnabled(boardInstance, true));
    SuccessOrQuit(otThreadSetEnabled(boardInstance, true));
    for (attachMs = 0; attachMs < 30 * 1000 && !board.Get<Mle::Mle>().IsAttached(); attachMs += 100)
    {
        nexus.AdvanceTime(100);
    }

    if (aVariant == kHazardSameAddress)
    {
        Check(!board.Get<Mle::Mle>().IsAttached(),
              "same extended address, fresh counters: not attached after 30 s (dropped as a replay)");
        Check(StoreEntries(board) == 0, "and the store is still empty");
        sBoard = nullptr;
        return;
    }

    Check(board.Get<Mle::Mle>().IsChild(), "attached again within 30 s, as a child (not router-eligible)");
    printf("      (attached after %u ms of simulated time)\n", static_cast<unsigned>(attachMs));

    if (aVariant == kSession)
    {
        /* Where the firmware reads the flash back, before it sends. */
        Check(StoreEntries(board) == 0, "attached, nothing sent yet: the board's store is empty");
    }
    else
    {
        Check(StoreHasDataset(board), "control: attached, and the Active Dataset is in the store before anything is sent");
    }

    SuccessOrQuit(otCoapStart(boardInstance, OT_DEFAULT_COAP_PORT));
    SendReceipt(boardInstance, hubAddress);
    nexus.AdvanceTime(10 * 1000);
    Check(sExchange.mRequests == 1 && sExchange.mRequestIntact, "the hub received the confirmable request, intact");
    Check(sExchange.mAnswered && sExchange.mResult == OT_ERROR_NONE && sExchange.mAnswerIntact,
          "the board received the hub's answer, intact");

    if (aVariant == kSession)
    {
        Check(StoreEntries(board) == 0, "while attached: the board's store is empty");
    }

    SuccessOrQuit(otCoapStop(boardInstance));
    SuccessOrQuit(otThreadSetEnabled(boardInstance, false));
    SuccessOrQuit(otIp6SetEnabled(boardInstance, false));

    pb_volatile_store_get_stats(&stats);
    if (aVariant == kSession)
    {
        Check(StoreEntries(board) == 0, "after the session: the board's store is empty");
        Check(stats.settings_kept > 0, "OpenThread did write its settings during the session -- into RAM");
        Check(stats.settings_refused == 0, "and everything it wrote fitted");
        printf("      (settings writes kept in RAM: %u)\n", static_cast<unsigned>(stats.settings_kept));
    }
    else
    {
        Check(StoreHasDataset(board), "control: the attach stored the Active Dataset again");
    }
    Check(sRealItsCalls == 0, "nothing reached the (absent) PSA store");

    /* The restart: RAM is gone, flash is what it was. */
    pb_volatile_store_test_reset();
    board.Reset();
    nexus.AdvanceTime(1000);
    if (aVariant == kSession)
    {
        Check(!otDatasetIsCommissioned(boardInstance), "after a restart: no Dataset");
        /* What a later boot's check of a final record relies on: a start on
         * an erased store, with Thread left disabled, stores nothing. (The
         * key OpenThread imports for itself at a start goes to the PSA key
         * store, which this build does not have.) */
        nexus.AdvanceTime(10 * 60 * 1000);
        Check(StoreEntries(board) == 0, "after a restart, Thread never enabled, ten minutes on: the store is still empty");
        /* The firmware never enables Thread without a stored Dataset. Even
         * if something did: */
        IgnoreError(otIp6SetEnabled(boardInstance, true));
        IgnoreError(otThreadSetEnabled(boardInstance, true));
        nexus.AdvanceTime(60 * 1000);
        Check(!board.Get<Mle::Mle>().IsAttached(), "after a restart: not attached, even with Thread enabled");
    }
    else
    {
        Check(otDatasetIsCommissioned(boardInstance), "control: after a restart the Dataset is back");
        SuccessOrQuit(otIp6SetEnabled(boardInstance, true));
        SuccessOrQuit(otThreadSetEnabled(boardInstance, true));
        nexus.AdvanceTime(60 * 1000);
        Check(board.Get<Mle::Mle>().IsAttached(), "control: after a restart the board attaches to the erased network");
    }
    sBoard = nullptr;
}

} // namespace Nexus
} // namespace ot

int main(void)
{
    ot::Nexus::Run(ot::Nexus::kControlNoSwitch);
    ot::Nexus::Run(ot::Nexus::kHazardSameAddress);
    ot::Nexus::Run(ot::Nexus::kSession);
    if (ot::Nexus::sFailures != 0)
    {
        printf("\n%d check(s) FAILED\n", ot::Nexus::sFailures);
        return 1;
    }
    printf("\nAll checks passed\n");
    return 0;
}
