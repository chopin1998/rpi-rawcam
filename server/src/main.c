#include "camera.h"
#include "protocol.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static atomic_bool g_stop = false;

static void signal_handler(int sig) {
    (void)sig;
    atomic_store(&g_stop, true);
}

/* ---------------- Minimal flat JSON helpers ---------------- */

static bool json_get_string(const char *json, const char *key, char *out, size_t out_sz) {
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p) return false;
    p = strchr(p + strlen(pat), ':');
    if (!p) return false;
    p = strchr(p + 1, '"');
    if (!p) return false;
    ++p;
    const char *end = strchr(p, '"');
    if (!end) return false;
    size_t n = (size_t)(end - p);
    if (n >= out_sz) n = out_sz - 1;
    memcpy(out, p, n);
    out[n] = '\0';
    return true;
}

static bool json_get_double(const char *json, const char *key, double *out) {
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p) return false;
    p = strchr(p + strlen(pat), ':');
    if (!p) return false;
    ++p;
    while (*p == ' ' || *p == '\t') ++p;
    char *endptr = NULL;
    double v = strtod(p, &endptr);
    if (endptr == p) return false;
    *out = v;
    return true;
}

static bool json_get_u32(const char *json, const char *key, uint32_t *out) {
    double v = 0;
    if (!json_get_double(json, key, &v) || v < 0) return false;
    *out = (uint32_t)(v + 0.5);
    return true;
}

static int format_status_json(const CameraStatus *s, char *buf, size_t sz) {
    return snprintf(
        buf, sz,
        "{\"ok\":true,\"status\":{"
        "\"camera_model\":\"%s\",\"video_dev\":\"%s\",\"subdev_dev\":\"%s\",\"i2c_dev\":\"%s\","
        "\"sensor_width\":%u,\"sensor_height\":%u,\"min_roi_width\":%u,\"min_roi_height\":%u,"
        "\"bit_depth\":%u,\"roi_x\":%u,\"roi_y\":%u,\"width\":%u,\"height\":%u,\"stride\":%u,"
        "\"stream_mode\":\"%s\",\"exposure_mode\":\"%s\","
        "\"target_exposure_us\":%u,\"actual_exposure_us\":%u,"
        "\"gain_mode\":\"%s\",\"target_gain_db\":%.2f,\"actual_gain_db\":%.2f,"
        "\"target_fps\":%.2f,\"effective_fps\":%.2f,\"sensor_max_fps\":%.2f,\"measured_fps\":%.2f,"
        "\"fps_clamped_by_exposure\":%s,"
        "\"frame_seq\":%llu,\"dropped_frames\":%llu,\"streaming\":%s}}\n",
        s->camera_model, s->video_dev, s->subdev_dev, s->i2c_dev,
        s->sensor_width, s->sensor_height, s->min_roi_width, s->min_roi_height,
        s->bit_depth, s->roi_x, s->roi_y, s->width, s->height, s->stride,
        s->stream_mode, s->exposure_mode,
        s->target_exposure_us, s->actual_exposure_us,
        s->gain_mode, s->target_gain_db, s->actual_gain_db,
        s->target_fps, s->effective_fps, s->sensor_max_fps, s->measured_fps,
        s->fps_clamped_by_exposure ? "true" : "false",
        (unsigned long long)s->frame_seq, (unsigned long long)s->dropped_frames,
        s->streaming ? "true" : "false"
    );
}

/* ---------------- Socket helpers ---------------- */

