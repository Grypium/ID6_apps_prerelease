// FuelBlend: how much E85 and pump gas to add for a target ethanol blend.
// No car access: the fuel in the tank, the current blend and the target are
// set with - / + buttons; the E85 content at the pump and the pump gas's
// ethanol are on the settings page. "I filled up" saves the result as the new
// current blend (tank full) and records the fill; the calibration page then
// works out the E85's real ethanol content from the blend measured afterwards
// (MHD ECA or a test kit). Settings live in settings.txt beside the plugin.
#include "plugin_factory.h"
#include "fuel_blend.h"
#include <stdio.h>
#include <stdlib.h>

namespace {
const char kSettings[] = "/var/opt/hmi/data/ID5/Plugins/FuelBlend/settings.txt";
const char kSettingsTmp[] = "/var/opt/hmi/data/ID5/Plugins/FuelBlend/settings.tmp";
const double kTankGallons = 13.7; // F22 M240i: 52 litres
const char* const kEighths[] = {"Empty", "1/8", "1/4", "3/8", "1/2", "5/8", "3/4", "7/8", "Full"};

struct Setting { int value, low, high, step; };
// FILL_*: the last fill, recorded by "I filled up" (amounts in tenths of a gallon).
enum { LEVEL, CURRENT, TARGET, E85, GAS, FILL_LEVEL, FILL_BEFORE, FILL_E85, FILL_GAS, MEASURED, SETTING_COUNT };
const double kMinCalibrationE85 = 1.5; // gallons of E85 in the fill for a reliable answer
}  // namespace

class FuelBlend : public bmw::PluginApp {
    Setting s_[SETTING_COUNT];
    fb_plan plan_;

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
        double in_tank = kTankGallons * s_[LEVEL].value / 8.0;
        plan_ = fb_solve(kTankGallons, in_tank, s_[CURRENT].value, s_[TARGET].value, s_[E85].value, s_[GAS].value);
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
        text(80u, line);
        snprintf(line, sizeof line, "Fuel in tank: %s (%.1f gal)", kEighths[s_[LEVEL].value], in_tank);
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
        return fb_calibrate(kTankGallons * s_[FILL_LEVEL].value / 8.0, s_[FILL_BEFORE].value,
                            s_[FILL_E85].value / 10.0, s_[FILL_GAS].value / 10.0, s_[GAS].value,
                            s_[MEASURED].value, kMinCalibrationE85, &e85) == FB_EXACT;
    }
    void refreshCalibration() {
        char line[160];
        bool recorded = s_[FILL_E85].value || s_[FILL_GAS].value;
        if (recorded)
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
        s_[FILL_LEVEL].value = s_[LEVEL].value;
        s_[FILL_BEFORE].value = s_[CURRENT].value;
        s_[FILL_E85].value = static_cast<int>(plan_.e85_amount * 10 + 0.5);
        s_[FILL_GAS].value = static_cast<int>(plan_.gas_amount * 10 + 0.5);
        s_[MEASURED].value = static_cast<int>(plan_.result + 0.5);
        s_[CURRENT].value = static_cast<int>(plan_.result + 0.5);
        s_[LEVEL].value = 8;
        save();
        refresh();
        return true;
    }

public:
    FuelBlend(bmw::IDispatcher& dispatcher, bmw::BasicString<char> name, unsigned major, unsigned minor,
              bmw::RHMIInterfaceVersion version, const bmw::Bundle& config, const bmw::Bundle& resources)
        : PluginApp(dispatcher, name, major, minor, version, config, resources) {
        const Setting defaults[SETTING_COUNT] = {{2, 0, 8, 1}, {10, 0, 100, 1}, {30, 0, 85, 5},
                                                 {80, 40, 100, 1}, {10, 0, 15, 5},
                                                 {0, 0, 8, 1}, {0, 0, 100, 1}, {0, 0, 140, 1}, {0, 0, 140, 1},
                                                 {0, 0, 100, 1}};
        for (unsigned i = 0; i < SETTING_COUNT; ++i) s_[i] = defaults[i];
        plan_.status = FB_INVALID;
    }

    virtual bool onInit() {
        typedef bmw::Delegate<bool, const bmw::Parcel&> Action;
        Action action;
        action.m_obj = this;
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
        return true;
    }
    virtual bool onFinalize() { return true; }
};

extern "C" {
PluginAppInfo pluginAppInfo = {1, "FuelBlend", "owner-development", 1, create_app<FuelBlend>, destroy_app};
}
