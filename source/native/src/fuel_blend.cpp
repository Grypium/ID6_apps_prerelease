// FuelBlend: how much E85 and pump gas to add for a target ethanol blend.
// No car access: the fuel in the tank, the current blend and the target are
// set with - / + buttons; the E85 content at the pump and the pump gas's
// ethanol are on the settings page. "I filled up" saves the result as the new
// current blend (tank full) and records the fill; the calibration page then
// works out the E85's real ethanol content from the blend measured afterwards
// (MHD ECA or a test kit). Settings live in settings.txt beside the plugin.
//
// Live tank level: while the main screen is visible the shared read-only hub
// runs as client "fuel" (OBD 2F, one request every two seconds) and the tank
// level replaces the manual setting. The last level seen is remembered, so when
// the tank has gone up by a gallon or more since then, the app reports the
// fill and "I filled up" records what really went in: the planned E85 first,
// the rest of the measured rise as pump gas.
#include "plugin_factory.h"
#include "diagnostic_worker.h"
#include "fuel_blend.h"
#include "hub_frames.h"
#include "gauge_core.h"
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/wait.h>

namespace {
const char kSettings[] = "/var/opt/hmi/data/ID5/Plugins/FuelBlend/settings.txt";
const char kSettingsTmp[] = "/var/opt/hmi/data/ID5/Plugins/FuelBlend/settings.tmp";
const char kWorker[] = "/var/opt/hmi/data/ID5/Plugins/FuelBlend/fuel-hub-qnx";
const unsigned kPoll = 0x46424c44u; // "FBLD"
const double kFillDetectGallons = 1.0;

// Gateway address written by the installer next to the worker (one line, 169.254.x.x).
bool installed_gateway(char (&host)[16]) {
    FILE* file = fopen("/var/opt/hmi/data/ID5/Plugins/FuelBlend/gateway-ip.txt", "r");
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
const double kTankGallons = 13.7; // F22 M240i: 52 litres
const char* const kEighths[] = {"Empty", "1/8", "1/4", "3/8", "1/2", "5/8", "3/4", "7/8", "Full"};

struct Setting { int value, low, high, step; };
// FILL_*: the last fill, recorded by "I filled up" (amounts in tenths of a gallon).
// LAST_TANK: the last live tank level seen (tenths of a gallon, -1 none);
// FILL_TANK: fuel in the tank before the recorded fill (tenths, -1 = use FILL_LEVEL eighths).
enum { LEVEL, CURRENT, TARGET, E85, GAS, FILL_LEVEL, FILL_BEFORE, FILL_E85, FILL_GAS, MEASURED,
       LAST_TANK, FILL_TANK, SETTING_COUNT };
const double kMinCalibrationE85 = 1.5; // gallons of E85 in the fill for a reliable answer
}  // namespace

class FuelBlend : public bmw::PluginApp {
    Setting s_[SETTING_COUNT];
    fb_plan plan_;
    int socket_;
    pid_t child_;
    unsigned generation_;
    bool visible_;
    uint64_t retry_at_, live_at_;
    double live_gallons_;   // tank level from the car
    double fill_from_;      // level before a detected fill (gallons), < 0 none
    double fill_to_;        // level when it was detected

    bool live(uint64_t now) const { return live_at_ && now - live_at_ < FUEL_STALE_MS; }
    double inTank() const { return live(bd_mono_ms()) ? live_gallons_ : kTankGallons * s_[LEVEL].value / 8.0; }
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
    void shutdown() {
        visible_ = false;
        ++generation_;
        stopChild();
        if (socket_ >= 0) { close(socket_); socket_ = -1; }
    }
    // A new live level: notice a fill since the last level seen, remember the level.
    void liveLevel(double gallons) {
        bool first = !live_at_;
        live_gallons_ = gallons;
        live_at_ = bd_mono_ms();
        int tenths = static_cast<int>(gallons * 10 + .5);
        if (first && fill_from_ < 0 && s_[LAST_TANK].value >= 0 &&
            gallons - s_[LAST_TANK].value / 10.0 >= kFillDetectGallons) {
            fill_from_ = s_[LAST_TANK].value / 10.0;
            fill_to_ = gallons;
        }
        bool changed = false;
        // Not recorded and since driven down again: forget the fill.
        if (fill_from_ >= 0 && gallons < fill_to_ - kFillDetectGallons) { fill_from_ = -1; changed = true; }
        int eighths = static_cast<int>(gallons / kTankGallons * 8 + .5);
        // Keep the pre-fill level until the fill is recorded.
        if (fill_from_ < 0 && (tenths - s_[LAST_TANK].value >= 3 || s_[LAST_TANK].value - tenths >= 3)) {
            s_[LAST_TANK].value = tenths;
            changed = true;
        }
        if (eighths != s_[LEVEL].value && eighths >= 0 && eighths <= 8) { s_[LEVEL].value = eighths; changed = true; }
        if (changed) save();
        if (first || changed) refresh();
    }