static int create_listen_socket(uint16_t port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 || listen(fd, 8) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static bool send_all(int fd, const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;
    size_t sent = 0;
    while (sent < len && !atomic_load(&g_stop)) {
        ssize_t n = send(fd, p + sent, len - sent, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (n == 0) return false;
        sent += (size_t)n;
    }
    return sent == len;
}

/* ---------------- Command dispatcher ---------------- */

static void handle_command(Camera *cam, const char *line, char *resp, size_t resp_sz) {
    char cmd[32];
    if (!json_get_string(line, "cmd", cmd, sizeof(cmd))) {
        snprintf(resp, resp_sz, "{\"ok\":false,\"error\":\"missing cmd\"}\n");
        return;
    }

    char err[256] = { 0 };
    bool ok = true;
    CameraStatus st;

    if (strcmp(cmd, "get_status") == 0) {
        /* return status below */
    } else if (strcmp(cmd, "set_exposure") == 0) {
        camera_get_status(cam, &st);
        char mode[16];
        snprintf(mode, sizeof(mode), "%s", st.exposure_mode);
        uint32_t exp_us = st.target_exposure_us;
        json_get_string(line, "mode", mode, sizeof(mode));
        json_get_u32(line, "exposure_us", &exp_us);
        ok = camera_set_exposure(cam, mode, exp_us, err, sizeof(err));
    } else if (strcmp(cmd, "set_gain") == 0) {
        camera_get_status(cam, &st);
        char mode[16];
        snprintf(mode, sizeof(mode), "%s", st.gain_mode);
        double gain_db = st.target_gain_db;
        json_get_string(line, "mode", mode, sizeof(mode));
        json_get_double(line, "gain_db", &gain_db);
        ok = camera_set_gain(cam, mode, gain_db, err, sizeof(err));
    } else if (strcmp(cmd, "set_fps") == 0) {
        double fps = 30.0;
        if (!json_get_double(line, "target_fps", &fps)) {
            snprintf(resp, resp_sz, "{\"ok\":false,\"error\":\"missing target_fps\"}\n");
            return;
        }
        ok = camera_set_fps(cam, fps, err, sizeof(err));
    } else if (strcmp(cmd, "set_format") == 0) {
        camera_get_status(cam, &st);
        uint32_t bd = st.bit_depth, rx = st.roi_x, ry = st.roi_y, w = st.width, h = st.height;
        json_get_u32(line, "bit_depth", &bd);
        json_get_u32(line, "roi_x", &rx);
        json_get_u32(line, "roi_y", &ry);
        json_get_u32(line, "width", &w);
        json_get_u32(line, "height", &h);
        ok = camera_set_format(cam, (uint16_t)bd, rx, ry, w, h, err, sizeof(err));
    } else if (strcmp(cmd, "set_stream_mode") == 0) {
        char mode[16];
        if (!json_get_string(line, "mode", mode, sizeof(mode))) {
            snprintf(resp, resp_sz, "{\"ok\":false,\"error\":\"missing mode\"}\n");
            return;
        }
        ok = camera_set_stream_mode(cam, mode, err, sizeof(err));
    } else {
        snprintf(resp, resp_sz, "{\"ok\":false,\"error\":\"unknown cmd\"}\n");
        return;
    }

    if (!ok) {
        snprintf(resp, resp_sz, "{\"ok\":false,\"error\":\"%s\"}\n", err);
        return;
    }
    camera_get_status(cam, &st);
    format_status_json(&st, resp, resp_sz);
}

/* ---------------- Client worker threads ---------------- */

typedef struct {
    int     cfd;
    Camera *cam;
    char    peer[64];
} ClientArg;

static void *ctrl_client_thread(void *arg) {
    ClientArg *ca = (ClientArg *)arg;
    int cfd = ca->cfd;
    Camera *cam = ca->cam;
    free(ca);

    int nodelay = 1;
    setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

    char buf[4096];
    size_t used = 0;
    char resp[2048];

    while (!atomic_load(&g_stop)) {
        struct pollfd pfd = { cfd, POLLIN, 0 };
        int pr = poll(&pfd, 1, 250);
        if (pr == 0) continue;
        if (pr < 0) break;
        if (used >= sizeof(buf) - 1) used = 0;
        ssize_t n = recv(cfd, buf + used, sizeof(buf) - 1 - used, 0);
        if (n <= 0) break;
        used += (size_t)n;
        buf[used] = '\0';

        char *start = buf;
        char *nl;
        while ((nl = strchr(start, '\n')) != NULL) {
            *nl = '\0';
            if (nl > start) {
                handle_command(cam, start, resp, sizeof(resp));
                if (!send_all(cfd, resp, strlen(resp))) {
                    close(cfd);
                    return NULL;
                }
            }
            start = nl + 1;
        }
        size_t rem = strlen(start);
        if (rem > 0 && start != buf) memmove(buf, start, rem);
        used = rem;
    }
    close(cfd);
    return NULL;
}

static void *stream_client_thread(void *arg) {
    ClientArg *ca = (ClientArg *)arg;
    int cfd = ca->cfd;
    Camera *cam = ca->cam;
    char peer[64];
    snprintf(peer, sizeof(peer), "%s", ca->peer);
    free(ca);

    int nodelay = 1;
    setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
    int sndbuf = 8 * 1024 * 1024;
    setsockopt(cfd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));

    printf("[stream] client connected: %s\n", peer);
    fflush(stdout);
    camera_request_keyframe(cam);

    FrameBuffer frame;
    framebuf_init(&frame);
    uint64_t last_seq = 0;

    while (!atomic_load(&g_stop)) {
        if (!camera_wait_next_frame(cam, last_seq, &frame, 500)) continue;
        last_seq = frame.header.frame_seq;
        if (!send_all(cfd, &frame.header, sizeof(frame.header))) break;
        if (!send_all(cfd, frame.data.data, frame.header.payload_size)) break;
    }

    framebuf_free(&frame);
    printf("[stream] client disconnected: %s\n", peer);
    fflush(stdout);
    close(cfd);
    return NULL;
}

static bool recv_line(int fd, char *buf, size_t sz, int timeout_ms) {
    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);
    size_t used = 0;

    while (used + 1 < sz && !atomic_load(&g_stop)) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        int elapsed = (int)((now.tv_sec - start.tv_sec) * 1000 +
                            (now.tv_nsec - start.tv_nsec) / 1000000);
        int rem = timeout_ms - elapsed;
        if (rem <= 0) return false;

        struct pollfd pfd = { fd, POLLIN, 0 };
        int pr = poll(&pfd, 1, rem);
        if (pr <= 0) return false;

        ssize_t n = recv(fd, buf + used, sz - 1 - used, 0);
        if (n <= 0) return false;
        used += (size_t)n;
        buf[used] = '\0';

        char *nl = strchr(buf, '\n');
        if (nl) {
            *nl = '\0';
            return true;
        }
    }
    return false;
}

