#ifndef BROKER_REMOTE_DSLINK_H
#define BROKER_REMOTE_DSLINK_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <jansson.h>
#include <mbedtls/ecdh.h>
#include <wslay/wslay.h>

#include <dslink/col/map.h>
#include <dslink/col/listener.h>
#include <dslink/socket.h>

typedef struct RemoteAuth {

    char salt[48];
    mbedtls_ecdh_context tempKey;
    const char *pubKey;

} RemoteAuth;

typedef struct RemoteDSLink {

    uint8_t isRequester;
    uint8_t isResponder;
    uint8_t pendingClose;

    wslay_event_context_ptr ws;
    Socket *socket;

    struct Broker *broker;
    struct DownstreamNode *node;
    RemoteAuth *auth;

    const char *dsId;
    const char *path;
    const char *name;

    json_t *linkData;

    // Map<uint32_t *, Stream *>

    // connect to requester
    // broker receive requests and send back responses
    Map requester_streams;
    // connect to responder
    // broker send requests and receive responses
    Map responder_streams;

    // Map<uint32_t *, BrokerSubStream *>
    Map sub_sids;

    // Map<char *, BrokerSubStream *>
    Map sub_paths;

} RemoteDSLink;

int broker_remote_dslink_init(RemoteDSLink *link);
void broker_remote_dslink_free(RemoteDSLink *link);

#ifdef __cplusplus
}
#endif

#endif // BROKER_REMOTE_DSLINK_H
