#include "camera.hpp"
#include "protocol.hpp"

#include <arpa/inet.h>
#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

namespace {

std::atomic<bool> g_stop{false};

void signal_handler(int) {
    g_stop.store(true);
}

std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 4);
    for (char c : s) {
        if (c == '"' || c == '\\') {
            out.push_back('\\');
            out.push_back(c);
        } else if (c == '\n') {
            out += "\\n";
        } else if (c == '\r') {
            out += "\\r";
        } else {
            out.push_back(c);
        }
    }
    return out;
}

std::string status_to_json(const rawcam::CameraStatus& s) {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(2);
    oss << "{"
        << "\"camera_model\":\"" << json_escape(s.camera_model) << "\","
        << "\"video_dev\":\"" << json_escape(s.video_dev) << "\","
        << "\"subdev_dev\":\"" << json_escape(s.subdev_dev) << "\","
        << "\"i2c_dev\":\"" << json_escape(s.i2c_dev) << "\","
        << "\"sensor_width\":" << s.sensor_width << ","
        << "\"sensor_height\":" << s.sensor_height << ","
        << "\"min_roi_width\":" << s.min_roi_width << ","
        << "\"min_roi_height\":" << s.min_roi_height << ","
        << "\"bit_depth\":" << s.bit_depth << ","
        << "\"roi_x\":" << s.roi_x << ","
        << "\"roi_y\":" << s.roi_y << ","
        << "\"width\":" << s.width << ","
        << "\"height\":" << s.height << ","
        << "\"stride\":" << s.stride << ","
        << "\"stream_mode\":\"" << json_escape(s.stream_mode) << "\","
        << "\"exposure_mode\":\"" << json_escape(s.exposure_mode) << "\","
        << "\"target_exposure_us\":" << s.target_exposure_us << ","
        << "\"actual_exposure_us\":" << s.actual_exposure_us << ","
        << "\"gain_mode\":\"" << json_escape(s.gain_mode) << "\","
        << "\"target_gain_db\":" << s.target_gain_db << ","
        << "\"actual_gain_db\":" << s.actual_gain_db << ","
        << "\"target_fps\":" << s.target_fps << ","
        << "\"effective_fps\":" << s.effective_fps << ","
        << "\"sensor_max_fps\":" << s.sensor_max_fps << ","
        << "\"measured_fps\":" << s.measured_fps << ","
        << "\"fps_clamped_by_exposure\":" << (s.fps_clamped_by_exposure ? "true" : "false") << ","
        << "\"frame_seq\":" << s.frame_seq << ","
        << "\"dropped_frames\":" << s.dropped_frames << ","
        << "\"streaming\":" << (s.streaming ? "true" : "false")
        << "}";
    return oss.str();
}

bool json_get_string(const std::string& json, const std::string& key, std::string& out) {
    std::string pat = "\"" + key + "\"";
    auto pos = json.find(pat);
    if (pos == std::string::npos) return false;
    pos = json.find(':', pos + pat.size());
    if (pos == std::string::npos) return false;
    pos = json.find('"', pos + 1);
    if (pos == std::string::npos) return false;
    auto end = json.find('"', pos + 1);
    if (end == std::string::npos) return false;
    out = json.substr(pos + 1, end - pos - 1);
    return true;
}

bool json_get_double(const std::string& json, const std::string& key, double& out) {
    std::string pat = "\"" + key + "\"";
    auto pos = json.find(pat);
    if (pos == std::string::npos) return false;
    pos = json.find(':', pos + pat.size());
    if (pos == std::string::npos) return false;
    ++pos;
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) ++pos;
    char* endptr = nullptr;
    out = std::strtod(json.c_str() + pos, &endptr);
    return endptr != json.c_str() + pos;
}

bool json_get_u32(const std::string& json, const std::string& key, uint32_t& out) {
    double v = 0;
    if (!json_get_double(json, key, v) || v < 0) return false;
    out = static_cast<uint32_t>(v + 0.5);
    return true;
}

