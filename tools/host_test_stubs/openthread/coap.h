/*
 * Host-only stand-in for OpenThread's openthread/coap.h -- only the subset
 * components/sensor_transport/sensor_transport.c actually uses (client-side
 * Confirmable POST + response handler, plus the small server-side surface
 * needed for that same file's coap_sample_handler() to still compile,
 * though no host test here ever exercises that handler). Real bodies for
 * every function declared here live in
 * tools/host_test_stubs/host_ot_coap_sim.c -- a stand-in for the
 * OpenThread/Thread-radio CoAP engine dependency itself (same category as
 * host_thread_manager_sim.c stands in for thread_manager.c's own OpenThread
 * calls), never a reimplementation of sensor_transport.c's own logic, which
 * links and runs completely unmodified against this.
 *
 * otCoapCode's numeric layout ((class << 5) | detail) and every
 * OT_COAP_CODE_* value used by this codebase match real OpenThread exactly
 * (openthread/include/openthread/coap.h) -- tools/test_coap_response_semantics.c
 * already relies on this same layout in its own mirror.
 */
#pragma once

#include <stdint.h>
#include <stddef.h>
#include "openthread/error.h"
#include "openthread/thread.h" /* otIp6Address */

#ifdef __cplusplus
extern "C" {
#endif

typedef struct otInstance otInstance;
typedef struct otMessage otMessage;

typedef enum {
    OT_COAP_TYPE_CONFIRMABLE = 0,
    OT_COAP_TYPE_NON_CONFIRMABLE = 1,
    OT_COAP_TYPE_ACKNOWLEDGMENT = 2,
    OT_COAP_TYPE_RESET = 3,
} otCoapType;

typedef uint8_t otCoapCode;

#define OT_COAP_CODE(c, d) ((otCoapCode)(((c) << 5) | (d)))
#define OT_COAP_CODE_POST OT_COAP_CODE(0, 2)
#define OT_COAP_CODE_CHANGED OT_COAP_CODE(2, 4)
#define OT_COAP_CODE_VALID OT_COAP_CODE(2, 3)
#define OT_COAP_CODE_BAD_REQUEST OT_COAP_CODE(4, 0)
#define OT_COAP_CODE_NOT_FOUND OT_COAP_CODE(4, 4)
#define OT_COAP_CODE_PRECONDITION_FAILED OT_COAP_CODE(4, 12)
#define OT_COAP_CODE_SERVICE_UNAVAILABLE OT_COAP_CODE(5, 3)

#define OT_DEFAULT_COAP_PORT 5683

typedef struct {
    otIp6Address mPeerAddr;
    otIp6Address mSockAddr;
    uint16_t mPeerPort;
    uint16_t mSockPort;
} otMessageInfo;

typedef struct {
    uint32_t mAckTimeout;
    uint8_t mAckRandomFactorNumerator;
    uint8_t mAckRandomFactorDenominator;
    uint8_t mMaxRetransmit;
} otCoapTxParameters;

typedef void (*otCoapResponseHandler)(void *context, otMessage *message, const otMessageInfo *messageInfo,
                                       otError result);
typedef void (*otCoapRequestHandler)(void *context, otMessage *message, const otMessageInfo *messageInfo);

typedef struct otCoapResource {
    const char *mUriPath;
    otCoapRequestHandler mHandler;
    void *mContext;
    struct otCoapResource *mNext;
} otCoapResource;

otError otCoapStart(otInstance *instance, uint16_t port);
void otCoapAddResource(otInstance *instance, otCoapResource *resource);

otMessage *otCoapNewMessage(otInstance *instance, const void *settings);
void otMessageFree(otMessage *message);
void otCoapMessageInit(otMessage *message, otCoapType type, otCoapCode code);
otError otCoapMessageAppendUriPathOptions(otMessage *message, const char *uriPath);
otError otCoapMessageSetPayloadMarker(otMessage *message);
otError otMessageAppend(otMessage *message, const void *data, uint16_t length);
otCoapCode otCoapMessageGetCode(const otMessage *message);
otCoapType otCoapMessageGetType(const otMessage *message);
uint16_t otMessageGetLength(const otMessage *message);
uint16_t otMessageGetOffset(const otMessage *message);
uint16_t otMessageRead(const otMessage *message, uint16_t offset, void *buf, uint16_t length);
otError otCoapMessageInitResponse(otMessage *response, const otMessage *request, otCoapType type, otCoapCode code);
otError otCoapSendResponse(otInstance *instance, otMessage *message, const otMessageInfo *messageInfo);

otError otCoapSendRequestWithParameters(otInstance *instance, otMessage *message, const otMessageInfo *messageInfo,
                                         otCoapResponseHandler handler, void *context,
                                         const otCoapTxParameters *txParameters);

#ifdef __cplusplus
}
#endif