static void *snap_client_thread(void *arg) {
    ClientArg *ca = (ClientArg *)arg;
    int cfd = ca->cfd;
    Camera *cam = ca->cam;
    char peer[64];
    snprintf(peer, sizeof(peer), "%s", ca->peer);
    free(ca);

    int nodelay = 1;
    setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

    char req_line[512] = { 0 };
    if (!recv_line(cfd, req_line, sizeof(req_line), 2000)) {
        fprintf(stderr, "[snap] missing or incomplete request line from %s\n", peer);
        close(cfd);
        return NULL;
    }

    char cmd[32] = { 0 };
    if (!json_get_string(req_line, "cmd", cmd, sizeof(cmd))) {
        fprintf(stderr, "[snap] invalid JSON request from %s: %s\n", peer, req_line);
        close(cfd);
        return NULL;
    }

    FrameBuffer raw;
    framebuf_init(&raw);
    bool ok = false;

    if (strcmp(cmd, "long_shot") == 0) {
        CameraStatus st;
        camera_get_status(cam, &st);
        uint32_t exp_us = st.target_exposure_us;
        double gain_db = st.target_gain_db;
        json_get_u32(req_line, "exposure_us", &exp_us);
        json_get_double(req_line, "gain_db", &gain_db);
        char err[256] = { 0 };
        printf("[snap] long_shot exp=%.1fms gain=%.1fdB for %s\n", exp_us / 1000.0, gain_db, peer);
        fflush(stdout);
        ok = camera_capture_long_shot(cam, exp_us, gain_db, &raw, err, sizeof(err));
        if (!ok) fprintf(stderr, "[snap] long_shot failed: %s\n", err);
    } else if (strcmp(cmd, "snap") == 0) {
        ok = camera_get_latest_raw_frame(cam, &raw);
    } else {
        fprintf(stderr, "[snap] unknown cmd '%s' from %s\n", cmd, peer);
    }

    if (ok) {
        send_all(cfd, &raw.header, sizeof(raw.header));
        send_all(cfd, raw.data.data, raw.header.payload_size);
        printf("[snap] sent lossless RAW%u (%ux%u, exp=%.2fms) to %s\n",
               raw.header.bit_depth, raw.header.width, raw.header.height,
               raw.header.exposure_us / 1000.0, peer);
        fflush(stdout);
    }

    framebuf_free(&raw);
    close(cfd);
    return NULL;
}

/* ---------------- Accept loops ---------------- */

typedef struct {
    int     lfd;
    Camera *cam;
    void *(*handler)(void *);
} AcceptArg;

