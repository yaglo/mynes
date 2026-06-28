/*
 * Debug Server — Implementation
 * ==============================
 *
 * Listens on a Unix domain socket for visualiser connections.
 * Background thread handles accept; main thread handles I/O (no races).
 */

#include "debug_server.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <pthread.h>
#include <errno.h>
#include <fcntl.h>

/* ============================================================================
 * Message format (matching SwiftUI client)
 * ============================================================================ */

#define DEBUG_MSG_SNAPSHOT      0
#define DEBUG_MSG_PARAM_UPDATE  1
#define DEBUG_MSG_PRESET_CHANGE 2
#define DEBUG_MSG_TAP_REQUEST   3
#define DEBUG_MSG_TAP_DATA      4

/* Message frame header */
typedef struct {
    uint32_t msg_type;
    uint32_t payload_size;
} DebugMessageHeader;

/* Snapshot message (server → client) */
typedef struct {
    uint32_t frame_number;
    uint32_t timestamp_ms;
    uint16_t num_video_stages;
    uint16_t num_audio_stages;
} DebugSnapshotHeader;

typedef struct {
    uint8_t  enabled;
    uint8_t  bypassed;
    uint8_t  kernel_type;    /* ChainKernelType ordinal (0-8) */
    uint8_t  pad;
    uint32_t timing_us;
    uint32_t timing_avg_us;
    char     name[32];       /* null-terminated stage name */
} DebugStageInfo;

/* Parameter update message (client → server) */
typedef struct {
    uint16_t stage_index;
    uint32_t params_size;
    uint8_t  params[128];  /* max CHAIN_MAX_UNIFORM_SIZE */
} DebugParamUpdate;

/* Preset change message (client → server) */
typedef struct {
    uint8_t preset_index;
} DebugPresetChange;

/* Tap request message (client → server) */
typedef struct {
    uint32_t stage_index;
} DebugTapRequest;

/* Tap data message (server → client) */
typedef struct {
    uint32_t stage_index;
    uint32_t sample_count;
    uint32_t samples_per_line;
    uint32_t lines;
    /* Followed by sample_count float32 values */
} DebugTapHeader;

/* ============================================================================
 * Server state
 * ============================================================================ */

typedef struct {
    int listen_sock;        /* listening socket (background thread) */
    int client_sock;        /* connected client, or -1 */
    pthread_t listen_thread;
    pthread_mutex_t client_lock;  /* protects client_sock */

    char socket_path[256];

    /* Pending tap request (from client) */
    int tap_stage_pending;  /* -1 = none, >= 0 = stage index */
} DebugServerState;

static DebugServer *g_server = NULL;

/* ============================================================================
 * Socket utilities
 * ============================================================================ */

