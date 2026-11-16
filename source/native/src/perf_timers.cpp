// PerfTimers: 0-60 mph, 0-100 mph, quarter mile (estimated) and 60-130 mph.
//
// While the screen is visible it runs the shared diagnostic hub as client
// "timer" (read-only, shared with the gauge apps, one gateway connection) and
// times runs from the speed it sends to UDP 42865. Best times are kept in
// best.txt beside the plugin. For closed courses only.
#include "plugin_factory.h"
#include "diagnostic_worker.h"
#include "drive_frame.h"
#include "perf_timer.h"
#include "gauge_core.h"
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/wait.h>

namespace {
const unsigned kPoll = 0x50544d52u; // "PTMR"
const unsigned kTimerPort = 42865;
const char kWorker[] = "/var/opt/hmi/data/ID5/Plugins/PerfTimers/perf-hub-qnx";
const char kBest[] = "/var/opt/hmi/data/ID5/Plugins/PerfTimers/best.txt";
const char kBestTmp[] = "/var/opt/hmi/data/ID5/Plugins/PerfTimers/best.tmp";
const char* const kNames[PT_COUNT] = {"0-60 mph", "0-100 mph", "1/4 mile (est.)", "60-130 mph"};

// Gateway address written by the installer next to the worker (one line, 169.254.x.x).
bool installed_gateway(char (&host)[16]) {
    FILE* file = fopen("/var/opt/hmi/data/ID5/Plugins/PerfTimers/gateway-ip.txt", "r");
    if (!file) return false;
    char line[32];
    bool valid = fgets(line, sizeof line, file) != NULL && fgetc(file) == EOF;
    fclose(file);
    if (!valid) return false;
    line[strcspn(line, "\r\n")] = 0;
    struct in_addr address;
    if (inet_pton(AF_INET, line, &address) != 1 || (ntohl(address.s_addr) >> 16) != 0xa9feu) return false;
    strcpy(host, line);
    return true;
}
}  // namespace

class PerfTimers : public bmw::PluginApp {
    int socket_;
    pid_t child_;
    unsigned generation_;
    bool visible_;
    uint64_t retry_at_, last_frame_, message_until_, base_;
    pt_state timer_;
    char message_[96];

    void text(unsigned model, const char* value) { updateModel(model, bmw::BasicString<char>(value)); }
    // Times as ms since the app started, through 32 bits: converting a 64-bit
    // integer to double needs an ARM EABI helper the head unit may not export.
    double ms(uint64_t t) const { return static_cast<double>(static_cast<uint32_t>(t - base_)); }
    void reap() {
        if (child_ <= 0) return;
        pid_t ended = waitpid(child_, 0, WNOHANG);
        if (ended == child_ || (ended < 0 && errno == ECHILD)) child_ = -1;
    }
    void stopChild() { reap(); if (child_ > 0) kill(child_, SIGTERM); }
    void postPoll() {
        bmw::Parcel data;
        data.writeUInt32(kPoll);
        data.writeUInt32(generation_);
        post(bmw::Message(0x40000003u, data), visible_ ? 50u : 500u);
    }

    void loadBest() {
        FILE* f = fopen(kBest, "r");
        if (!f) return;
        char line[128];
        if (fgets(line, sizeof line, f)) {
            char* p = line;
            for (unsigned i = 0; i <= PT_COUNT; ++i) {
                char* end;
                double v = strtod(p, &end);
                if (end == p || v < 0 || v > 999) break;
                if (i < PT_COUNT) timer_.best[i] = v; else timer_.trap_best = v;
                p = end;
            }
        }
        fclose(f);
    }
    void saveBest() {
        FILE* f = fopen(kBestTmp, "w");
        if (!f) return;
        bool ok = fprintf(f, "%.3f %.3f %.3f %.3f %.2f\n", timer_.best[0], timer_.best[1], timer_.best[2],
                          timer_.best[3], timer_.trap_best) > 0;
        ok = fclose(f) == 0 && ok;
        if (ok) rename(kBestTmp, kBest);
    }

    void showResults() {
        char line[128], last[48], best[48];
        for (unsigned i = 0; i < PT_COUNT; ++i) {
            text(82u + i, kNames[i]);
            if (i == PT_QUARTER) {
                if (timer_.last[i]) snprintf(last, sizeof last, "%.2f s @ %.0f mph", timer_.last[i], timer_.trap_last / PT_MPH);
                else strcpy(last, "--");
                if (timer_.best[i]) snprintf(best, sizeof best, "%.2f s @ %.0f mph", timer_.best[i], timer_.trap_best / PT_MPH);
                else strcpy(best, "--");
            } else {
                if (timer_.last[i]) snprintf(last, sizeof last, "%.2f s", timer_.last[i]); else strcpy(last, "--");
                if (timer_.best[i]) snprintf(best, sizeof best, "%.2f s", timer_.best[i]); else strcpy(best, "--");
            }
            snprintf(line, sizeof line, "Last %s     Best %s", last, best);
            text(92u + i, line);
        }
    }