static void *accept_loop(void *arg) {
    AcceptArg *aa = (AcceptArg *)arg;
    while (!atomic_load(&g_stop)) {
        struct pollfd pfd = { aa->lfd, POLLIN, 0 };
        int pr = poll(&pfd, 1, 250);
        if (pr <= 0) continue;

        struct sockaddr_in caddr;
        socklen_t clen = sizeof(caddr);
        int cfd = accept(aa->lfd, (struct sockaddr *)&caddr, &clen);
        if (cfd < 0) continue;

        ClientArg *ca = (ClientArg *)malloc(sizeof(*ca));
        if (!ca) {
            close(cfd);
            continue;
        }
        ca->cfd = cfd;
        ca->cam = aa->cam;
        char ip[INET_ADDRSTRLEN] = { 0 };
        inet_ntop(AF_INET, &caddr.sin_addr, ip, sizeof(ip));
        snprintf(ca->peer, sizeof(ca->peer), "%s:%u", ip, ntohs(caddr.sin_port));

        pthread_t tid;
        if (pthread_create(&tid, NULL, aa->handler, ca) == 0) {
            pthread_detach(tid);
        } else {
            close(cfd);
            free(ca);
        }
    }
    return NULL;
}

int main(int argc, char **argv) {
    uint16_t ctrl_port = 5600;
    uint16_t stream_port = 5601;
    uint16_t snap_port = 5602;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--ctrl-port") == 0 && i + 1 < argc) {
            ctrl_port = (uint16_t)atoi(argv[++i]);
        } else if (strcmp(argv[i], "--stream-port") == 0 && i + 1 < argc) {
            stream_port = (uint16_t)atoi(argv[++i]);
        } else if (strcmp(argv[i], "--snap-port") == 0 && i + 1 < argc) {
            snap_port = (uint16_t)atoi(argv[++i]);
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("Usage: %s [--ctrl-port 5600] [--stream-port 5601] [--snap-port 5602]\n", argv[0]);
            return 0;
        }
    }

    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    Camera cam;
    char err[256] = { 0 };
    if (!camera_init(&cam, err, sizeof(err))) {
        fprintf(stderr, "[error] Camera init failed: %s\n", err);
        return 1;
    }
    if (!camera_start_capture(&cam, err, sizeof(err))) {
        fprintf(stderr, "[error] Camera start_capture failed: %s\n", err);
        camera_shutdown(&cam);
        return 1;
    }

    CameraStatus st;
    camera_get_status(&cam, &st);
    printf("[rawcamd] Model=%s video=%s subdev=%s i2c=%s fmt=%ux%u RAW%u stride=%u mode=%s max_fps=%.2f\n",
           st.camera_model, st.video_dev, st.subdev_dev, st.i2c_dev,
           st.width, st.height, st.bit_depth, st.stride, st.stream_mode, st.sensor_max_fps);

    int ctrl_lfd = create_listen_socket(ctrl_port);
    int stream_lfd = create_listen_socket(stream_port);
    int snap_lfd = create_listen_socket(snap_port);
    if (ctrl_lfd < 0 || stream_lfd < 0 || snap_lfd < 0) {
        fprintf(stderr, "[error] Failed to bind ports %u/%u/%u: %s\n",
                ctrl_port, stream_port, snap_port, strerror(errno));
        if (ctrl_lfd >= 0) close(ctrl_lfd);
        if (stream_lfd >= 0) close(stream_lfd);
        if (snap_lfd >= 0) close(snap_lfd);
        camera_shutdown(&cam);
        return 1;
    }

    printf("[rawcamd] Listening on ctrl:%u stream:%u snap:%u\n", ctrl_port, stream_port, snap_port);
    fflush(stdout);

    AcceptArg ctrl_arg   = { ctrl_lfd,   &cam, ctrl_client_thread };
    AcceptArg stream_arg = { stream_lfd, &cam, stream_client_thread };
    AcceptArg snap_arg   = { snap_lfd,   &cam, snap_client_thread };

    pthread_t ctrl_thr, stream_thr, snap_thr;
    pthread_create(&ctrl_thr, NULL, accept_loop, &ctrl_arg);
    pthread_create(&stream_thr, NULL, accept_loop, &stream_arg);
    pthread_create(&snap_thr, NULL, accept_loop, &snap_arg);

    while (!atomic_load(&g_stop)) {
        usleep(200000);
    }

    printf("[rawcamd] Shutting down...\n");
    close(ctrl_lfd);
    close(stream_lfd);
    close(snap_lfd);
    pthread_join(ctrl_thr, NULL);
    pthread_join(stream_thr, NULL);
    pthread_join(snap_thr, NULL);
    camera_shutdown(&cam);
    return 0;
}
