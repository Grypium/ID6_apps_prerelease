// ExhaustFlap: Auto / Open / Closed through the MHD flash's own flap control
// (2F 02 nn: 00 = the car controls it, 01 = open, 02 = closed). One request per
// press, through exhaust-flap-qnx (collector.c, -DMHD_MAPS_ONLY -DPERF_LAB),
// which takes the shared diagnostic lock so the gauge hub steps aside.
// MHD has no read-back of the flap, so the screen shows what was last accepted.
#include "plugin_factory.h"
#include "diagnostic_worker.h"
#include <arpa/inet.h>
#include <stdio.h>
#include <sys/wait.h>

namespace {
const unsigned kPoll = 0x464c4150u; // "FLAP"
const char kWorker[] = "/var/opt/hmi/data/ID5/Plugins/ExhaustFlap/exhaust-flap-qnx";
const char* const kModes[] = {"Auto (the car controls it)", "Open", "Closed"};

bool installed_gateway(char (&host)[16]) {
    FILE* file = fopen("/var/opt/hmi/data/ID5/Plugins/ExhaustFlap/gateway-ip.txt", "r");
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

class ExhaustFlap : public bmw::PluginApp {
    pid_t child_;
    int pipe_;
    unsigned generation_, pending_;
    int accepted_; // last mode MHD accepted, -1 = none this session
    char result_[128];
    size_t used_;

    void text(unsigned model, const char* value) { updateModel(model, bmw::BasicString<char>(value)); }
    void reap() {
        if (child_ <= 0) return;
        pid_t ended = waitpid(child_, 0, WNOHANG);
        if (ended == child_ || (ended < 0 && errno == ECHILD)) child_ = -1;
    }
    void stopWorker() {
        ++generation_;
        if (pipe_ >= 0) { close(pipe_); pipe_ = -1; }
        reap();
        if (child_ > 0) kill(child_, SIGTERM);
    }
    void postPoll() {
        bmw::Parcel data;
        data.writeUInt32(kPoll);
        data.writeUInt32(generation_);
        post(bmw::Message(0x40000003u, data), 100u);
    }
    void showState() {
        char line[96];
        if (accepted_ < 0) snprintf(line, sizeof line, "Flap: not changed by this app since iDrive started");
        else snprintf(line, sizeof line, "Flap: %s", kModes[accepted_]);
        text(81u, line);
    }

    template <unsigned Mode> bool select(const bmw::Parcel&) {
        reap();
        if (pipe_ >= 0 || child_ > 0) return true;
        char host[16];
        if (!installed_gateway(host)) { text(80u, "Missing gateway-ip.txt. Reinstall ExhaustFlap."); return true; }
        ++generation_;
        int channel[2];
        if (pipe(channel) < 0) { text(80u, "Could not start the request"); return true; }
        const char argument[] = {static_cast<char>('0' + Mode), 0};
        const char* args[] = {kWorker, "--host", host, "--flap", argument, 0};
        if (fcntl(channel[0], F_SETFL, O_NONBLOCK) >= 0 && fcntl(channel[0], F_SETFD, FD_CLOEXEC) >= 0 &&
            fcntl(channel[1], F_SETFD, FD_CLOEXEC) >= 0)
            child_ = launch(args, channel[1]);
        close(channel[1]);
        if (child_ < 0) { close(channel[0]); text(80u, "Could not start the request"); return true; }
        pipe_ = channel[0];
        used_ = 0;
        pending_ = Mode;
        text(80u, "Sending...");
        postPoll();
        return true;
    }

public:
    ExhaustFlap(bmw::IDispatcher& d, bmw::BasicString<char> name, unsigned maj, unsigned min,
                bmw::RHMIInterfaceVersion version, const bmw::Bundle& cfg, const bmw::Bundle& res)
        : PluginApp(d, name, maj, min, version, cfg, res), child_(-1), pipe_(-1), generation_(0),
          pending_(0), accepted_(-1), used_(0) {}
    virtual ~ExhaustFlap() { stopWorker(); }
    virtual bool onFinalize() { stopWorker(); return true; }
    virtual bool onInit() {
        stopWorker();
        typedef bmw::Delegate<bool, const bmw::Parcel&> Action;
        Action action;
        action.m_obj = this;
        action.m_exec = &Action::execImpl<ExhaustFlap, &ExhaustFlap::select<0> >;
        addHandler(21u, action);
        action.m_exec = &Action::execImpl<ExhaustFlap, &ExhaustFlap::select<1> >;
        addHandler(22u, action);
        action.m_exec = &Action::execImpl<ExhaustFlap, &ExhaustFlap::select<2> >;
        addHandler(23u, action);
        text(80u, "Through your MHD tune. Auto hands the flap back to the car.");
        showState();
        return true;
    }
    virtual bool invokeDataUpdate(unsigned key, const bmw::Parcel& data) {
        if (key != kPoll) return PluginApp::invokeDataUpdate(key, data);
        unsigned generation;
        if (!data.readUInt32(generation)) return false;
        if (generation != generation_) return true;
        if (pipe_ >= 0) {
            ssize_t n = read(pipe_, result_ + used_, sizeof result_ - 1 - used_);
            if (n > 0) used_ += static_cast<size_t>(n);
            else if (n == 0 || (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK)) {
                close(pipe_);
                pipe_ = -1;
                while (used_ && (result_[used_ - 1] == '\n' || result_[used_ - 1] == '\r')) --used_;
                result_[used_] = 0;
                if (!strncmp(result_, "Accepted by DME", 15)) {
                    accepted_ = static_cast<int>(pending_);
                    text(80u, "Accepted by your MHD tune.");
                } else text(80u, used_ ? result_ : "No result from the diagnostic worker.");
                showState();
            }
        }
        reap();
        if (pipe_ >= 0 || child_ > 0) postPoll();
        return true;
    }
};

extern "C" {
PluginAppInfo pluginAppInfo = {1, "ExhaustFlap", "owner-development", 1, create_app<ExhaustFlap>, destroy_app};
}