int create_listen_socket(uint16_t port) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int opt = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);
    if (::bind(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(fd);
        return -1;
    }
    if (::listen(fd, 8) < 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

bool send_all(int fd, const void* data, size_t len) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    size_t sent = 0;
    while (sent < len && !g_stop.load()) {
        ssize_t n = ::send(fd, p + sent, len - sent, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (n == 0) return false;
        sent += static_cast<size_t>(n);
    }
    return sent == len;
}

std::string handle_command(rawcam::Camera& cam, const std::string& line) {
    std::string cmd;
    if (!json_get_string(line, "cmd", cmd)) {
        return "{\"ok\":false,\"error\":\"missing cmd\"}\n";
    }

    std::string err;
    bool ok = true;
    if (cmd == "get_status") {
        // return status below
    } else if (cmd == "set_exposure") {
        auto st = cam.get_status();
        std::string mode = st.exposure_mode;
        uint32_t exp_us = st.target_exposure_us;
        json_get_string(line, "mode", mode);
        json_get_u32(line, "exposure_us", exp_us);
        ok = cam.set_exposure(mode, exp_us, err);
    } else if (cmd == "set_gain") {
        auto st = cam.get_status();
        std::string mode = st.gain_mode;
        double gain_db = st.target_gain_db;
        json_get_string(line, "mode", mode);
        json_get_double(line, "gain_db", gain_db);
        ok = cam.set_gain(mode, gain_db, err);
    } else if (cmd == "set_fps") {
        double fps = 30.0;
        if (!json_get_double(line, "target_fps", fps)) {
            return "{\"ok\":false,\"error\":\"missing target_fps\"}\n";
        }
        ok = cam.set_fps(fps, err);
    } else if (cmd == "set_format") {
        auto st = cam.get_status();
        uint32_t bd = st.bit_depth;
        uint32_t rx = st.roi_x;
        uint32_t ry = st.roi_y;
        uint32_t w = st.width;
        uint32_t h = st.height;
        json_get_u32(line, "bit_depth", bd);
        json_get_u32(line, "roi_x", rx);
        json_get_u32(line, "roi_y", ry);
        json_get_u32(line, "width", w);
        json_get_u32(line, "height", h);
        ok = cam.set_format(static_cast<uint16_t>(bd), rx, ry, w, h, err);
    } else if (cmd == "set_stream_mode") {
        std::string mode;
        if (!json_get_string(line, "mode", mode)) {
            return "{\"ok\":false,\"error\":\"missing mode\"}\n";
        }
        ok = cam.set_stream_mode(mode, err);
    } else {
        return "{\"ok\":false,\"error\":\"unknown cmd\"}\n";
    }

    if (!ok) {
        return "{\"ok\":false,\"error\":\"" + json_escape(err) + "\"}\n";
    }
    return "{\"ok\":true,\"status\":" + status_to_json(cam.get_status()) + "}\n";
}

void handle_ctrl_client(int cfd, rawcam::Camera& cam) {
    int nodelay = 1;
    ::setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

    std::string buf;
    char tmp[1024];
    while (!g_stop.load()) {
        struct pollfd pfd{cfd, POLLIN, 0};
        int pr = ::poll(&pfd, 1, 250);
        if (pr == 0) continue;
        if (pr < 0) break;
        ssize_t n = ::recv(cfd, tmp, sizeof(tmp), 0);
        if (n <= 0) break;
        buf.append(tmp, static_cast<size_t>(n));
        size_t nl;
        while ((nl = buf.find('\n')) != std::string::npos) {
            std::string line = buf.substr(0, nl);
            buf.erase(0, nl + 1);
            if (line.empty()) continue;
            std::string resp = handle_command(cam, line);
            if (!send_all(cfd, resp.data(), resp.size())) {
                ::close(cfd);
                return;
            }
        }
    }
    ::close(cfd);
}

void handle_stream_client(int cfd, rawcam::Camera& cam, std::string peer) {
    int nodelay = 1;
    ::setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
    int sndbuf = 8 * 1024 * 1024;
    ::setsockopt(cfd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));

    std::cout << "[stream] client connected: " << peer << std::endl;
    cam.request_keyframe();

    rawcam::FrameBuffer frame;
    uint64_t last_seq = 0;
    while (!g_stop.load()) {
        if (!cam.wait_next_frame(last_seq, frame, 500)) {
            continue;
        }
        last_seq = frame.header.frame_seq;
        if (!send_all(cfd, &frame.header, sizeof(frame.header))) break;
        if (!send_all(cfd, frame.data.data(), frame.header.payload_size)) break;
    }

    std::cout << "[stream] client disconnected: " << peer << std::endl;
    ::close(cfd);
}

void handle_snap_client(int cfd, rawcam::Camera& cam, std::string peer) {
    int nodelay = 1;
    ::setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

    // Check if client sends a JSON request line (e.g. long_shot); wait up to 80ms
    std::string req_line;
    struct pollfd pfd{cfd, POLLIN, 0};
    if (::poll(&pfd, 1, 80) > 0 && (pfd.revents & POLLIN)) {
        char buf[512];
        ssize_t n = ::recv(cfd, buf, sizeof(buf) - 1, 0);
        if (n > 0) {
            buf[n] = '\0';
            req_line = buf;
        }
    }

    rawcam::FrameBuffer raw;
    bool ok = false;
    std::string cmd;
    if (json_get_string(req_line, "cmd", cmd) && cmd == "long_shot") {
        auto st = cam.get_status();
        uint32_t exp_us = st.target_exposure_us;
        double gain_db = st.target_gain_db;
        json_get_u32(req_line, "exposure_us", exp_us);
        json_get_double(req_line, "gain_db", gain_db);
        std::string err;
        std::cout << "[snap] long_shot exp=" << (exp_us / 1000.0) << "ms gain=" << gain_db
                  << "dB for " << peer << std::endl;
        ok = cam.capture_long_shot(exp_us, gain_db, raw, err);
        if (!ok) {
            std::cerr << "[snap] long_shot failed: " << err << std::endl;
        }
    } else {
        ok = cam.get_latest_raw_frame(raw);
    }

    if (ok) {
        send_all(cfd, &raw.header, sizeof(raw.header));
        send_all(cfd, raw.data.data(), raw.header.payload_size);
        std::cout << "[snap] sent lossless RAW" << raw.header.bit_depth
                  << " (" << raw.header.width << "x" << raw.header.height
                  << ", exp=" << (raw.header.exposure_us / 1000.0) << "ms) to " << peer << std::endl;
    }
    ::close(cfd);
}

void accept_loop(int lfd, std::function<void(int, std::string)> on_client) {
    while (!g_stop.load()) {
        struct pollfd pfd{lfd, POLLIN, 0};
        int pr = ::poll(&pfd, 1, 250);
        if (pr <= 0) continue;
        struct sockaddr_in caddr{};
        socklen_t clen = sizeof(caddr);
        int cfd = ::accept(lfd, reinterpret_cast<struct sockaddr*>(&caddr), &clen);
        if (cfd < 0) continue;
        char ip[INET_ADDRSTRLEN]{};
        ::inet_ntop(AF_INET, &caddr.sin_addr, ip, sizeof(ip));
        std::string peer = std::string(ip) + ":" + std::to_string(ntohs(caddr.sin_port));
        std::thread(on_client, cfd, peer).detach();
    }
}

} // namespace