static ssize_t send_all(int sock, const void *data, size_t size) {
    const uint8_t *buf = (const uint8_t *)data;
    size_t sent = 0;
    while (sent < size) {
        ssize_t n = send(sock, buf + sent, size - sent, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1;  /* connection closed */
        sent += (size_t)n;
    }
    return (ssize_t)sent;
}

static ssize_t recv_all(int sock, void *data, size_t size) {
    uint8_t *buf = (uint8_t *)data;
    size_t recvd = 0;
    while (recvd < size) {
        ssize_t n = recv(sock, buf + recvd, size - recvd, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return 0;  /* connection closed */
        recvd += (size_t)n;
    }
    return (ssize_t)recvd;
}

/* ============================================================================
 * Background listener thread
 * ============================================================================ */

static void *listen_thread_main(void *arg) {
    DebugServerState *state = (DebugServerState *)arg;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, state->socket_path, sizeof(addr.sun_path) - 1);

    /* Remove old socket file if it exists */
    unlink(state->socket_path);

    /* Create and bind listening socket */
    state->listen_sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (state->listen_sock < 0) {
        perror("socket(AF_UNIX)");
        return NULL;
    }

    if (bind(state->listen_sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(state->listen_sock);
        state->listen_sock = -1;
        return NULL;
    }

    if (listen(state->listen_sock, 1) < 0) {
        perror("listen");
        close(state->listen_sock);
        state->listen_sock = -1;
        return NULL;
    }

    printf("Debug server listening on %s\n", state->socket_path);

    /* Accept loop: one client at a time */
    while (state->listen_sock >= 0) {
        int client = accept(state->listen_sock, NULL, NULL);
        if (client < 0) {
            if (errno == EINTR || errno == EBADF) break;
            perror("accept");
            continue;
        }

        printf("Visualiser connected\n");

        pthread_mutex_lock(&state->client_lock);
        if (state->client_sock >= 0) {
            close(state->client_sock);
        }
        state->client_sock = client;
        pthread_mutex_unlock(&state->client_lock);
    }

    if (state->listen_sock >= 0) {
        close(state->listen_sock);
        state->listen_sock = -1;
    }

    return NULL;
}

/* ============================================================================
 * Message I/O (main thread only)
 * ============================================================================ */

static bool send_message(DebugServerState *state, uint32_t msg_type,
                         const void *payload, uint32_t payload_size) {
    int sock;
    pthread_mutex_lock(&state->client_lock);
    sock = state->client_sock;
    pthread_mutex_unlock(&state->client_lock);

    if (sock < 0) return false;  /* no client */

    DebugMessageHeader hdr;
    hdr.msg_type = msg_type;
    hdr.payload_size = payload_size;

    if (send_all(sock, &hdr, sizeof(hdr)) < 0) {
        printf("Debug server: header send failed, closing client\n");
        pthread_mutex_lock(&state->client_lock);
        close(state->client_sock);
        state->client_sock = -1;
        pthread_mutex_unlock(&state->client_lock);
        return false;
    }

    if (payload_size > 0) {
        if (send_all(sock, payload, payload_size) < 0) {
            printf("Debug server: payload send failed, closing client\n");
            pthread_mutex_lock(&state->client_lock);
            close(state->client_sock);
            state->client_sock = -1;
            pthread_mutex_unlock(&state->client_lock);
            return false;
        }
    }

    return true;
}

static bool read_message(DebugServerState *state, uint32_t *out_type,
                         void *out_payload, uint32_t max_payload_size) {
    int sock;
    pthread_mutex_lock(&state->client_lock);
    sock = state->client_sock;
    pthread_mutex_unlock(&state->client_lock);

    if (sock < 0) return false;  /* no client */

    /* Set non-blocking mode temporarily to check for messages */
    int flags = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, flags | O_NONBLOCK);

    DebugMessageHeader hdr;
    ssize_t n = recv_all(sock, &hdr, sizeof(hdr));

    fcntl(sock, F_SETFL, flags);  /* restore */

    if (n <= 0) {
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return false;  /* no message available */
        }
        if (n == 0) {
            printf("Debug server: client disconnected\n");
            pthread_mutex_lock(&state->client_lock);
            close(state->client_sock);
            state->client_sock = -1;
            pthread_mutex_unlock(&state->client_lock);
        }
        return false;
    }

    if (hdr.payload_size > max_payload_size) {
        printf("Debug server: message too large (%u > %u)\n",
               hdr.payload_size, max_payload_size);
        return false;
    }

    if (hdr.payload_size > 0) {
        if (recv_all(sock, out_payload, hdr.payload_size) < 0) {
            printf("Debug server: payload recv failed\n");
            pthread_mutex_lock(&state->client_lock);
            close(state->client_sock);
            state->client_sock = -1;
            pthread_mutex_unlock(&state->client_lock);
            return false;
        }
    }

    *out_type = hdr.msg_type;
    return true;
}

/* ============================================================================
 * Public API
 * ============================================================================ */

DebugServer *debug_server_create(const char *socket_path) {
    DebugServerState *state = (DebugServerState *)calloc(1, sizeof(*state));
    if (!state) return NULL;

    strncpy(state->socket_path, socket_path, sizeof(state->socket_path) - 1);
    state->client_sock = -1;
    state->listen_sock = -1;
    state->tap_stage_pending = -1;

    pthread_mutex_init(&state->client_lock, NULL);

    if (pthread_create(&state->listen_thread, NULL, listen_thread_main, state) < 0) {
        perror("pthread_create");
        free(state);
        return NULL;
    }

    g_server = (DebugServer *)state;
    return (DebugServer *)state;
}

void debug_server_destroy(DebugServer *srv) {
    if (!srv) return;

    DebugServerState *state = (DebugServerState *)srv;

    if (state->listen_sock >= 0) {
        close(state->listen_sock);
        state->listen_sock = -1;
    }

    pthread_mutex_lock(&state->client_lock);
    if (state->client_sock >= 0) {
        close(state->client_sock);
        state->client_sock = -1;
    }
    pthread_mutex_unlock(&state->client_lock);

    pthread_join(state->listen_thread, NULL);
    pthread_mutex_destroy(&state->client_lock);

    free(state);

    if (g_server == srv) {
        g_server = NULL;
    }
}

bool debug_server_has_client(const DebugServer *srv) {
    if (!srv) return false;

    DebugServerState *state = (DebugServerState *)srv;
    pthread_mutex_lock(&state->client_lock);
    bool has = (state->client_sock >= 0);
    pthread_mutex_unlock(&state->client_lock);

    return has;
}