    void text(unsigned model, const char* value) { updateModel(model, bmw::BasicString<char>(value)); }

    void load() {
        FILE* f = fopen(kSettings, "r");
        if (!f) return;
        char line[160];
        if (fgets(line, sizeof line, f)) {
            char* p = line;
            for (unsigned i = 0; i < SETTING_COUNT; ++i) {
                char* end;
                double v = strtod(p, &end);
                if (end == p) break;
                int n = static_cast<int>(v);
                if (n >= s_[i].low && n <= s_[i].high) s_[i].value = n;
                p = end;
            }
        }
        fclose(f);
    }
    void save() {
        FILE* f = fopen(kSettingsTmp, "w");
        if (!f) return;
        bool ok = true;
        for (unsigned i = 0; i < SETTING_COUNT; ++i) ok = fprintf(f, i ? " %d" : "%d", s_[i].value) > 0 && ok;
        ok = fputs("\n", f) >= 0 && ok;
        ok = fclose(f) == 0 && ok;
        if (ok) rename(kSettingsTmp, kSettings);
    }

    void refresh() {
        char line[160];
        double in_tank = inTank();
        // After a detected fill, the plan is the one for the tank as it was before it.
        double plan_tank = fill_from_ >= 0 ? fill_from_ : in_tank;
        plan_ = fb_solve(kTankGallons, plan_tank, s_[CURRENT].value, s_[TARGET].value, s_[E85].value, s_[GAS].value);
        int result = static_cast<int>(plan_.result + 0.5);
        switch (plan_.status) {
        case FB_EXACT:
            if (plan_.e85_amount < 0.05)
                snprintf(line, sizeof line, "Fill with %d-octane only (%.1f gal) for E%d", 91, plan_.gas_amount, result);
            else if (plan_.gas_amount < 0.05)
                snprintf(line, sizeof line, "Fill with E85 only (%.1f gal) for E%d", plan_.e85_amount, result);
            else
                snprintf(line, sizeof line, "Pump %.1f gal of E85 first, then fill with 91", plan_.e85_amount);
            break;
        case FB_TOO_LOW:
            snprintf(line, sizeof line, "Already above E%d: fill with 91 only (%.1f gal) for E%d",
                     s_[TARGET].value, plan_.gas_amount, result);
            break;
        case FB_TOO_HIGH:
            snprintf(line, sizeof line, "Not enough room for E%d: fill with E85 only (%.1f gal) for E%d",
                     s_[TARGET].value, plan_.e85_amount, result);
            break;
        default:
            snprintf(line, sizeof line, "Check settings: E85 content must be above the pump gas's");
        }
        if (fill_from_ >= 0)
            snprintf(line, sizeof line, "Fill detected: %.1f to %.1f gal. Press \"I filled up\" to record it.",
                     fill_from_, in_tank);
        text(80u, line);
        if (live(bd_mono_ms()))
            snprintf(line, sizeof line, "Fuel in tank: %.1f gal (%.0f%%), from the car", in_tank, in_tank / kTankGallons * 100);
        else snprintf(line, sizeof line, "Fuel in tank: %s (%.1f gal)", kEighths[s_[LEVEL].value], in_tank);
        text(81u, line);
        snprintf(line, sizeof line, "Blend in tank now: E%d", s_[CURRENT].value);
        text(82u, line);
        snprintf(line, sizeof line, "Target blend: E%d", s_[TARGET].value);
        text(83u, line);
        if (plan_.status == FB_INVALID) line[0] = 0;
        else snprintf(line, sizeof line, "Result: E%d   %.1f gal E85 + %.1f gal 91 = %.1f gal",
                      result, plan_.e85_amount, plan_.gas_amount, plan_.e85_amount + plan_.gas_amount);
        text(84u, line);
        snprintf(line, sizeof line, "E85 at the pump really is: E%d", s_[E85].value);
        text(86u, line);
        snprintf(line, sizeof line, "Ethanol in your 91: E%d", s_[GAS].value);
        text(87u, line);
        snprintf(line, sizeof line, "Tank size %.1f gal (F22 M240i). Calibrate after a fill to learn your E85.", kTankGallons);
        text(88u, line);
        refreshCalibration();
    }

