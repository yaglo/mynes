/*
 * Debug Server — Implementation
 * ==============================
 *
 * Listens on a Unix domain socket for visualiser connections.
 * Background thread handles accept; main thread handles I/O (no races).
 */

#include "debug_server.h"
#include "preset_apply.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#ifdef MSG_NOSIGNAL
#define DEBUG_SERVER_MSG_NOSIGNAL MSG_NOSIGNAL
#else
#define DEBUG_SERVER_MSG_NOSIGNAL 0
#endif
#include <sys/un.h>
#include <pthread.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>

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
    pthread_mutex_t client_lock;  /* protects client socket and output queue */
    uint8_t *output;
    size_t output_capacity;
    size_t output_start;
    size_t output_end;

    char socket_path[256];

    /* Pending tap request (from client) */
    DebugControl controls[DEBUG_MAX_CONTROLS];
    int control_count;
    uint64_t last_snapshot_ms;
    uint64_t last_catalog_ms;
    int tap_stage_pending;  /* -1 = none, >= 0 = stage index */
} DebugServerState;

static DebugServer *g_server = NULL;

/* ============================================================================
 * Socket utilities
 * ============================================================================ */

/* Called with client_lock held. A slow reader is not a lost connection. */
static bool flush_output(DebugServerState *state) {
    if (state->client_sock < 0) return false;
    while (state->output_start < state->output_end) {
        ssize_t n = send(state->client_sock, state->output + state->output_start,
                         state->output_end - state->output_start,
                         MSG_DONTWAIT | DEBUG_SERVER_MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return true;
        if (n <= 0) {
            fprintf(stderr,"Signal Studio send closed: %s\n",n<0 ? strerror(errno) : "EOF");
            close(state->client_sock);
            state->client_sock = -1;
            state->output_start = state->output_end = 0;
            return false;
        }
        state->output_start += (size_t)n;
    }
    state->output_start = state->output_end = 0;
    return true;
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

#ifdef SO_NOSIGPIPE
        /* macOS/BSD: a vanished editor must not kill the emulator with SIGPIPE.
         * Linux has no socket option for this; send() uses MSG_NOSIGNAL instead. */
        int no_sigpipe = 1;
        setsockopt(client, SOL_SOCKET, SO_NOSIGPIPE, &no_sigpipe, sizeof(no_sigpipe));
#endif
        int send_buffer = 65536;
        setsockopt(client, SOL_SOCKET, SO_SNDBUF, &send_buffer, sizeof(send_buffer));
        printf("Visualiser connected\n");

        pthread_mutex_lock(&state->client_lock);
        if (state->client_sock >= 0) {
            close(state->client_sock);
        }
        state->client_sock = client;
        state->output_start = state->output_end = 0;
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
    const size_t telemetry_limit = 32 * 1024;
    const size_t tap_limit = 32 * 1024 * 1024;
    const size_t reply_reserve = 64 * 1024;
    size_t size = sizeof(DebugMessageHeader) + (size_t)payload_size;
    bool accepted = false;
    pthread_mutex_lock(&state->client_lock);
    if (!flush_output(state)) goto done;

    size_t pending = state->output_end - state->output_start;
    size_t limit = msg_type == 9 ? tap_limit + reply_reserve :
                   msg_type == DEBUG_MSG_TAP_DATA ? tap_limit : telemetry_limit;
    /* Drop replaceable telemetry under pressure, preserving whole frames and
     * reserving space for command acknowledgements. Never stall emulation. */
    if (size > limit || pending > limit - size) goto done;
    if (state->output_start) {
        memmove(state->output, state->output + state->output_start, pending);
        state->output_start = 0;
        state->output_end = pending;
    }
    if (pending + size > state->output_capacity) {
        size_t capacity = pending + size;
        if (capacity < telemetry_limit + reply_reserve)
            capacity = telemetry_limit + reply_reserve;
        uint8_t *output = realloc(state->output, capacity);
        if (!output) goto done;
        state->output = output;
        state->output_capacity = capacity;
    }
    DebugMessageHeader hdr = {msg_type, payload_size};
    memcpy(state->output + state->output_end, &hdr, sizeof(hdr));
    if (payload_size)
        memcpy(state->output + state->output_end + sizeof(hdr), payload, payload_size);
    state->output_end += size;
    accepted = flush_output(state);
done:
    pthread_mutex_unlock(&state->client_lock);
    return accepted;
}

/* Peek until the entire frame is available. Never consume a partial header
 * or block the emulation thread waiting for a client's payload. */
static bool read_message(DebugServerState *state, uint32_t *out_type,
                         void *out_payload, uint32_t max_payload_size, uint32_t *out_size) {
    pthread_mutex_lock(&state->client_lock);
    int sock = state->client_sock;
    if (sock < 0) { pthread_mutex_unlock(&state->client_lock); return false; }
    uint8_t frame[520];
    ssize_t n = recv(sock, frame, sizeof(frame), MSG_PEEK | MSG_DONTWAIT);
    if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
        fprintf(stderr,"Signal Studio receive closed: %s\n",n<0 ? strerror(errno) : "EOF");
        close(sock); state->client_sock = -1;
    }
    if (n < 8) { pthread_mutex_unlock(&state->client_lock); return false; }
    DebugMessageHeader hdr;
    memcpy(&hdr, frame, sizeof(hdr));
    if (hdr.payload_size > max_payload_size || hdr.payload_size > sizeof(frame)-8) {
        fprintf(stderr,"Signal Studio invalid frame: type %u, size %u\n",hdr.msg_type,hdr.payload_size);
        close(sock); state->client_sock = -1;
        pthread_mutex_unlock(&state->client_lock); return false;
    }
    size_t size = 8 + hdr.payload_size;
    if ((size_t)n < size) { pthread_mutex_unlock(&state->client_lock); return false; }
    n = recv(sock, frame, size, MSG_DONTWAIT);
    pthread_mutex_unlock(&state->client_lock);
    if (n != (ssize_t)size) return false;
    memcpy(out_payload, frame+8, hdr.payload_size);
    *out_type = hdr.msg_type; *out_size = hdr.payload_size;
    return true;
}