void debug_server_frame(DebugServer *srv,
                        const SignalChain *video_chain,
                        const SignalChain *audio_chain,
                        uint32_t frame_number) {
    if (!srv || !debug_server_has_client(srv)) return;

    DebugServerState *state = (DebugServerState *)srv;

    /* Process incoming messages */
    uint32_t msg_type;
    uint8_t msg_payload[512];

    while (read_message(state, &msg_type, msg_payload, sizeof(msg_payload))) {
        switch (msg_type) {
            case DEBUG_MSG_TAP_REQUEST: {
                if (sizeof(DebugTapRequest) <= sizeof(msg_payload)) {
                    DebugTapRequest *req = (DebugTapRequest *)msg_payload;
                    state->tap_stage_pending = (int)req->stage_index;
                }
                break;
            }

            case DEBUG_MSG_PARAM_UPDATE: {
                /* Parameter updates would be handled here.
                 * For now, just acknowledge. */
                break;
            }

            case DEBUG_MSG_PRESET_CHANGE: {
                /* Preset changes would be handled here. */
                break;
            }

            default:
                break;
        }
    }

    /* Build and send snapshot */
    uint32_t num_video_stages = video_chain ? video_chain->num_stages : 0;
    uint32_t num_audio_stages = audio_chain ? audio_chain->num_stages : 0;

    /* Snapshot header + per-stage info */
    DebugSnapshotHeader snap_hdr;
    snap_hdr.frame_number = frame_number;
    snap_hdr.timestamp_ms = 0;  /* would use SDL_GetTicks() */
    snap_hdr.num_video_stages = (uint16_t)num_video_stages;
    snap_hdr.num_audio_stages = (uint16_t)num_audio_stages;

    size_t payload_size = sizeof(snap_hdr) +
                          (num_video_stages + num_audio_stages) * sizeof(DebugStageInfo);

    uint8_t *payload = (uint8_t *)malloc(payload_size);
    if (!payload) return;

    memcpy(payload, &snap_hdr, sizeof(snap_hdr));

    DebugStageInfo *stage_infos = (DebugStageInfo *)(payload + sizeof(snap_hdr));

    /* Fill in video stages */
    for (uint32_t i = 0; i < num_video_stages; i++) {
        stage_infos[i].enabled = video_chain->stages[i].enabled ? 1 : 0;
        stage_infos[i].bypassed = video_chain->stages[i].bypass ? 1 : 0;
        stage_infos[i].kernel_type = (uint8_t)video_chain->stages[i].kernel_type;
        stage_infos[i].pad = 0;
        stage_infos[i].timing_us = (uint32_t)video_chain->stages[i].timing_us;
        stage_infos[i].timing_avg_us = (uint32_t)video_chain->stages[i].timing_avg_us;
        strncpy(stage_infos[i].name,
                video_chain->stages[i].name ? video_chain->stages[i].name : "?", 31);
        stage_infos[i].name[31] = '\0';
    }

    /* Fill in audio stages */
    for (uint32_t i = 0; i < num_audio_stages; i++) {
        uint32_t idx = num_video_stages + i;
        stage_infos[idx].enabled = audio_chain->stages[i].enabled ? 1 : 0;
        stage_infos[idx].bypassed = audio_chain->stages[i].bypass ? 1 : 0;
        stage_infos[idx].kernel_type = (uint8_t)audio_chain->stages[i].kernel_type;
        stage_infos[idx].pad = 0;
        stage_infos[idx].timing_us = (uint32_t)audio_chain->stages[i].timing_us;
        stage_infos[idx].timing_avg_us = (uint32_t)audio_chain->stages[i].timing_avg_us;
        strncpy(stage_infos[idx].name,
                audio_chain->stages[i].name ? audio_chain->stages[i].name : "?", 31);
        stage_infos[idx].name[31] = '\0';
    }

    send_message(state, DEBUG_MSG_SNAPSHOT, payload, (uint32_t)payload_size);
    free(payload);
}

void debug_server_send_tap(DebugServer *srv,
                           int stage_index,
                           const float *data,
                           int sample_count,
                           int samples_per_line,
                           int lines) {
    if (!srv || !debug_server_has_client(srv) || !data) return;

    DebugServerState *state = (DebugServerState *)srv;

    /* Build tap message: header + float32 data */
    DebugTapHeader tap_hdr;
    tap_hdr.stage_index = (uint32_t)stage_index;
    tap_hdr.sample_count = (uint32_t)sample_count;
    tap_hdr.samples_per_line = (uint32_t)samples_per_line;
    tap_hdr.lines = (uint32_t)lines;

    size_t payload_size = sizeof(tap_hdr) + (size_t)sample_count * sizeof(float);
    uint8_t *payload = (uint8_t *)malloc(payload_size);
    if (!payload) return;

    memcpy(payload, &tap_hdr, sizeof(tap_hdr));
    memcpy(payload + sizeof(tap_hdr), data, (size_t)sample_count * sizeof(float));

    send_message(state, DEBUG_MSG_TAP_DATA, payload, (uint32_t)payload_size);
    free(payload);
}
