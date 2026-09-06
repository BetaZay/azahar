// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include "core/frontend/barista/barista_app_hook.h"
#include "common/logging/log.h"
#include "common/settings.h"
#include "core/3ds.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>

#if defined(__linux__) || defined(BOOST_OS_LINUX)
#include <csignal>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace {
using Clock = std::chrono::steady_clock;
constexpr size_t chunk = 16384, max_audio = 9600, frame_bytes = 864 * 480 * 3 / 2;
enum Type : u32 { Video = 1, Idle = 2, Active = 3, Pcm = 4, Input = 5, Reject = 6 };

struct LockInfo {
    bool locked = false;
    u32 pid = 0;
    std::string app;
    u64 lastSeen = 0;
};

LockInfo ReadLockFile(const std::string& lockPath) {
    LockInfo info;
    FILE* f = fopen(lockPath.c_str(), "re");
    if (!f)
        return info;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        std::string_view sv(line);
        while (!sv.empty() && (sv.back() == '\r' || sv.back() == '\n' || sv.back() == ' '))
            sv.remove_suffix(1);
        auto eq = sv.find('=');
        if (eq == std::string_view::npos)
            continue;
        auto key = sv.substr(0, eq);
        auto val = sv.substr(eq + 1);
        if (key == "pid") {
            char* end = nullptr;
            info.pid = static_cast<u32>(strtoul(std::string(val).c_str(), &end, 10));
        } else if (key == "app" || key == "name") {
            info.app = std::string(val);
        } else if (key == "last_seen") {
            char* end = nullptr;
            info.lastSeen = strtoull(std::string(val).c_str(), &end, 10);
        }
    }
    fclose(f);
    if (info.pid > 0) {
        if (kill(static_cast<pid_t>(info.pid), 0) == 0 || errno == EPERM)
            info.locked = true;
    }
    return info;
}

void put(u8* p, u32 v) {
    for (unsigned i = 0; i < 4; i++)
        p[i] = static_cast<u8>(v >> (8 * i));
}

u32 get(const u8* p) {
    return u32(p[0]) | u32(p[1]) << 8 | u32(p[2]) << 16 | u32(p[3]) << 24;
}

struct Rgb {
    std::vector<u8> b;
    unsigned w = 0, h = 0;
};