    bool calibrated(double& e85) const {
        double before = s_[FILL_TANK].value >= 0 ? s_[FILL_TANK].value / 10.0 : kTankGallons * s_[FILL_LEVEL].value / 8.0;
        return fb_calibrate(before, s_[FILL_BEFORE].value,
                            s_[FILL_E85].value / 10.0, s_[FILL_GAS].value / 10.0, s_[GAS].value,
                            s_[MEASURED].value, kMinCalibrationE85, &e85) == FB_EXACT;
    }
    void refreshCalibration() {
        char line[160];
        bool recorded = s_[FILL_E85].value || s_[FILL_GAS].value;
        if (recorded && s_[FILL_TANK].value >= 0)
            snprintf(line, sizeof line, "Last fill: %.1f gal at E%d before. Set what the pump showed and the blend measured.",
                     s_[FILL_TANK].value / 10.0, s_[FILL_BEFORE].value);
        else if (recorded)
            snprintf(line, sizeof line, "Last fill: %s tank at E%d. Set what the pump showed and the blend measured.",
                     kEighths[s_[FILL_LEVEL].value], s_[FILL_BEFORE].value);
        else snprintf(line, sizeof line, "Press \"I filled up\" after a fill, then measure the blend after a short drive.");
        text(89u, line);
        snprintf(line, sizeof line, "E85 pumped: %.1f gal", s_[FILL_E85].value / 10.0);
        text(90u, line);
        snprintf(line, sizeof line, "91 pumped: %.1f gal", s_[FILL_GAS].value / 10.0);
        text(91u, line);
        snprintf(line, sizeof line, "Measured blend now: E%d", s_[MEASURED].value);
        text(92u, line);
        double e85;
        if (!recorded) line[0] = 0;
        else if (s_[FILL_E85].value < kMinCalibrationE85 * 10)
            snprintf(line, sizeof line, "Too little E85 in this fill to tell (need %.1f gal or more)", kMinCalibrationE85);
        else if (!calibrated(e85))
            snprintf(line, sizeof line, "These numbers don't add up. Check the amounts and the measured blend.");
        else
            snprintf(line, sizeof line, "Your E85 works out to E%.0f (setting is E%d)", e85, s_[E85].value);
        text(93u, line);
    }

    bool applyCalibration(const bmw::Parcel&) {
        double e85;
        if (!(s_[FILL_E85].value || s_[FILL_GAS].value) || !calibrated(e85)) return true;
        int v = static_cast<int>(e85 + 0.5);
        if (v < s_[E85].low) v = s_[E85].low;
        if (v > s_[E85].high) v = s_[E85].high;
        s_[E85].value = v;
        s_[CURRENT].value = s_[MEASURED].value; // the measurement is the truth for the tank now
        save();
        refresh();
        return true;
    }

    void change(unsigned which, int direction) {
        if (which == LEVEL && live(bd_mono_ms())) return; // the car's level wins
        Setting& s = s_[which];
        int v = s.value + direction * s.step;
        if (v < s.low) v = s.low;
        if (v > s.high) v = s.high;
        if (v == s.value) return;
        s.value = v;
        save();
        refresh();
    }

    // C = setting * 2 + (1 for +, 0 for -): one template argument keeps the binding macro simple.
    template <unsigned C> bool step(const bmw::Parcel&) { change(C >> 1, (C & 1u) ? 1 : -1); return true; }

    bool filledUp(const bmw::Parcel&) {
        if (plan_.status == FB_INVALID) return true;
        const bool measured = fill_from_ >= 0 && live(bd_mono_ms());
        double before = measured ? fill_from_ : inTank();
        double e85 = plan_.e85_amount, gas = plan_.gas_amount, after = kTankGallons;
        if (measured) { // the planned E85 went in first; the rest of the measured rise was pump gas
            after = live_gallons_;
            if (e85 > after - before) e85 = after - before;
            gas = after - before - e85;
        }
        double result = after > 0 ? (before * s_[CURRENT].value + e85 * s_[E85].value + gas * s_[GAS].value) / after
                                  : plan_.result;
        s_[FILL_LEVEL].value = static_cast<int>(before / kTankGallons * 8 + .5);
        s_[FILL_TANK].value = static_cast<int>(before * 10 + .5);
        s_[FILL_BEFORE].value = s_[CURRENT].value;
        s_[FILL_E85].value = static_cast<int>(e85 * 10 + 0.5);
        s_[FILL_GAS].value = static_cast<int>(gas * 10 + 0.5);
        s_[MEASURED].value = static_cast<int>(result + 0.5);
        s_[CURRENT].value = static_cast<int>(result + 0.5);
        s_[LEVEL].value = measured ? static_cast<int>(after / kTankGallons * 8 + .5) : 8;
        if (measured) s_[LAST_TANK].value = static_cast<int>(after * 10 + .5);
        fill_from_ = -1;
        save();
        refresh();
        return true;
    }