int main(int argc, char** argv) {
    uint16_t ctrl_port = 5600;
    uint16_t stream_port = 5601;
    uint16_t snap_port = 5602;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--ctrl-port" && i + 1 < argc) {
            ctrl_port = static_cast<uint16_t>(std::atoi(argv[++i]));
        } else if (arg == "--stream-port" && i + 1 < argc) {
            stream_port = static_cast<uint16_t>(std::atoi(argv[++i]));
        } else if (arg == "--snap-port" && i + 1 < argc) {
            snap_port = static_cast<uint16_t>(std::atoi(argv[++i]));
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: " << argv[0] << " [--ctrl-port 5600] [--stream-port 5601] [--snap-port 5602]\n";
            return 0;
        }
    }

    std::signal(SIGPIPE, SIG_IGN);
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    rawcam::Camera cam;
    std::string err;
    if (!cam.init(err)) {
        std::cerr << "[error] Camera init failed: " << err << std::endl;
        return 1;
    }
    if (!cam.start_capture(err)) {
        std::cerr << "[error] Camera start_capture failed: " << err << std::endl;
        return 1;
    }

    auto st = cam.get_status();
    std::cout << "[rawcamd] Model=" << st.camera_model
              << " video=" << st.video_dev
              << " subdev=" << st.subdev_dev
              << " i2c=" << st.i2c_dev
              << " fmt=" << st.width << "x" << st.height
              << " RAW" << st.bit_depth
              << " stride=" << st.stride
              << " mode=" << st.stream_mode
              << " max_fps=" << st.sensor_max_fps << std::endl;

    int ctrl_lfd = create_listen_socket(ctrl_port);
    int stream_lfd = create_listen_socket(stream_port);
    int snap_lfd = create_listen_socket(snap_port);
    if (ctrl_lfd < 0 || stream_lfd < 0 || snap_lfd < 0) {
        std::cerr << "[error] Failed to bind ports " << ctrl_port << "/" << stream_port << "/" << snap_port
                  << ": " << std::strerror(errno) << std::endl;
        if (ctrl_lfd >= 0) ::close(ctrl_lfd);
        if (stream_lfd >= 0) ::close(stream_lfd);
        if (snap_lfd >= 0) ::close(snap_lfd);
        return 1;
    }

    std::cout << "[rawcamd] Listening on ctrl:" << ctrl_port
              << " stream:" << stream_port << " snap:" << snap_port << std::endl;

    std::thread ctrl_thr(accept_loop, ctrl_lfd, [&](int cfd, std::string) {
        handle_ctrl_client(cfd, cam);
    });
    std::thread stream_thr(accept_loop, stream_lfd, [&](int cfd, std::string peer) {
        handle_stream_client(cfd, cam, std::move(peer));
    });
    std::thread snap_thr(accept_loop, snap_lfd, [&](int cfd, std::string peer) {
        handle_snap_client(cfd, cam, std::move(peer));
    });

    while (!g_stop.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    std::cout << "[rawcamd] Shutting down..." << std::endl;
    ::close(ctrl_lfd);
    ::close(stream_lfd);
    ::close(snap_lfd);
    if (ctrl_thr.joinable()) ctrl_thr.join();
    if (stream_thr.joinable()) stream_thr.join();
    if (snap_thr.joinable()) snap_thr.join();
    cam.shutdown();
    return 0;
}
