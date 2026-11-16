// EngineTemps: oil, coolant, transmission and intake air temperatures, with a
// warm-up guide based on oil temperature.
//
// While the screen is visible it runs the shared diagnostic hub as client
// "temps" (read-only, shared with the gauge apps, one gateway connection):
// one DME request a second (oil 4408, coolant 4300, intake air 580F) plus the
// transmission temperature (EGS DA12) every two seconds, sent to UDP 42866.
#include "plugin_factory.h"
#include "diagnostic_worker.h"
#include "engine_temps.h"
#include "temps_frame.h"
#include "gauge_core.h"
#include <arpa/inet.h>
#include <stdio.h>
#include <sys/socket.h>
#include <sys/wait.h>

namespace {
const unsigned kPoll = 0x544d5053u; // "TMPS"
const char kWorker[] = "/var/opt/hmi/data/ID5/Plugins/EngineTemps/temps-hub-qnx";
const char* const kNames[4] = {"Oil", "Coolant", "Transmission", "Intake air"};

bool installed_gateway(char (&host)[16]) {
    FILE* file = fopen("/var/opt/hmi/data/ID5/Plugins/EngineTemps/gateway-ip.txt", "r");
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

class EngineTemps : public bmw::PluginApp {
    int socket_;
    pid_t child_;
    unsigned generation_;
    bool visible_;
    uint64_t retry_at_, last_frame_;
    temps_frame last_;

    void text(unsigned model, const char* value) { updateModel(model, bmw::BasicString<char>(value)); }
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
        post(bmw::Message(0x40000003u, data), visible_ ? 250u : 1000u);
    }

    void show(uint64_t now) {
        bool live = last_frame_ && now - last_frame_ < TEMPS_STALE_MS;
        const int32_t values[4] = {last_.oil, last_.coolant, last_.trans, last_.intake};
        char line[96];
        for (unsigned i = 0; i < 4; ++i) {
            text(82u + i, kNames[i]);
            if (!live || values[i] == TEMPS_NONE) strcpy(line, "--");
            else {
                double c = values[i] / 10.0;
                snprintf(line, sizeof line, "%.0f F   (%.0f C)", et_fahrenheit(c), c);
            }
            text(92u + i, line);
        }
        bool oil_ok = live && last_.oil != TEMPS_NONE;
        text(80u, et_guide(et_oil_stage(oil_ok, last_.oil / 10.0)));
        // Bottom line: the first heat warning, else what the guide is based on.
        if (live && last_.trans != TEMPS_NONE && last_.trans / 10.0 >= ET_TRANS_HOT)
            text(81u, "Transmission hot: ease off and let it cool.");
        else if (live && last_.coolant != TEMPS_NONE && last_.coolant / 10.0 >= ET_COOLANT_HOT)
            text(81u, "Coolant hot: ease off and let it cool.");
        else if (live && last_.intake != TEMPS_NONE && last_.intake / 10.0 >= ET_INTAKE_HOT)
            text(81u, "Intake air hot (heat soak): expect less power until it cools.");
        else if (oil_ok)
            text(81u, "Warm-up guide follows oil temperature. Full performance from 194 F (90 C).");
        else text(81u, "");
    }

    void poll(uint64_t now) {
        for (unsigned i = 0; i < 8 && socket_ >= 0; ++i) {
            temps_frame f;
            ssize_t n = recv(socket_, &f, sizeof f, 0);
            if (n <= 0) break;
            if (n != static_cast<ssize_t>(sizeof f) || !visible_) continue;
            last_ = f;
            last_frame_ = now;
        }
        show(now);
    }

public:
    EngineTemps(bmw::IDispatcher& dispatcher, bmw::BasicString<char> name, unsigned major, unsigned minor,
                bmw::RHMIInterfaceVersion version, const bmw::Bundle& config, const bmw::Bundle& resources)
        : PluginApp(dispatcher, name, major, minor, version, config, resources),
          socket_(-1), child_(-1), generation_(0), visible_(false), retry_at_(0), last_frame_(0) {
        last_.sampled_ms = 0;
        last_.oil = last_.coolant = last_.intake = last_.trans = TEMPS_NONE;
    }
    virtual ~EngineTemps() { shutdown(); }

    void shutdown() {
        visible_ = false;
        ++generation_;
        stopChild();
        if (socket_ >= 0) { close(socket_); socket_ = -1; }
    }

    virtual bool onInit() {
        shutdown();
        socket_ = socket(AF_INET, SOCK_DGRAM, 0);
        if (socket_ >= 0) {
            struct sockaddr_in address;
            memset(&address, 0, sizeof address);
            address.sin_family = AF_INET;
            address.sin_port = htons(TEMPS_PORT);
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
        action.m_exec = &Action::execImpl<EngineTemps, &EngineTemps::visibility>;
        addHandler(100u, 11u, action);
        show(bd_mono_ms());
        if (socket_ < 0) text(80u, "Could not open the data channel. Restart iDrive.");
        else postPoll();
        return true;
    }
    virtual bool onFinalize() { shutdown(); return true; }

    bool visibility(const bmw::Parcel& event) {
        bool visible;
        if (!event.readBool(visible) || visible == visible_) return true;
        visible_ = visible;
        if (!visible) { stopChild(); last_frame_ = 0; }
        else retry_at_ = 0;
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
                const char* args[] = {kWorker, "--host", host, "--client", "temps", 0};
                child_ = launch(args, -1);
            } else text(80u, "Missing gateway-ip.txt. Reinstall EngineTemps.");
            retry_at_ = now + 2000;
        }
        poll(now);
        postPoll();
        return true;
    }
};

extern "C" {
PluginAppInfo pluginAppInfo = {1, "EngineTemps", "owner-development", 1, create_app<EngineTemps>, destroy_app};
}