    bool visibility(const bmw::Parcel& event) {
        bool visible;
        if (!event.readBool(visible) || visible == visible_) return true;
        visible_ = visible;
        if (!visible) { stopChild(); live_at_ = 0; refresh(); }
        else retry_at_ = 0;
        return true;
    }

public:
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
                const char* args[] = {kWorker, "--host", host, "--client", "fuel", 0};
                child_ = launch(args, -1);
            }
            retry_at_ = now + 2000;
        }
        for (unsigned i = 0; i < 8; ++i) {
            fuel_frame f;
            ssize_t n = recv(socket_, &f, sizeof f, 0);
            if (n <= 0) break;
            if (n == static_cast<ssize_t>(sizeof f) && visible_ && f.sampled_ms <= now &&
                f.level != HUB_NONE && f.level >= 0 && f.level <= 1000)
                liveLevel(kTankGallons * f.level / 1000.0);
        }
        bool was_live = live_at_ != 0;
        if (was_live && !live(now)) { live_at_ = 0; refresh(); }
        postPoll();
        return true;
    }

    FuelBlend(bmw::IDispatcher& dispatcher, bmw::BasicString<char> name, unsigned major, unsigned minor,
              bmw::RHMIInterfaceVersion version, const bmw::Bundle& config, const bmw::Bundle& resources)
        : PluginApp(dispatcher, name, major, minor, version, config, resources),
          socket_(-1), child_(-1), generation_(0), visible_(false), retry_at_(0), live_at_(0),
          live_gallons_(0), fill_from_(-1), fill_to_(0) {
        const Setting defaults[SETTING_COUNT] = {{2, 0, 8, 1}, {10, 0, 100, 1}, {30, 0, 85, 5},
                                                 {80, 40, 100, 1}, {10, 0, 15, 5},
                                                 {0, 0, 8, 1}, {0, 0, 100, 1}, {0, 0, 140, 1}, {0, 0, 140, 1},
                                                 {0, 0, 100, 1}, {-1, -1, 140, 1}, {-1, -1, 140, 1}};
        for (unsigned i = 0; i < SETTING_COUNT; ++i) s_[i] = defaults[i];
        plan_.status = FB_INVALID;
    }

    virtual ~FuelBlend() { shutdown(); }
    virtual bool onInit() {
        shutdown();
        socket_ = socket(AF_INET, SOCK_DGRAM, 0);
        if (socket_ >= 0) {
            struct sockaddr_in address;
            memset(&address, 0, sizeof address);
            address.sin_family = AF_INET;
            address.sin_port = htons(FUEL_PORT);
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
        action.m_exec = &Action::execImpl<FuelBlend, &FuelBlend::visibility>;
        addHandler(100u, 11u, action);
#define FB_BIND(id, fn) action.m_exec = &Action::execImpl<FuelBlend, &FuelBlend::fn>; addHandler(id, action)
        FB_BIND(21u, step<LEVEL * 2>);   FB_BIND(22u, step<LEVEL * 2 + 1>);
        FB_BIND(23u, step<CURRENT * 2>); FB_BIND(24u, step<CURRENT * 2 + 1>);
        FB_BIND(25u, step<TARGET * 2>);  FB_BIND(26u, step<TARGET * 2 + 1>);
        FB_BIND(28u, step<E85 * 2>);     FB_BIND(29u, step<E85 * 2 + 1>);
        FB_BIND(30u, step<GAS * 2>);     FB_BIND(31u, step<GAS * 2 + 1>);
        FB_BIND(32u, step<FILL_E85 * 2>); FB_BIND(33u, step<FILL_E85 * 2 + 1>);
        FB_BIND(34u, step<FILL_GAS * 2>); FB_BIND(35u, step<FILL_GAS * 2 + 1>);
        FB_BIND(36u, step<MEASURED * 2>); FB_BIND(37u, step<MEASURED * 2 + 1>);
        FB_BIND(38u, applyCalibration);
        FB_BIND(27u, filledUp);
#undef FB_BIND
        load();
        refresh();
        if (socket_ >= 0) postPoll();
        return true;
    }
    virtual bool onFinalize() { shutdown(); return true; }
};

extern "C" {
PluginAppInfo pluginAppInfo = {1, "FuelBlend", "owner-development", 1, create_app<FuelBlend>, destroy_app};
}