    void showStatus(uint64_t now) {
        char line[128];
        bool live = last_frame_ && now - last_frame_ < 1000;
        double v = timer_.last_v;
        if (!live) {
            text(80u, "Waiting for speed from the car...   Closed course only.");
            text(81u, "Speed --");
            return;
        }
        if (now < message_until_) text(80u, message_);
        else switch (timer_.phase) {
        case PT_ARMED: text(80u, "Armed: launch when ready. Closed course only."); break;
        case PT_LAUNCH: case PT_RUNNING: text(80u, "Timing..."); break;
        default:
            text(80u, v > 0 ? "Stop for a moment to arm 0-60 / 1/4 mile. 60-130 times on its own."
                            : "Hold still to arm...");
        }
        double run = pt_elapsed(&timer_, ms(now));
        if (run > 0) snprintf(line, sizeof line, "Speed %.0f mph     Run %.1f s", v / PT_MPH, run);
        else snprintf(line, sizeof line, "Speed %.0f mph", v / PT_MPH);
        text(81u, line);
    }

    void poll(uint64_t now) {
        for (unsigned i = 0; i < 16 && socket_ >= 0; ++i) {
            drive_frame f;
            ssize_t n = recv(socket_, &f, sizeof f, 0);
            if (n <= 0) break;
            if (n != static_cast<ssize_t>(sizeof f) || !visible_ || f.sampled_ms > now ||
                f.sampled_ms < base_ || f.kph > 400) continue;
            last_frame_ = now;
            pt_sample(&timer_, ms(f.sampled_ms), f.kph < 0 ? -1.0 : static_cast<double>(f.kph));
        }
        if (timer_.updated) {
            timer_.updated = 0;
            if (timer_.new_best) {
                timer_.new_best = 0;
                saveBest();
                strcpy(message_, "New best time!");
            } else strcpy(message_, "Run recorded.");
            message_until_ = now + 5000;
            showResults();
        }
        showStatus(now);
    }

public:
    PerfTimers(bmw::IDispatcher& dispatcher, bmw::BasicString<char> name, unsigned major, unsigned minor,
               bmw::RHMIInterfaceVersion version, const bmw::Bundle& config, const bmw::Bundle& resources)
        : PluginApp(dispatcher, name, major, minor, version, config, resources),
          socket_(-1), child_(-1), generation_(0), visible_(false), retry_at_(0), last_frame_(0), message_until_(0),
          base_(bd_mono_ms()) {
        pt_init(&timer_);
        message_[0] = 0;
    }
    virtual ~PerfTimers() { shutdown(); }

    void shutdown() {
        visible_ = false;
        ++generation_;
        stopChild();
        if (socket_ >= 0) { close(socket_); socket_ = -1; }
    }

    virtual bool onInit() {
        shutdown();
        pt_init(&timer_);
        loadBest();
        socket_ = socket(AF_INET, SOCK_DGRAM, 0);
        if (socket_ >= 0) {
            struct sockaddr_in address;
            memset(&address, 0, sizeof address);
            address.sin_family = AF_INET;
            address.sin_port = htons(kTimerPort);
            address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            if (fcntl(socket_, F_SETFL, O_NONBLOCK) < 0 || fcntl(socket_, F_SETFD, FD_CLOEXEC) < 0 ||
                bind(socket_, reinterpret_cast<struct sockaddr*>(&address), sizeof address) < 0) {
                close(socket_);
                socket_ = -1;
            }
        }
        typedef bmw::Delegate<bool, const bmw::Parcel&> Action;
        Action action;
        action.m_obj = this;
        action.m_exec = &Action::execImpl<PerfTimers, &PerfTimers::visibility>;
        addHandler(100u, 11u, action);
        action.m_exec = &Action::execImpl<PerfTimers, &PerfTimers::resetBest>;
        addHandler(26u, action);
        showResults();
        showStatus(bd_mono_ms());
        if (socket_ < 0) text(80u, "Could not open the speed channel. Restart iDrive.");
        else postPoll();
        return true;
    }
    virtual bool onFinalize() { shutdown(); return true; }

    bool visibility(const bmw::Parcel& event) {
        bool visible;
        if (!event.readBool(visible) || visible == visible_) return true;
        visible_ = visible;
        if (!visible) {
            stopChild();
            last_frame_ = 0;
            pt_sample(&timer_, ms(bd_mono_ms()), -1); // cancels a run in progress
        } else retry_at_ = 0;
        return true;
    }
    bool resetBest(const bmw::Parcel&) {
        for (unsigned i = 0; i < PT_COUNT; ++i) timer_.best[i] = timer_.last[i] = 0;
        timer_.trap_best = timer_.trap_last = 0;
        saveBest();
        showResults();
        return true;
    }

    virtual bool invokeDataUpdate(unsigned key, const bmw::Parcel& data) {
        if (key != kPoll) return PluginApp::invokeDataUpdate(key, data);
        unsigned generation;
        if (!data.readUInt32(generation)) return false;
        if (generation != generation_ || socket_ < 0) return true;
        uint64_t now = bd_mono_ms();
        reap();
        if (visible_ && child_ <= 0 && now >= retry_at_) {
            char host[16];
            if (installed_gateway(host)) {
                const char* args[] = {kWorker, "--host", host, "--client", "timer", 0};
                child_ = launch(args, -1);
            } else text(80u, "Missing gateway-ip.txt. Reinstall PerfTimers.");
            retry_at_ = now + 2000;
        }
        poll(now);
        postPoll();
        return true;
    }
};

extern "C" {
PluginAppInfo pluginAppInfo = {1, "PerfTimers", "owner-development", 1, create_app<PerfTimers>, destroy_app};
}
