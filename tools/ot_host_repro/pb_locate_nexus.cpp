/*
 * "The same hub at another Thread address" (docs/power-base-management.md,
 * section 12) on the REAL OpenThread core -- the sources ESP-IDF builds the
 * firmware from ($IDF_PATH/components/openthread/openthread), running on
 * OpenThread's own in-process "Nexus" simulation: several instances, a
 * simulated radio, virtual time.
 *
 * What runs unmodified:
 *   - the board's management client (components/power_base_mgmt:
 *     pb_mgmt_client.c, pb_mgmt_proto.c) and its enrollment record
 *     (components/power_base_enrollment, on the NVS simulator);
 *   - the firmware's hint collection, power_base/main/enrollment_locate_hints.c
 *     -- the same file the firmware links -- reading the board instance's
 *     real Thread Network Data and router table;
 *   - OpenThread: MLE, the leader's Network Data, routing to a routing
 *     locator, the CoAP client and server.
 *
 * What the test plays: the hub. A CoAP resource "pogo/v1/mgmt" on the node
 * that is "the Linux OTBR host"; it checks a request's MAC with the hub's key
 * and answers POLL with IDLE and LOCATE with LOCATE_ACK naming that node's
 * own Mesh-Local EID, through the same pb_mgmt_proto.c. Like the Linux
 * bridge, it answers from the address the request was sent to (OpenThread's
 * CoAP server does that by itself).
 *
 * The questions this answers, which no test without a Thread stack can:
 *   1. Does a board find the node that is its hub's OTBR among the hints the
 *      firmware collects -- when that node is in the Network Data as a border
 *      router, and when it is in there as nothing at all?
 *   2. Does a LOCATE sent to that node's routing locator reach it, and does
 *      its answer, sent from the locator, get back to the board's CoAP
 *      client as the answer to that request?
 *   3. After the OTBR was replaced by a new device with the same Dataset --
 *      another extended address, another Mesh-Local EID, another RLOC16,
 *      another leader -- does the board end up at the new EID, by itself?
 *   4. Is a node that answers LOCATE with another hub's key passed over?
 *   5. In a network with more routers than one round can ask (and more than
 *      the eight hints the collection once stopped at): does the firmware's
 *      collection give ALL of them, and does the board get to a hub that
 *      sits on the LAST one, in a later round?
 *
 * What this is NOT: the ESP-IDF port, a radio, NVS, a board; and not Linux --
 * the hub here is an OpenThread CoAP server, not otbr-agent's wpan0 and the
 * bridge's UDP socket (those are in linux/matter_bridge/ingest/tests and
 * linux/matter_bridge/app/api/tests, without Thread). That the kernel
 * delivers a datagram for the locator address to the bridge's socket on a
 * real OTBR is not shown here.
 *
 * Built and run by tools/test_power_base_locate_openthread.sh.
 */
#include <stdio.h>
#include <string.h>

#include <openthread/border_router.h>
#include <openthread/coap.h>
#include <openthread/dataset.h>
#include <openthread/instance.h>
#include <openthread/ip6.h>
#include <openthread/netdata.h>
#include <openthread/thread.h>
#include <openthread/thread_ftd.h>

#include "platform/nexus_core.hpp"
#include "platform/nexus_node.hpp"

extern "C" {
#include "enrollment_locate_hints.h"
#include "nvs_sim.h"
#include "power_base_enrollment/power_base_enrollment.h"
#include "power_base_mgmt/pb_mgmt_client.h"
#include "power_base_mgmt/pb_mgmt_proto.h"

bool pb_locate_glue_hmac(const uint8_t *key, size_t key_len, const uint8_t *data, size_t len, uint8_t out[32],
                         void *ctx);
}