std::vector<u8> i420(const Rgb& f) {
    if (!f.w || !f.h || f.b.size() != size_t(f.w) * f.h * 3)
        return {};
    std::vector<u8> o(frame_bytes, 128);
    for (size_t y = 0; y < 480; y += 2) {
        for (size_t x = 0; x < 864; x += 2) {
            int rs = 0, gs = 0, bs = 0;
            for (size_t dy = 0; dy < 2; dy++) {
                for (size_t dx = 0; dx < 2; dx++) {
                    auto s = (((y + dy) * f.h / 480) * f.w + (x + dx) * f.w / 864) * 3;
                    int r = f.b[s], g = f.b[s + 1], b = f.b[s + 2];
                    o[(y + dy) * 864 + x + dx] =
                        std::clamp(((66 * r + 129 * g + 25 * b + 128) >> 8) + 16, 16, 235);
                    rs += r;
                    gs += g;
                    bs += b;
                }
            }
            int r = rs / 4, g = gs / 4, b = bs / 4;
            auto c = (y / 2) * 432 + x / 2;
            o[864 * 480 + c] =
                std::clamp(((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128, 16, 240);
            o[864 * 480 * 5 / 4 + c] =
                std::clamp(((112 * r - 94 * g - 18 * b + 128) >> 8) + 128, 16, 240);
        }
    }
    return o;
}

int32_t s24_le_to_int32(const u8 in[3]) {
    uint32_t u = static_cast<uint32_t>(in[0]) | (static_cast<uint32_t>(in[1]) << 8) |
                 (static_cast<uint32_t>(in[2]) << 16);
    if (u & 0x00800000u) {
        u |= 0xFF000000u;
    }
    return static_cast<int32_t>(u);
}

class Client {
public:
    ~Client() {
        stop();
    }

    bool start(std::string p, std::string& e) {
        if (p.empty() || p.size() >= sizeof(sockaddr_un::sun_path)) {
            e = "invalid Barista socket path";
            return false;
        }
        path = std::move(p);
        worker = std::jthread([this] { run(); });
        return true;
    }

    void stop() {
        stopping = true;
        if (worker.joinable())
            worker.join();
    }

    bool connected() const {
        return linked;
    }

    void active(bool a) {
        std::lock_guard l(m);
        on = a;
        if (!a) {
            audio.clear();
            pending = {};
        }
    }

    void rgb(std::vector<u8> b, unsigned w, unsigned h, bool idle = false) {
        if (!w || !h || b.size() != size_t(w) * h * 3)
            return;
        if (idle) {
            std::lock_guard l(m);
            logo = {std::move(b), w, h};
            logo_rev++;
            return;
        }
        std::unique_lock l(m, std::try_to_lock);
        if (!l)
            return;
        pending = {std::move(b), w, h};
    }

    void pcm(std::span<const s16> s) {
        std::unique_lock l(m, std::try_to_lock);
        if (!l || !on || !linked)
            return;
        audio.insert(audio.end(), s.begin(), s.end());
        while (audio.size() > max_audio)
            audio.pop_front();
    }

    bool input(std::array<u8, 128>& r) const {
        std::lock_guard l(m);
        if (!linked || Clock::now() - input_time > std::chrono::milliseconds(500))
            return false;
        r = in;
        return true;
    }

    u64 get_frames_sent() const {
        return frames_sent.load();
    }
    u64 get_audio_chunks_sent() const {
        return audio_chunks_sent.load();
    }
    u64 get_input_reports_received() const {
        return input_reports_received.load();
    }
    s64 get_last_input_ms() const {
        std::lock_guard l(m);
        if (input_reports_received == 0)
            return -1;
        return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - input_time)
            .count();
    }
    std::string get_rejection_reason() const {
        std::lock_guard l(m);
        return rejection_reason;
    }
    const std::string& get_path() const {
        return path;
    }

private:
    bool sendp(int fd, Type t, u32 id, u32 off, std::span<const u8> d) {
        if (d.size() > chunk)
            return false;
        std::array<u8, 16 + chunk> p{};
        put(p.data(), 0x3147554d);
        put(p.data() + 4, t);
        put(p.data() + 8, id);
        put(p.data() + 12, off);
        std::copy(d.begin(), d.end(), p.begin() + 16);
        const auto deadline = Clock::now() + std::chrono::milliseconds(100);
        do {
            const auto n = send(fd, p.data(), d.size() + 16, MSG_DONTWAIT | MSG_NOSIGNAL);
            if (n == ssize_t(d.size() + 16))
                return true;
            if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
                return false;
            pollfd q{fd, POLLOUT, 0};
            poll(&q, 1, 2);
        } while (!stopping && Clock::now() < deadline);
        return false;
    }

    bool sendf(int fd, Type t, u32 id, const std::vector<u8>& f) {
        for (size_t o = 0; o < f.size(); o += chunk)
            if (!sendp(fd, t, id, static_cast<u32>(o),
                       std::span(f).subspan(o, std::min(chunk, f.size() - o))))
                return false;
        return true;
    }

    void session(int fd) {
        {
            std::lock_guard z(m);
            rejection_reason.clear();
        }
        linked = true;
        u32 id = 0;
        u64 sent_logo = 0;
        auto beat = Clock::time_point{};
        while (!stopping) {
            Rgb f, l;
            std::vector<u8> p;
            bool a;
            {
                std::lock_guard z(m);
                f = std::move(pending);
                pending = {};
                if (sent_logo != logo_rev) {
                    l = logo;
                    sent_logo = logo_rev;
                }
                a = on;
                while (!audio.empty() && p.size() + 2 <= chunk) {
                    auto v = uint16_t(audio.front());
                    audio.pop_front();
                    p.push_back(static_cast<u8>(v));
                    p.push_back(static_cast<u8>(v >> 8));
                }
            }
            if (Clock::now() >= beat) {
                std::array<u8, 1> q{u8(a)};
                if (!sendp(fd, Active, 0, 0, q))
                    break;
                beat = Clock::now() + std::chrono::milliseconds(100);
            }
            if (!l.b.empty() && !sendf(fd, Idle, ++id, i420(l)))
                break;
            if (!f.b.empty()) {
                if (!sendf(fd, Video, ++id, i420(f)))
                    break;
                frames_sent++;
            }
            if (!p.empty()) {
                if (!sendp(fd, Pcm, 0, 0, p))
                    break;
                audio_chunks_sent++;
            }
            pollfd q{fd, POLLIN, 0};
            poll(&q, 1, 2);
            if (q.revents & (POLLERR | POLLNVAL))
                break;
            if (q.revents & (POLLIN | POLLHUP)) {
                std::array<u8, 16 + chunk> b;
                auto n = recv(fd, b.data(), b.size(), MSG_DONTWAIT | MSG_TRUNC);
                if (n >= 16 && get(b.data()) == 0x3147554d) {
                    const auto type = get(b.data() + 4);
                    if (type == Reject) {
                        std::string reason(reinterpret_cast<const char*>(b.data() + 16), n - 16);
                        std::lock_guard z(m);
                        rejection_reason = reason;
                        LOG_ERROR(Frontend, "Barista connection rejected: {}", reason);
                        break;
                    }
                    if (type == Input && n == 144) {
                        std::lock_guard z(m);
                        std::copy_n(b.data() + 16, 128, in.begin());
                        input_time = Clock::now();
                        input_reports_received++;
                    }
                } else if (n <= 0 && (q.revents & POLLHUP)) {
                    break;
                }
            }
        }
        linked = false;
        close(fd);
    }

    void run() {
        while (!stopping) {
            const auto lock = ReadLockFile(path + ".lock");
            if (lock.locked && lock.pid != static_cast<u32>(getpid())) {
                std::lock_guard l(m);
                rejection_reason = "Busy: already connected to " +
                                   (lock.app.empty() ? "another app" : lock.app) + " (PID " +
                                   std::to_string(lock.pid) + ")";
            }

            int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
            if (fd >= 0) {
                int buf_size = 2 * 1024 * 1024;
                setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &buf_size, sizeof(buf_size));
                setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &buf_size, sizeof(buf_size));
            }
            sockaddr_un a{};
            a.sun_family = AF_UNIX;
            std::memcpy(a.sun_path, path.c_str(), path.size() + 1);
            if (fd >= 0 && connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0) {
                {
                    std::lock_guard l(m);
                    if (!logo.b.empty()) {
                        auto yuv = i420(logo);
                        if (yuv.size() == frame_bytes) {
                            std::string logo_path = path + ".idle.i420";
                            FILE* f = fopen(logo_path.c_str(), "wb");
                            if (f) {
                                fwrite(yuv.data(), 1, yuv.size(), f);
                                fclose(f);
                            }
                        }
                    }
                }
                session(fd);
                continue;
            }
            if (fd >= 0)
                close(fd);
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    std::atomic_bool stopping = false, linked = false;
    std::atomic<u64> frames_sent = 0, audio_chunks_sent = 0, input_reports_received = 0;
    std::jthread worker;
    std::string path;
    std::string rejection_reason;
    mutable std::mutex m;
    Rgb pending, logo;
    std::deque<s16> audio;
    std::array<u8, 128> in{};
    Clock::time_point input_time{};
    bool on = false;
    u64 logo_rev = 0;
};

std::atomic<std::shared_ptr<Client>> bridge;
std::atomic_bool game = false;
std::atomic_bool s_enabled = false;
std::mutex s_configMutex;
std::string s_configuredPath;
std::vector<u8> s_idleCanvas;
unsigned s_idleWidth = 0, s_idleHeight = 0;

// Audio resampler state (32728 Hz -> 48000 Hz)
double s_audioPhase = 0.0;
s16 s_lastSampleL = 0, s_lastSampleR = 0;

std::string ResolveSocketPath(const std::string& custom) {
    if (!custom.empty())
        return custom;
    if (auto p = std::getenv("BARISTA_MUG_SOCKET"); p && *p)
        return p;
    return "/run/barista/media-" + std::to_string(static_cast<unsigned long>(geteuid())) + ".sock";
}

} // namespace
#endif