void debug_server_set_controls(DebugServer *srv, const DebugControl *controls, int count) {
    if (!srv || count < 0 || count > DEBUG_MAX_CONTROLS) return;
    DebugServerState *state = (DebugServerState *)srv;
    memcpy(state->controls, controls, (size_t)count * sizeof(*controls));
    state->control_count = count;
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
    free(state->output);

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

    pthread_mutex_lock(&state->client_lock);
    flush_output(state);
    pthread_mutex_unlock(&state->client_lock);

    /* Process incoming messages */
    uint32_t msg_type, msg_size;
    uint8_t msg_payload[512];

    for (int processed = 0; processed < 32 &&
         read_message(state, &msg_type, msg_payload, sizeof(msg_payload), &msg_size); processed++) {
        switch (msg_type) {
            case DEBUG_MSG_TAP_REQUEST: {
                if (msg_size == sizeof(DebugTapRequest)) {
                    DebugTapRequest *req = (DebugTapRequest *)msg_payload;
                    state->tap_stage_pending = (int)req->stage_index;
                }
                break;
            }

            case 8: { /* op, preset id, catalog revision, UTF-8 name[128] */
                if(msg_size!=140) break;
                uint32_t op,id,revision;
                memcpy(&op,msg_payload,4); memcpy(&id,msg_payload+4,4); memcpy(&revision,msg_payload+8,4);
                char name[128]; memcpy(name,msg_payload+12,128); name[127]=0;
                uint8_t result[136]={0}; memcpy(result,&op,4);
                uint32_t ok=preset_manage(op,(int)id,revision,name,(char *)result+8,128);
                memcpy(result+4,&ok,4); send_message(state,9,result,sizeof(result));
                state->last_catalog_ms=0;
                break;
            }

            case 6: { /* physical control: uint32 id, float value */
                uint32_t id; float value;
                if (msg_size != 8) break;
                memcpy(&id, msg_payload, 4); memcpy(&value, msg_payload+4, 4);
                if (id >= (uint32_t)state->control_count || !isfinite(value)) break;
                DebugControl *control = &state->controls[id];
                if (value < control->minimum || value > control->maximum) break;
                *control->value = value;
                if (control->apply) control->apply();
                break;
            }

            default:
                break;
        }
    }

    uint64_t now = SDL_GetTicks();
    if (now - state->last_snapshot_ms < 250) return;
    state->last_snapshot_ms = now;

    if(!state->last_catalog_ms || now-state->last_catalog_ms>=500) {
        uint8_t catalog[20+63*136]={0};
        uint32_t header[]={1,preset_catalog_revision(),(uint32_t)preset_total_count(),
            (uint32_t)preset_active_index(),preset_is_modified() ? 1u : 0u};
        memcpy(catalog,header,sizeof(header));
        for(uint32_t i=0;i<header[2];i++) {
            uint8_t *r=catalog+20+i*136; uint32_t user=preset_is_user((int)i);
            memcpy(r,&i,4); memcpy(r+4,&user,4); snprintf((char *)r+8,128,"%s",preset_display_name((int)i));
        }
        send_message(state,7,catalog,20+header[2]*136); state->last_catalog_ms=now;
    }

    /* Build and send snapshot */
    uint32_t num_video_stages = video_chain ? video_chain->num_stages : 0;
    uint32_t num_audio_stages = audio_chain ? audio_chain->num_stages : 0;

    /* Snapshot header + per-stage info */
    DebugSnapshotHeader snap_hdr;
    snap_hdr.frame_number = frame_number;
    snap_hdr.timestamp_ms = (uint32_t)now;
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
        stage_infos[i].enabled = i >= (uint32_t)video_chain->first_stage && video_chain->stages[i].enabled ? 1 : 0;
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

    /* Versioned physical-control snapshot. Fixed records avoid C struct padding. */
    uint8_t controls[8 + DEBUG_MAX_CONTROLS * 72] = {0};
    uint32_t version = 1, count = (uint32_t)state->control_count;
    memcpy(controls, &version, 4); memcpy(controls+4, &count, 4);
    for (uint32_t i = 0; i < count; i++) {
        DebugControl *c = &state->controls[i];
        uint8_t *record = controls + 8 + i*72;
        memcpy(record, &i, 4); memcpy(record+4, c->value, 4);
        memcpy(record+8, &c->minimum, 4); memcpy(record+12, &c->maximum, 4);
        snprintf((char *)record+16, 32, "%s", c->name);
        snprintf((char *)record+48, 24, "%s", c->group);
    }
    send_message(state, 5, controls, 8 + count*72);
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