namespace ot {
namespace Nexus {

static int sFailures = 0;

static void Check(bool aCondition, const char *aWhat)
{
    printf("%s %s\n", aCondition ? "ok:  " : "FAIL:", aWhat);
    if (!aCondition)
    {
        sFailures++;
    }
}

static const char     kMgmtPath[] = "pogo/v1/mgmt";
static const uint16_t kHubPort    = 5684;
static const uint8_t  kEui[8]     = {0x60, 0x55, 0xf9, 0xff, 0xfe, 0x00, 0x12, 0x34};

static Core *sNexus = nullptr;

static bool IsLocator(const otIp6Address &aAddress)
{
    static const uint8_t kLocatorIid[6] = {0x00, 0x00, 0x00, 0xff, 0xfe, 0x00};

    return memcmp(&aAddress.mFields.m8[8], kLocatorIid, sizeof(kLocatorIid)) == 0;
}

/* ---- a hub: the management resource on one node ---- */

struct Hub
{
    otInstance    *mInstance;
    otCoapResource mResource;
    uint8_t        mHubId[16];
    uint8_t        mGeneration[16];
    uint8_t        mKey[32];
    unsigned       mPolls;
    unsigned       mLocatesAtLocator; /* LOCATEs that arrived addressed to a routing locator of this node */
    unsigned       mLocatesAtEid;     /* ... addressed to its Mesh-Local EID */
    unsigned       mRefused;
    uint8_t        mNonceCounter;
};

static void HubHandle(void *aContext, otMessage *aMessage, const otMessageInfo *aMessageInfo)
{
    Hub          *hub = static_cast<Hub *>(aContext);
    uint8_t       request[PB_MGMT_MAX_MESSAGE_LEN];
    uint8_t       wire[PB_MGMT_MAX_MESSAGE_LEN];
    uint16_t      offset = otMessageGetOffset(aMessage);
    uint16_t      length = static_cast<uint16_t>(otMessageGetLength(aMessage) - offset);
    pb_mgmt_msg_t m;
    pb_mgmt_msg_t a;
    size_t        n     = 0;
    otMessage    *reply = otCoapNewMessage(hub->mInstance, nullptr);

    VerifyOrQuit(reply != nullptr);
    memset(&a, 0, sizeof(a));
    if (length <= sizeof(request) && otMessageRead(aMessage, offset, request, length) == length &&
        pb_mgmt_parse(request, length, &m) && pb_mgmt_verify(request, length, hub->mKey, pb_locate_glue_hmac, nullptr) &&
        memcmp(m.id.hub_id, hub->mHubId, 16) == 0 && memcmp(m.id.generation, hub->mGeneration, 16) == 0)
    {
        a.id = m.id;
        if (m.type == PB_MGMT_TYPE_LOCATE)
        {
            /* What the Linux bridge answers with: this host's own Mesh-Local
             * EID, read when asked, and its ingest port. */
            const otIp6Address *eid = otThreadGetMeshLocalEid(hub->mInstance);

            if (IsLocator(aMessageInfo->mSockAddr))
            {
                hub->mLocatesAtLocator++;
            }
            else if (memcmp(&aMessageInfo->mSockAddr, eid, sizeof(*eid)) == 0)
            {
                hub->mLocatesAtEid++;
            }
            a.type = PB_MGMT_TYPE_LOCATE_ACK;
            memcpy(a.board_nonce, m.board_nonce, PB_MGMT_NONCE_LEN);
            memcpy(a.endpoint_addr, eid->mFields.m8, PB_MGMT_ENDPOINT_ADDR_LEN);
            a.endpoint_port = kHubPort;
            n               = pb_mgmt_pack(&a, hub->mKey, pb_locate_glue_hmac, nullptr, wire, sizeof(wire));
        }
        else if (m.type == PB_MGMT_TYPE_POLL)
        {
            hub->mPolls++;
            a.type = PB_MGMT_TYPE_IDLE;
            memcpy(a.board_nonce, m.board_nonce, PB_MGMT_NONCE_LEN);
            memset(a.hub_nonce, ++hub->mNonceCounter, PB_MGMT_NONCE_LEN);
            n = pb_mgmt_pack(&a, hub->mKey, pb_locate_glue_hmac, nullptr, wire, sizeof(wire));
        }
    }
    if (n != 0)
    {
        SuccessOrQuit(otCoapMessageInitResponse(reply, aMessage, OT_COAP_TYPE_ACKNOWLEDGMENT, OT_COAP_CODE_CONTENT));
        SuccessOrQuit(otCoapMessageSetPayloadMarker(reply));
        SuccessOrQuit(otMessageAppend(reply, wire, static_cast<uint16_t>(n)));
    }
    else
    {
        /* Another board, hub, generation or key, or a message this hub does
         * not answer (HELLO is left out here): refused, as the real one does. */
        hub->mRefused++;
        SuccessOrQuit(
            otCoapMessageInitResponse(reply, aMessage, OT_COAP_TYPE_ACKNOWLEDGMENT, OT_COAP_CODE_UNAUTHORIZED));
    }
    SuccessOrQuit(otCoapSendResponse(hub->mInstance, reply, aMessageInfo));
}

static void HubStart(Hub &aHub, Node &aNode, uint8_t aKeyFirstByte)
{
    memset(&aHub, 0, sizeof(aHub));
    aHub.mInstance = &aNode.GetInstance();
    for (uint8_t i = 0; i < 16; i++)
    {
        aHub.mHubId[i]     = static_cast<uint8_t>(0x10 + i);
        aHub.mGeneration[i] = static_cast<uint8_t>(0x20 + i);
    }
    for (uint8_t i = 0; i < 32; i++)
    {
        aHub.mKey[i] = static_cast<uint8_t>(aKeyFirstByte + i);
    }
    SuccessOrQuit(otCoapStart(aHub.mInstance, kHubPort));
    aHub.mResource.mUriPath = kMgmtPath;
    aHub.mResource.mHandler = HubHandle;
    aHub.mResource.mContext = &aHub;
    otCoapAddResource(aHub.mInstance, &aHub.mResource);
}

/* The OTBR publishes an on-mesh prefix (what border routing does). */
static void PublishPrefix(Node &aNode)
{
    otBorderRouterConfig config;

    memset(&config, 0, sizeof(config));
    config.mPrefix.mPrefix.mFields.m8[0] = 0xfd;
    config.mPrefix.mPrefix.mFields.m8[1] = 0x7a;
    config.mPrefix.mPrefix.mFields.m8[2] = 0x9b;
    config.mPrefix.mLength               = 64;
    config.mPreferred                    = true;
    config.mSlaac                        = true;
    config.mDefaultRoute                 = true;
    config.mOnMesh                       = true;
    config.mStable                       = true;
    SuccessOrQuit(otBorderRouterAddOnMeshPrefix(&aNode.GetInstance(), &config));
    SuccessOrQuit(otBorderRouterRegister(&aNode.GetInstance()));
}

/* ---- the board: the real client, its platform operations over OpenThread ---- */

struct Board
{
    otInstance                 *mInstance;
    pb_mgmt_client_t            mClient;
    bool                        mAnswered;
    otError                     mResult;
    otCoapCode                  mCode;
    uint8_t                     mPayload[PB_MGMT_MAX_MESSAGE_LEN];
    size_t                      mPayloadLength;
    unsigned                    mRequests;
    unsigned                    mLocates;
    unsigned                    mMoved;
    pb_enroll_bridge_endpoint_t mMovedTo;
    uint8_t                     mRandom;
    uint8_t                     mLastHints[PB_MGMT_LOCATE_MAX_HINTS][PB_ENROLL_BRIDGE_ADDR_LEN];
    size_t                      mLastHintCount;
    unsigned                    mHintCalls;
};

static Board sBoard;

static void BoardAnswer(void *, otMessage *aMessage, const otMessageInfo *, otError aResult)
{
    sBoard.mAnswered      = true;
    sBoard.mResult        = aResult;
    sBoard.mPayloadLength = 0;
    if (aResult == OT_ERROR_NONE && aMessage != nullptr)
    {
        uint16_t offset = otMessageGetOffset(aMessage);
        uint16_t length = static_cast<uint16_t>(otMessageGetLength(aMessage) - offset);

        sBoard.mCode = otCoapMessageGetCode(aMessage);
        if (length <= sizeof(sBoard.mPayload))
        {
            sBoard.mPayloadLength = otMessageRead(aMessage, offset, sBoard.mPayload, length);
        }
    }
}

/* power_base/main/enrollment_mgmt.c's op_exchange(), with virtual time in
 * place of the semaphore: one Confirmable POST, no retransmission, 2 s ACK
 * timeout x 1.5 plus 2 s of margin. */
static pb_mgmt_xfer_t BoardExchange(const pb_enroll_bridge_endpoint_t *aEndpoint, const uint8_t *aRequest,
                                    size_t aRequestLength, uint8_t *aResponse, size_t aResponseCap,
                                    size_t *aResponseLength, void *)
{
    otMessage         *message = otCoapNewMessage(sBoard.mInstance, nullptr);
    otMessageInfo      info;
    otCoapTxParameters tx;

    *aResponseLength = 0;
    VerifyOrQuit(message != nullptr);
    sBoard.mRequests++;
    if (aRequest[1] == PB_MGMT_TYPE_LOCATE)
    {
        sBoard.mLocates++;
    }
    SuccessOrQuit(otCoapMessageInit(message, OT_COAP_TYPE_CONFIRMABLE, OT_COAP_CODE_POST));
    SuccessOrQuit(otCoapMessageAppendUriPathOptions(message, kMgmtPath));
    SuccessOrQuit(otCoapMessageSetPayloadMarker(message));
    SuccessOrQuit(otMessageAppend(message, aRequest, static_cast<uint16_t>(aRequestLength)));
    memset(&info, 0, sizeof(info));
    memcpy(info.mPeerAddr.mFields.m8, aEndpoint->addr, sizeof(info.mPeerAddr.mFields.m8));
    info.mPeerPort = aEndpoint->port;
    memset(&tx, 0, sizeof(tx));
    tx.mAckTimeout                 = 2000;
    tx.mAckRandomFactorNumerator   = 3;
    tx.mAckRandomFactorDenominator = 2;
    tx.mMaxRetransmit              = 0;
    sBoard.mAnswered = false;
    if (otCoapSendRequestWithParameters(sBoard.mInstance, message, &info, BoardAnswer, nullptr, &tx) != OT_ERROR_NONE)
    {
        otMessageFree(message);
        return PB_MGMT_XFER_NO_ANSWER;
    }
    for (uint32_t waited = 0; waited < 5000 && !sBoard.mAnswered; waited += 50)
    {
        sNexus->AdvanceTime(50);
    }
    if (!sBoard.mAnswered || sBoard.mResult != OT_ERROR_NONE)
    {
        return PB_MGMT_XFER_NO_ANSWER;
    }
    if ((sBoard.mCode >> 5) == 4)
    {
        return PB_MGMT_XFER_REJECTED;
    }
    if ((sBoard.mCode >> 5) != 2)
    {
        return PB_MGMT_XFER_NO_ANSWER;
    }
    if (sBoard.mPayloadLength <= aResponseCap)
    {
        memcpy(aResponse, sBoard.mPayload, sBoard.mPayloadLength);
        *aResponseLength = sBoard.mPayloadLength;
    }
    return PB_MGMT_XFER_ANSWERED;
}

static bool BoardRandom(uint8_t *aBuffer, size_t aLength, void *)
{
    for (size_t i = 0; i < aLength; i++)
    {
        aBuffer[i] = static_cast<uint8_t>(sBoard.mRandom + 3 * i + 1);
    }
    sBoard.mRandom = static_cast<uint8_t>(sBoard.mRandom + 0x1d);
    return true;
}

/* The firmware's own collection, on this instance. */
static size_t BoardHints(uint8_t (*aOut)[PB_ENROLL_BRIDGE_ADDR_LEN], size_t aCap, void *)
{
    size_t count = pb_locate_collect_hints(sBoard.mInstance, aOut, aCap);

    sBoard.mHintCalls++;
    sBoard.mLastHintCount = count < PB_MGMT_LOCATE_MAX_HINTS ? count : PB_MGMT_LOCATE_MAX_HINTS;
    memcpy(sBoard.mLastHints, aOut, sBoard.mLastHintCount * PB_ENROLL_BRIDGE_ADDR_LEN);
    return count;
}

static void BoardEndpointChanged(const pb_enroll_bridge_endpoint_t *aEndpoint, void *)
{
    sBoard.mMoved++;
    sBoard.mMovedTo = *aEndpoint;
}

static void      BoardQuiesce(void *) {}
static void      BoardDelay(uint32_t aMs, void *) { sNexus->AdvanceTime(aMs); }
static bool      BoardNetworkUp(void *) { return true; }
static esp_err_t BoardErase(void *) { return ESP_OK; }
static bool      BoardErased(void *) { return true; }

static bool HintsContain(const otIp6Address &aAddress)
{
    for (size_t i = 0; i < sBoard.mLastHintCount; i++)
    {
        if (memcmp(sBoard.mLastHints[i], aAddress.mFields.m8, PB_ENROLL_BRIDGE_ADDR_LEN) == 0)
        {
            return true;
        }
    }
    return false;
}

static bool EndpointIs(const pb_enroll_bridge_endpoint_t &aEndpoint, const otIp6Address &aAddress)
{
    return aEndpoint.port == kHubPort && memcmp(aEndpoint.addr, aAddress.mFields.m8, sizeof(aEndpoint.addr)) == 0;
}

static pb_enroll_bridge_endpoint_t StoredEndpoint(void)
{
    pb_enroll_record_t record;

    memset(&record, 0, sizeof(record));
    (void)pb_enroll_record_load(kEui, &record);
    return record.bridge_endpoint;
}

/* ---- one run ---- */

static void Run(bool aOtbrPublishesPrefix)
{
    Core         nexus;
    Node        &otbr      = nexus.CreateNode(); /* the hub's Linux OTBR: forms the network */
    Node        &neighbour = nexus.CreateNode(); /* some other router (and, for question 4, another hub) */
    Node        &base      = nexus.CreateNode(); /* the Power Base */
    Node        &otbr2     = nexus.CreateNode(); /* the device that replaces the OTBR later */
    static Hub   hub, foreignHub, hubAfter;
    otIp6Address otbrEid, otbrLocator, otbr2Eid, otbr2Locator, ownLocator;
    pb_mgmt_ops_t      ops;
    pb_enroll_record_t record;
    pb_mgmt_poll_result_t result;
    int                movedAtPoll = 0;

    printf("\n==== the OTBR %s ====\n",
           aOtbrPublishesPrefix ? "publishes an on-mesh prefix (border routing)" : "publishes nothing in the Network Data");
    sNexus = &nexus;
    memset(&sBoard, 0, sizeof(sBoard));
    sBoard.mInstance = &base.GetInstance();
    sBoard.mRandom   = 0x51;
    nvs_sim_reset();
    nexus.AdvanceTime(0);

    otbr.Form();
    nexus.AdvanceTime(13 * 1000);
    VerifyOrQuit(otbr.Get<Mle::Mle>().IsLeader());
    neighbour.Join(otbr);
    base.Join(otbr);
    nexus.AdvanceTime(300 * 1000);
    VerifyOrQuit(neighbour.Get<Mle::Mle>().IsRouter() && base.Get<Mle::Mle>().IsRouter());
    if (aOtbrPublishesPrefix)
    {
        PublishPrefix(otbr);
    }
    nexus.AdvanceTime(30 * 1000);

    HubStart(hub, otbr, 0x30);        /* this board's hub */
    HubStart(foreignHub, neighbour, 0xc0); /* a hub with another key, on another router */
    SuccessOrQuit(otCoapStart(sBoard.mInstance, OT_DEFAULT_COAP_PORT));
    otbrEid = *otThreadGetMeshLocalEid(&otbr.GetInstance());
    otbrLocator = *otThreadGetRloc(&otbr.GetInstance());

    /* The board was enrolled with this hub: record version 3, the hub's
     * endpoint as it is now. */
    memset(&record, 0, sizeof(record));
    memcpy(record.id.ext_pan_id, otThreadGetExtendedPanId(sBoard.mInstance)->m8, 8);
    memcpy(record.id.mesh_local_prefix, otThreadGetMeshLocalPrefix(sBoard.mInstance)->m8, 8);
    record.has_bridge_endpoint = true;
    memcpy(record.bridge_endpoint.addr, otbrEid.mFields.m8, 16);
    record.bridge_endpoint.port = kHubPort;
    record.has_mgmt             = true;
    record.mgmt_state           = PB_ENROLL_MGMT_ENROLLED;
    memcpy(record.mgmt.hub_id, hub.mHubId, 16);
    memcpy(record.mgmt.generation, hub.mGeneration, 16);
    memcpy(record.mgmt.key, hub.mKey, 32);
    VerifyOrQuit(pb_enroll_record_store(kEui, &record) == ESP_OK);

    memset(&ops, 0, sizeof(ops));
    ops.hmac                 = pb_locate_glue_hmac;
    ops.random               = BoardRandom;
    ops.exchange             = BoardExchange;
    ops.quiesce              = BoardQuiesce;
    ops.delay_ms             = BoardDelay;
    ops.network_up_for_proof = BoardNetworkUp;
    ops.network_erase        = BoardErase;
    ops.network_erased       = BoardErased;
    ops.locate_hints         = BoardHints;
    ops.endpoint_changed     = BoardEndpointChanged;
    pb_mgmt_client_init(&sBoard.mClient, &ops, kEui);
    VerifyOrQuit(pb_mgmt_client_load(&sBoard.mClient) == PB_MGMT_BOOT_ARMED);

    /* Steady state. */
    for (int i = 0; i < 12; i++)
    {
        result = pb_mgmt_client_poll(&sBoard.mClient);
        nexus.AdvanceTime(10 * 1000);
    }
    Check(result == PB_MGMT_POLL_IDLE && hub.mPolls >= 12 && sBoard.mLocates == 0 && sBoard.mHintCalls == 0,
          "two minutes of polls over real CoAP, each answered: the board never asks where its hub is");

    /* Question 1 and 2, before anything moves: what the firmware's hint
     * collection sees, and a LOCATE to the OTBR's routing locator. */
    {
        uint8_t                     hints[PB_MGMT_LOCATE_MAX_HINTS][PB_ENROLL_BRIDGE_ADDR_LEN];
        size_t                      count = BoardHints(hints, PB_MGMT_LOCATE_MAX_HINTS, nullptr);
        pb_enroll_bridge_endpoint_t toLocator;
        uint8_t                     wire[PB_MGMT_MAX_MESSAGE_LEN], answer[PB_MGMT_MAX_MESSAGE_LEN];
        size_t                      answerLength = 0;
        pb_mgmt_msg_t               m, a;
        bool                        allInPrefix = true;

        ownLocator = *otThreadGetRloc(sBoard.mInstance);
        for (size_t i = 0; i < count; i++)
        {
            allInPrefix = allInPrefix && memcmp(hints[i], record.id.mesh_local_prefix, 8) == 0;
        }
        printf("info: %u hint(s); the OTBR's locator is %s\n", static_cast<unsigned>(count),
               count > 0 && memcmp(hints[0], otbrLocator.mFields.m8, 16) == 0 ? "the first" : "not the first");
        Check(count >= 2 && HintsContain(otbrLocator) && HintsContain(*otThreadGetRloc(&neighbour.GetInstance())),
              "the firmware's hints, read from the board's own Network Data and router table, hold the OTBR's "
              "routing locator (and the other router's)");
        Check(!HintsContain(ownLocator) && allInPrefix && !HintsContain(otbrEid),
              "every hint is a locator in the Mesh-Local Prefix; the board itself is not among them");
        if (aOtbrPublishesPrefix)
        {
            Check(memcmp(hints[0], otbrLocator.mFields.m8, 16) == 0,
                  "a border router named in the Network Data comes first");
        }

        memset(&m, 0, sizeof(m));
        m.type = PB_MGMT_TYPE_LOCATE;
        memcpy(m.id.eui64, kEui, 8);
        memcpy(m.id.hub_id, hub.mHubId, 16);
        memcpy(m.id.generation, hub.mGeneration, 16);
        memset(m.board_nonce, 0x77, sizeof(m.board_nonce));
        size_t n = pb_mgmt_pack(&m, hub.mKey, pb_locate_glue_hmac, nullptr, wire, sizeof(wire));
        memcpy(toLocator.addr, otbrLocator.mFields.m8, 16);
        toLocator.port = kHubPort;
        unsigned atLocator = hub.mLocatesAtLocator;
        pb_mgmt_xfer_t x = BoardExchange(&toLocator, wire, n, answer, sizeof(answer), &answerLength, nullptr);
        Check(x == PB_MGMT_XFER_ANSWERED && hub.mLocatesAtLocator == atLocator + 1,
              "a LOCATE sent to the OTBR's routing locator arrives there, addressed to the locator, and its "
              "answer comes back to the board's CoAP client");
        Check(pb_mgmt_parse(answer, answerLength, &a) && a.type == PB_MGMT_TYPE_LOCATE_ACK &&
                  pb_mgmt_verify(answer, answerLength, hub.mKey, pb_locate_glue_hmac, nullptr) &&
                  memcmp(a.endpoint_addr, otbrEid.mFields.m8, 16) == 0 && a.endpoint_port == kHubPort,
              "that answer verifies with the enrollment's key and names the OTBR's Mesh-Local EID, not the locator");
        sBoard.mLocates   = 0;
        sBoard.mHintCalls = 0;
    }

    /* The OTBR is replaced: the old device is gone for good, a new one comes
     * up with the same Dataset (and the hub's state: the same key). Nothing
     * of its addresses is the old one's. */
    printf("-- the OTBR device is replaced (same Dataset, same hub identity and key) --\n");
    SuccessOrQuit(otThreadSetEnabled(&otbr.GetInstance(), false));
    SuccessOrQuit(otIp6SetEnabled(&otbr.GetInstance(), false));
    nexus.AdvanceTime(300 * 1000);
    Check(neighbour.Get<Mle::Mle>().IsLeader() || base.Get<Mle::Mle>().IsLeader(),
          "the network goes on without the old OTBR (another router is the leader now)");
    otbr2.Join(neighbour);
    nexus.AdvanceTime(300 * 1000);
    VerifyOrQuit(otbr2.Get<Mle::Mle>().IsAttached());
    if (aOtbrPublishesPrefix)
    {
        PublishPrefix(otbr2);
    }
    HubStart(hubAfter, otbr2, 0x30);
    nexus.AdvanceTime(30 * 1000);
    otbr2Eid     = *otThreadGetMeshLocalEid(&otbr2.GetInstance());
    otbr2Locator = *otThreadGetRloc(&otbr2.GetInstance());
    Check(memcmp(&otbr2Eid, &otbrEid, sizeof(otbrEid)) != 0 && memcmp(&otbr2Locator, &otbrLocator, sizeof(otbrEid)) != 0 &&
              memcmp(otbr2Eid.mFields.m8, otbrEid.mFields.m8, 8) == 0,
          "the new OTBR has another Mesh-Local EID and another locator, in the same Mesh-Local Prefix");
    printf("info: the new OTBR is a %s\n", otbr2.Get<Mle::Mle>().IsRouter() ? "router" : "child");

    /* The board, by itself. (The old OTBR was gone for ten minutes of
     * virtual time during which this test did not poll; counted from here.) */
    for (int poll = 1; poll <= 40 && movedAtPoll == 0; poll++)
    {
        result = pb_mgmt_client_poll(&sBoard.mClient);
        if (result == PB_MGMT_POLL_ENDPOINT_MOVED)
        {
            movedAtPoll = poll;
        }
        else
        {
            nexus.AdvanceTime(10 * 1000);
        }
    }
    printf("info: moved at poll %d, after %u LOCATE request(s); last round had %u hint(s)\n", movedAtPoll, sBoard.mLocates,
           static_cast<unsigned>(sBoard.mLastHintCount));
    Check(movedAtPoll == static_cast<int>(PB_MGMT_LOCATE_AFTER_SILENT_POLLS),
          "six polls to the old endpoint go unanswered; the round that follows finds the hub");
    Check(HintsContain(otbr2Locator), "that round's hints, from the real Network Data and router table, hold the new "
                                      "OTBR's locator");
    Check(sBoard.mMoved == 1 && EndpointIs(sBoard.mMovedTo, otbr2Eid) && EndpointIs(StoredEndpoint(), otbr2Eid),
          "the board stored the new OTBR's Mesh-Local EID -- confirmed by the hub from that EID -- and moved there");
    Check(hubAfter.mLocatesAtLocator >= 1 && hubAfter.mLocatesAtEid >= 1,
          "the hub was asked at its locator (the hint) and once more at the EID it named");
    if (aOtbrPublishesPrefix)
    {
        /* The Network Data names the new OTBR, so it is the first hint. */
        Check(sBoard.mLocates == 3,
              "three requests in all: the stored address (gone), the border router the Network Data names, and the "
              "confirmation at its EID");
    }
    else
    {
        /* No Network Data entry: the leader and the routers, in router table
         * order -- which of the two remaining routers became the leader, and
         * so whether the other hub is asked before the right one, is up to
         * the network. */
        Check(sBoard.mLocates >= 3 && sBoard.mLocates <= PB_MGMT_LOCATE_MAX_PROBES,
              "found through the router table alone, within one round's limit");
        printf("info: the other router's hub was asked %u time(s) on the way\n", foreignHub.mRefused);
    }
    {
        /* Question 4, by itself: the other router's hub answers a LOCATE that
         * carries this board's key with a refusal, and nothing moves. */
        pb_enroll_bridge_endpoint_t toNeighbour;
        uint8_t                     wire[PB_MGMT_MAX_MESSAGE_LEN], answer[PB_MGMT_MAX_MESSAGE_LEN];
        size_t                      answerLength = 0;
        pb_mgmt_msg_t               m;
        unsigned                    refused = foreignHub.mRefused;

        memset(&m, 0, sizeof(m));
        m.type = PB_MGMT_TYPE_LOCATE;
        memcpy(m.id.eui64, kEui, 8);
        memcpy(m.id.hub_id, hub.mHubId, 16);
        memcpy(m.id.generation, hub.mGeneration, 16);
        memset(m.board_nonce, 0x78, sizeof(m.board_nonce));
        size_t n = pb_mgmt_pack(&m, hub.mKey, pb_locate_glue_hmac, nullptr, wire, sizeof(wire));
        memcpy(toNeighbour.addr, otThreadGetRloc(&neighbour.GetInstance())->mFields.m8, 16);
        toNeighbour.port = kHubPort;
        Check(BoardExchange(&toNeighbour, wire, n, answer, sizeof(answer), &answerLength, nullptr) == PB_MGMT_XFER_REJECTED &&
                  foreignHub.mRefused == refused + 1 && sBoard.mMoved == 1,
              "a hub with another key, on another router of the same network, refuses this board's LOCATE");
        sBoard.mLocates--;
    }
    unsigned polls = hubAfter.mPolls;
    result         = pb_mgmt_client_poll(&sBoard.mClient);
    Check(result == PB_MGMT_POLL_IDLE && hubAfter.mPolls == polls + 1,
          "the next poll goes to the new endpoint and is answered");
    unsigned locates = sBoard.mLocates;
    for (int i = 0; i < 12; i++)
    {
        nexus.AdvanceTime(10 * 1000);
        result = pb_mgmt_client_poll(&sBoard.mClient);
    }
    Check(result == PB_MGMT_POLL_IDLE && sBoard.mLocates == locates && sBoard.mMoved == 1,
          "two more minutes: answered polls, nothing is looked for again");
    sNexus = nullptr;
}

/* ---- question 5: more routers than one round asks ---- */

static void RunManyRouters(void)
{
    enum { kOthers = 12 }; /* routers besides the OTBR and the board */
    Core               nexus;
    Node              &otbr = nexus.CreateNode();
    Node              *others[kOthers];
    static Hub         hub, hubAfter;
    pb_mgmt_ops_t      ops;
    pb_enroll_record_t record;
    pb_mgmt_poll_result_t result = PB_MGMT_POLL_NOT_ARMED;
    otIp6Address       otbrEid;

    for (int i = 0; i < kOthers; i++)
    {
        others[i] = &nexus.CreateNode();
    }
    Node &base = nexus.CreateNode();

    printf("\n==== %d routers besides the board: more than one round asks ====\n", kOthers + 1);
    sNexus = &nexus;
    memset(&sBoard, 0, sizeof(sBoard));
    sBoard.mInstance = &base.GetInstance();
    sBoard.mRandom   = 0x51;
    nvs_sim_reset();
    nexus.AdvanceTime(0);

    otbr.Form();
    nexus.AdvanceTime(13 * 1000);
    VerifyOrQuit(otbr.Get<Mle::Mle>().IsLeader());
    for (int i = 0; i < kOthers; i++)
    {
        others[i]->Join(otbr);
    }
    base.Join(otbr);
    nexus.AdvanceTime(900 * 1000);
    {
        int routers = 0;

        for (int i = 0; i < kOthers; i++)
        {
            routers += others[i]->Get<Mle::Mle>().IsRouter() ? 1 : 0;
        }
        VerifyOrQuit(routers == kOthers && base.Get<Mle::Mle>().IsAttached());
    }

    HubStart(hub, otbr, 0x30);
    SuccessOrQuit(otCoapStart(sBoard.mInstance, OT_DEFAULT_COAP_PORT));
    otbrEid = *otThreadGetMeshLocalEid(&otbr.GetInstance());
    memset(&record, 0, sizeof(record));
    memcpy(record.id.ext_pan_id, otThreadGetExtendedPanId(sBoard.mInstance)->m8, 8);
    memcpy(record.id.mesh_local_prefix, otThreadGetMeshLocalPrefix(sBoard.mInstance)->m8, 8);
    record.has_bridge_endpoint = true;
    memcpy(record.bridge_endpoint.addr, otbrEid.mFields.m8, 16);
    record.bridge_endpoint.port = kHubPort;
    record.has_mgmt             = true;
    record.mgmt_state           = PB_ENROLL_MGMT_ENROLLED;
    memcpy(record.mgmt.hub_id, hub.mHubId, 16);
    memcpy(record.mgmt.generation, hub.mGeneration, 16);
    memcpy(record.mgmt.key, hub.mKey, 32);
    VerifyOrQuit(pb_enroll_record_store(kEui, &record) == ESP_OK);

    memset(&ops, 0, sizeof(ops));
    ops.hmac                 = pb_locate_glue_hmac;
    ops.random               = BoardRandom;
    ops.exchange             = BoardExchange;
    ops.quiesce              = BoardQuiesce;
    ops.delay_ms             = BoardDelay;
    ops.network_up_for_proof = BoardNetworkUp;
    ops.network_erase        = BoardErase;
    ops.network_erased       = BoardErased;
    ops.locate_hints         = BoardHints;
    ops.endpoint_changed     = BoardEndpointChanged;
    pb_mgmt_client_init(&sBoard.mClient, &ops, kEui);
    VerifyOrQuit(pb_mgmt_client_load(&sBoard.mClient) == PB_MGMT_BOOT_ARMED);
    for (int i = 0; i < 6; i++)
    {
        result = pb_mgmt_client_poll(&sBoard.mClient);
        nexus.AdvanceTime(10 * 1000);
    }
    Check(result == PB_MGMT_POLL_IDLE && sBoard.mLocates == 0, "a minute of answered polls in the large network");

    /* What the firmware's collection gives here, and which device its LAST
     * entry is. */
    static uint8_t hints[PB_MGMT_LOCATE_MAX_HINTS][PB_ENROLL_BRIDGE_ADDR_LEN];
    size_t         count = BoardHints(hints, PB_MGMT_LOCATE_MAX_HINTS, nullptr);
    Node          *last  = nullptr;
    bool           all   = HintsContain(*otThreadGetRloc(&otbr.GetInstance()));

    for (int i = 0; i < kOthers; i++)
    {
        const otIp6Address *locator = otThreadGetRloc(&others[i]->GetInstance());

        all = all && HintsContain(*locator);
        if (count > 0 && memcmp(hints[count - 1], locator->mFields.m8, PB_ENROLL_BRIDGE_ADDR_LEN) == 0)
        {
            last = others[i];
        }
    }
    printf("info: the firmware's collection gave %u hint(s)\n", static_cast<unsigned>(count));
    Check(count == kOthers + 1 && all && !HintsContain(*otThreadGetRloc(sBoard.mInstance)),
          "the firmware's hints hold the locator of every other router of the network -- 13 of them, not the first 8");
    Check(last != nullptr && count - 1 >= PB_MGMT_LOCATE_MAX_PROBES - 1,
          "the last of them is one of the plain routers, further down the list than one round gets");
    if (last == nullptr)
    {
        sNexus = nullptr;
        return;
    }

    /* The hub is on that device from now on (same identity and key); the old
     * device stays in the network as the router it is, so the list the
     * board collects does not change. */
    printf("-- the hub moves to the device behind the last hint --\n");
    otCoapRemoveResource(&otbr.GetInstance(), &hub.mResource);
    HubStart(hubAfter, *last, 0x30);
    nexus.AdvanceTime(5 * 1000);
    sBoard.mLocates   = 0;
    sBoard.mHintCalls = 0;

    int      movedAtPoll = 0, round = 0, movedInRound = 0;
    unsigned inRound[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    for (int poll = 1; poll <= 60 && movedAtPoll == 0; poll++)
    {
        unsigned locates = sBoard.mLocates, calls = sBoard.mHintCalls;

        result = pb_mgmt_client_poll(&sBoard.mClient);
        if (sBoard.mHintCalls != calls && round < 8)
        {
            inRound[round++] = sBoard.mLocates - locates;
        }
        if (result == PB_MGMT_POLL_ENDPOINT_MOVED)
        {
            movedAtPoll  = poll;
            movedInRound = round;
        }
        else
        {
            nexus.AdvanceTime(10 * 1000);
        }
    }
    printf("info: moved at poll %d, in round %d; requests per round: %u, %u, %u\n", movedAtPoll, movedInRound, inRound[0],
           inRound[1], inRound[2]);
    Check(inRound[0] == PB_MGMT_LOCATE_MAX_PROBES && movedInRound != 1,
          "the first round sends its 12 requests -- the stored address and the first 11 routers -- and does not get "
          "to the hub");
    Check(movedInRound == 2 && inRound[1] <= PB_MGMT_LOCATE_MAX_PROBES,
          "the second round goes on behind them and finds it");
    Check(sBoard.mMoved == 1 && EndpointIs(sBoard.mMovedTo, *otThreadGetMeshLocalEid(&last->GetInstance())) &&
              EndpointIs(StoredEndpoint(), *otThreadGetMeshLocalEid(&last->GetInstance())),
          "the board stored that device's Mesh-Local EID, confirmed from there, and moved");
    Check(hubAfter.mLocatesAtLocator == 1 && hubAfter.mLocatesAtEid == 1,
          "the hub was asked once at its locator and once at the EID it named");
    unsigned polls = hubAfter.mPolls;
    result         = pb_mgmt_client_poll(&sBoard.mClient);
    Check(result == PB_MGMT_POLL_IDLE && hubAfter.mPolls == polls + 1, "the next poll is answered at the new endpoint");
    sNexus = nullptr;
}

} // namespace Nexus
} // namespace ot

int main(void)
{
    ot::Nexus::Run(true);
    ot::Nexus::Run(false);
    ot::Nexus::RunManyRouters();
    if (ot::Nexus::sFailures != 0)
    {
        printf("\n%d check(s) FAILED\n", ot::Nexus::sFailures);
        return 1;
    }
    printf("\nPASS: 0 failure(s)\n");
    return 0;
}