namespace BaristaAppHook {

namespace {

Common::Rectangle<u32> FitScreen(const Common::Rectangle<u32>& bounds, u32 source_width,
                                 u32 source_height) {
    const u32 bounds_width = bounds.GetWidth();
    const u32 bounds_height = bounds.GetHeight();
    if (bounds_width == 0 || bounds_height == 0 || source_width == 0 || source_height == 0)
        return bounds;

    u32 width = bounds_width;
    u32 height = bounds_height;
    if (static_cast<u64>(bounds_width) * source_height >
        static_cast<u64>(bounds_height) * source_width) {
        width = static_cast<u32>(static_cast<u64>(bounds_height) * source_width / source_height);
    } else {
        height = static_cast<u32>(static_cast<u64>(bounds_width) * source_height / source_width);
    }
    const u32 left = bounds.left + (bounds_width - width) / 2;
    const u32 top = bounds.top + (bounds_height - height) / 2;
    return {left, top, left + width, top + height};
}

} // namespace

std::string GetDefaultSocketPath() {
#if defined(__linux__) || defined(BOOST_OS_LINUX)
    return "/run/barista/media-" + std::to_string(static_cast<unsigned long>(geteuid())) + ".sock";
#else
    return "";
#endif
}

std::string GetEffectiveSocketPath() {
#if defined(__linux__) || defined(BOOST_OS_LINUX)
    std::lock_guard lock(s_configMutex);
    return ResolveSocketPath(s_configuredPath);
#else
    return "";
#endif
}

bool IsConnected() {
#if defined(__linux__) || defined(BOOST_OS_LINUX)
    auto b = bridge.load();
    return b && b->connected();
#else
    return false;
#endif
}

Status GetStatus() {
    Status s;
#if defined(__linux__) || defined(BOOST_OS_LINUX)
    std::lock_guard lock(s_configMutex);
    s.enabled = s_enabled.load();
    s.gameActive = game.load();
    s.configuredSocketPath = s_configuredPath;
    s.effectiveSocketPath = ResolveSocketPath(s_configuredPath);

    if (!s.effectiveSocketPath.empty()) {
        struct stat st {};
        if (stat(s.effectiveSocketPath.c_str(), &st) == 0)
            s.socketFileExists = true;

        const auto lockInfo = ReadLockFile(s.effectiveSocketPath + ".lock");
        if (lockInfo.locked && lockInfo.pid != static_cast<u32>(getpid())) {
            s.lockHolder = "Locked by " + (lockInfo.app.empty() ? "another app" : lockInfo.app) +
                           " (PID " + std::to_string(lockInfo.pid) + ")";
        }
    }

    if (auto b = bridge.load()) {
        s.connected = b->connected();
        s.framesSent = b->get_frames_sent();
        s.audioChunksSent = b->get_audio_chunks_sent();
        s.inputReportsReceived = b->get_input_reports_received();
        s.lastInputMsAgo = b->get_last_input_ms();
        s.rejectionReason = b->get_rejection_reason();
    }
#endif
    return s;
}

void Reconnect() {
#if defined(__linux__) || defined(BOOST_OS_LINUX)
    std::lock_guard lock(s_configMutex);
    if (!s_enabled.load())
        return;

    if (auto old = bridge.exchange({}))
        old->stop();

    const auto effectivePath = ResolveSocketPath(s_configuredPath);
    auto b = std::make_shared<Client>();
    if (!s_idleCanvas.empty())
        b->rgb(s_idleCanvas, s_idleWidth, s_idleHeight, true);
    std::string e;
    if (!b->start(effectivePath, e)) {
        LOG_ERROR(Frontend, "Barista client could not reconnect: {}", e);
        return;
    }
    b->active(game.load());
    bridge.store(b);
    LOG_INFO(Frontend, "Barista client reconnected: {}", effectivePath);
#endif
}

void Reconfigure(const std::string& customSocketPath, bool enabled) {
#if defined(__linux__) || defined(BOOST_OS_LINUX)
    std::lock_guard lock(s_configMutex);
    const bool pathChanged = (s_configuredPath != customSocketPath);
    const bool enabledChanged = (s_enabled.load() != enabled);

    s_configuredPath = customSocketPath;
    s_enabled.store(enabled);

    if (!enabled) {
        if (auto b = bridge.exchange({}))
            b->stop();
        return;
    }

    if (pathChanged || enabledChanged || !bridge.load()) {
        if (auto old = bridge.exchange({}))
            old->stop();

        const auto effectivePath = ResolveSocketPath(s_configuredPath);
        auto b = std::make_shared<Client>();
        if (!s_idleCanvas.empty())
            b->rgb(s_idleCanvas, s_idleWidth, s_idleHeight, true);
        std::string e;
        if (!b->start(effectivePath, e)) {
            LOG_ERROR(Frontend, "Barista client could not start: {}", e);
            return;
        }
        b->active(game.load());
        bridge.store(b);
        LOG_INFO(Frontend, "Barista client reconfigured: {}", effectivePath);
    }
#endif
}

void Initialize(std::vector<u8> rgb, unsigned w, unsigned h, const std::string& customSocketPath,
                bool enabled) {
#if defined(__linux__) || defined(BOOST_OS_LINUX)
    std::lock_guard lock(s_configMutex);
    s_idleCanvas = rgb;
    s_idleWidth = w;
    s_idleHeight = h;
    s_configuredPath = customSocketPath;
    s_enabled.store(enabled);

    if (!s_enabled.load())
        return;

    const auto effectivePath = ResolveSocketPath(s_configuredPath);
    auto b = std::make_shared<Client>();
    b->rgb(std::move(rgb), w, h, true);
    std::string e;
    if (!b->start(effectivePath, e)) {
        LOG_ERROR(Frontend, "Barista client could not start: {}", e);
        return;
    }
    b->active(game.load());
    bridge.store(b);
    LOG_INFO(Frontend, "Barista client enabled: {}", effectivePath);
#endif
}

void Shutdown() {
#if defined(__linux__) || defined(BOOST_OS_LINUX)
    game = false;
    if (auto b = bridge.exchange({}))
        b->stop();
#endif
}

void SetGameActive(bool a) {
#if defined(__linux__) || defined(BOOST_OS_LINUX)
    game = a;
    if (auto b = bridge.load())
        b->active(a);
#endif
}

bool WantsFrame() {
#if defined(__linux__) || defined(BOOST_OS_LINUX)
    if (!s_enabled.load())
        return false;
    auto b = bridge.load();
    if (!game || !b || !b->connected())
        return false;
    static auto last = Clock::time_point{};
    auto now = Clock::now();
    if (now - last < std::chrono::milliseconds(14))
        return false;
    last = now;
    return true;
#else
    return false;
#endif
}

void SubmitFrame(std::vector<u8> r, unsigned w, unsigned h) {
#if defined(__linux__) || defined(BOOST_OS_LINUX)
    if (!s_enabled.load())
        return;
    if (auto b = bridge.load(); b && game)
        b->rgb(std::move(r), w, h);
#endif
}

void SubmitAudio(std::span<const s16> samples, unsigned channels) {
#if defined(__linux__) || defined(BOOST_OS_LINUX)
    if (!s_enabled.load() || channels != 2 || samples.size() < 2 || samples.size() % 2 != 0)
        return;
    auto b = bridge.load();
    if (!b || !game || !b->connected())
        return;

    // Resample from 32728 Hz to 48000 Hz
    constexpr double in_rate = 32728.0;
    constexpr double out_rate = 48000.0;
    constexpr double step = in_rate / out_rate;

    const size_t in_frames = samples.size() / 2;
    std::vector<s16> resampled;
    resampled.reserve(static_cast<size_t>(in_frames * (out_rate / in_rate) * 2 + 16));

    while (s_audioPhase < in_frames) {
        size_t idx = static_cast<size_t>(s_audioPhase);
        double frac = s_audioPhase - idx;

        s16 curL = samples[idx * 2];
        s16 curR = samples[idx * 2 + 1];
        s16 nextL = (idx + 1 < in_frames) ? samples[(idx + 1) * 2] : curL;
        s16 nextR = (idx + 1 < in_frames) ? samples[(idx + 1) * 2 + 1] : curR;

        s16 outL = static_cast<s16>(curL + frac * (nextL - curL));
        s16 outR = static_cast<s16>(curR + frac * (nextR - curR));

        resampled.push_back(outL);
        resampled.push_back(outR);

        s_audioPhase += step;
    }

    s_audioPhase -= in_frames;
    b->pcm(resampled);
#endif
}

bool ReadInput(std::array<u8, 128>& r) {
#if defined(__linux__) || defined(BOOST_OS_LINUX)
    if (!s_enabled.load())
        return false;
    if (auto b = bridge.load())
        return b->input(r);
#endif
    return false;
}

static constexpr u32 ButtonToMask(Button button) {
    switch (button) {
    case Button::A:
        return 0x8000;
    case Button::B:
        return 0x4000;
    case Button::X:
        return 0x2000;
    case Button::Y:
        return 0x1000;
    case Button::Left:
        return 0x800;
    case Button::Right:
        return 0x400;
    case Button::Up:
        return 0x200;
    case Button::Down:
        return 0x100;
    case Button::ZL:
        return 0x80;
    case Button::ZR:
        return 0x40;
    case Button::L:
        return 0x20;
    case Button::R:
        return 0x10;
    case Button::Start:
        return 0x8;
    case Button::Select:
        return 0x4;
    case Button::Home:
        return 0x2;
    case Button::Sync:
        return 0x1;
    case Button::L3:
        return 0x800000;
    case Button::R3:
        return 0x400000;
    case Button::TV:
        return 0x200000;
    default:
        return 0;
    }
}

bool GetButton(Button button) {
#if defined(__linux__) || defined(BOOST_OS_LINUX)
    std::array<u8, 128> report{};
    if (!ReadInput(report))
        return false;
    return IsButtonPressed(report, button);
#else
    return false;
#endif
}

std::pair<float, float> GetStick(bool right_stick) {
#if defined(__linux__) || defined(BOOST_OS_LINUX)
    std::array<u8, 128> report{};
    if (!ReadInput(report))
        return {0.0f, 0.0f};
    return DecodeStick(report, right_stick);
#else
    return {0.0f, 0.0f};
#endif
}

bool IsButtonPressed(const std::array<u8, 128>& report, Button button) {
    const u32 buttons = (u32(report[80]) << 16) | (u32(report[2]) << 8) | report[3];
    return (buttons & ButtonToMask(button)) != 0;
}

std::pair<float, float> DecodeStick(const std::array<u8, 128>& report, bool right_stick) {
    const size_t offset = right_stick ? 10 : 6;
    const s32 raw_x = report[offset] | (u32(report[offset + 1]) << 8);
    const s32 raw_y = report[offset + 2] | (u32(report[offset + 3]) << 8);

    float norm_x = std::clamp((raw_x - 2048) / 1150.0f, -1.0f, 1.0f);
    float norm_y = std::clamp((raw_y - 2048) / 1150.0f, -1.0f, 1.0f);

    if (std::abs(norm_x) < 0.08f)
        norm_x = 0.0f;
    if (std::abs(norm_y) < 0.08f)
        norm_y = 0.0f;

    return {norm_x, norm_y};
}

Layout::FramebufferLayout GetLayout() {
    const auto mode = Settings::values.barista_screen_mode.GetValue();
    Layout::FramebufferLayout layout{};
    layout.width = 864;
    layout.height = 480;
    layout.is_rotated = true;
    layout.render_3d_mode = Settings::StereoRenderOption::Off;

    switch (mode) {
    case Settings::BaristaScreenMode::BottomScreen:
        // Preserve the native 4:3 bottom-screen aspect ratio in the GamePad frame.
        layout.top_screen_enabled = false;
        layout.bottom_screen_enabled = true;
        layout.bottom_screen = FitScreen({0, 0, 864, 480}, Core::kScreenBottomWidth,
                                         Core::kScreenBottomHeight);
        layout.top_screen = FitScreen({0, 0, 864, 480}, Core::kScreenTopWidth,
                                      Core::kScreenTopHeight);
        break;

    case Settings::BaristaScreenMode::TopScreen:
        // Preserve the native 5:3 top-screen aspect ratio in the GamePad frame.
        layout.top_screen_enabled = true;
        layout.bottom_screen_enabled = false;
        layout.top_screen = FitScreen({0, 0, 864, 480}, Core::kScreenTopWidth,
                                      Core::kScreenTopHeight);
        layout.bottom_screen = FitScreen({0, 0, 864, 480}, Core::kScreenBottomWidth,
                                         Core::kScreenBottomHeight);
        break;

    case Settings::BaristaScreenMode::SideBySide:
        // Allocate 5:3 and 4:3 slots, then letterbox within each. The unused
        // area is left black by the Barista framebuffer clear.
        layout.top_screen_enabled = true;
        layout.bottom_screen_enabled = true;
        layout.top_screen = FitScreen({0, 0, 480, 480}, Core::kScreenTopWidth,
                                      Core::kScreenTopHeight);
        layout.bottom_screen = FitScreen({480, 0, 864, 480}, Core::kScreenBottomWidth,
                                         Core::kScreenBottomHeight);
        break;

    case Settings::BaristaScreenMode::TopBottom:
        // Each screen retains its native aspect ratio; side margins are black.
        layout.top_screen_enabled = true;
        layout.bottom_screen_enabled = true;
        layout.top_screen = FitScreen({0, 0, 864, 240}, Core::kScreenTopWidth,
                                      Core::kScreenTopHeight);
        layout.bottom_screen = FitScreen({0, 240, 864, 480}, Core::kScreenBottomWidth,
                                         Core::kScreenBottomHeight);
        break;
    }

    return layout;
}

bool GetTouch(float& out_x, float& out_y) {
#if defined(__linux__) || defined(BOOST_OS_LINUX)
    std::array<u8, 128> report{};
    if (!ReadInput(report))
        return false;
    return DecodeTouch(report, out_x, out_y);
#else
    return false;
#endif
}

bool DecodeTouch(const std::array<u8, 128>& report, float& out_x, float& out_y) {
    // Check pressure bits
    int pressure = ((report[37] >> 4) & 7) | (((report[39] >> 4) & 7) << 3) |
                   (((report[41] >> 4) & 7) << 6) | (((report[43] >> 4) & 7) << 9);
    if (pressure == 0)
        return false;

    // Average 10 touchscreen points
    int ts_x = 0, ts_y = 0;
    for (int i = 0; i < 10; ++i) {
        int b = 36 + 4 * i;
        ts_x += ((report[b + 1] & 0x0F) << 8) | report[b];
        ts_y += ((report[b + 3] & 0x0F) << 8) | report[b + 2];
    }
    ts_x /= 10;
    ts_y /= 10;

    // Calibrated GamePad screen coordinate (0..854 x 0..480)
    float gx = std::clamp(-23.1097f + static_cast<float>(ts_x) * 0.2210755f, 0.0f, 854.0f);
    float gy = std::clamp(507.639f - static_cast<float>(ts_y) * 0.12772f, 0.0f, 480.0f);

    // Map to active 3DS bottom screen rectangle
    const auto layout = GetLayout();
    if (!layout.bottom_screen_enabled)
        return false;

    const auto& b = layout.bottom_screen;
    if (gx >= b.left && gx < b.right && gy >= b.top && gy < b.bottom && b.GetWidth() > 0 &&
        b.GetHeight() > 0) {
        out_x = std::clamp(static_cast<float>(gx - b.left) / static_cast<float>(b.GetWidth()), 0.0f,
                           1.0f);
        out_y = std::clamp(static_cast<float>(gy - b.top) / static_cast<float>(b.GetHeight()), 0.0f,
                           1.0f);
        return true;
    }

    return false;
}

std::pair<Common::Vec3<float>, Common::Vec3<float>> GetMotion() {
#if defined(__linux__) || defined(BOOST_OS_LINUX)
    std::array<u8, 128> report{};
    if (!ReadInput(report))
        return {};
    return DecodeMotion(report);
#else
    return {};
#endif
}

std::pair<Common::Vec3<float>, Common::Vec3<float>>
DecodeMotion(const std::array<u8, 128>& report) {
#if defined(__linux__) || defined(BOOST_OS_LINUX)
    const int16_t raw_z = static_cast<int16_t>(report[15] | (report[16] << 8));
    const int16_t raw_x = static_cast<int16_t>(report[17] | (report[18] << 8));
    const int16_t raw_y = static_cast<int16_t>(report[19] | (report[20] << 8));

    Common::Vec3<float> accel{-static_cast<float>(raw_x) / 800.0f,
                              -static_cast<float>(raw_y) / 800.0f,
                              static_cast<float>(raw_z) / 800.0f};

    const int32_t roll = s24_le_to_int32(&report[21]);
    const int32_t pitch = s24_le_to_int32(&report[24]);
    const int32_t yaw = s24_le_to_int32(&report[27]);

    constexpr float gyro_scale = (200.0f * 6.0f) / 154000.0f;
    Common::Vec3<float> gyro{roll * gyro_scale, pitch * gyro_scale, yaw * gyro_scale};

    return {accel, gyro};
#else
    return {};
#endif
}

} // namespace BaristaAppHook
