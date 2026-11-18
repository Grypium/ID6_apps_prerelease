// Native NBT EVO RA plugin. Models are touched only on the HMI dispatcher.
// Network diagnostics run in the separate collector process, never in HmiMain3D.
#include "plugin_factory.h"
#include "gauge_core.h"
#include "dial_scales.h"
#include "diagnostic_worker.h"
#include "hub_frames.h"
#include "ethanol_source.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#if defined(__arm__)
// C++98 target ABI gates. Host builds have 64-bit pointers and cannot prove these.
typedef char bd_pointer_size[(sizeof(void*)==4) ? 1 : -1];
typedef char bd_parcel_size[(sizeof(bmw::Parcel)==20) ? 1 : -1];
typedef char bd_message_size[(sizeof(bmw::Message)==80 && __alignof__(bmw::Message)==8) ? 1 : -1];
typedef char bd_std_string_size[(sizeof(std::string)==4) ? 1 : -1];
typedef char bd_delegate_size[(sizeof(bmw::Delegate<bool,const bmw::Parcel&>)==8) ? 1 : -1];
#endif

namespace {
// In the installed AppFramework, this message reads a UInt32 key from its
// Parcel and dispatches through the primary invokeDataUpdate virtual.
const unsigned kDataUpdateMessage = 0x40000003u;
const unsigned kPollKey = 0x4244504cu; // BDPL, private to this app
const unsigned kActivePollIntervalMs = 25u;
const unsigned kIdlePollIntervalMs = 250u;
const char kCollector[] = "/var/opt/hmi/data/ID5/Plugins/BoostDeck/boostdeck-collector-qnx";
const char kGatewayFile[] = "/var/opt/hmi/data/ID5/Plugins/BoostDeck/gateway-ip.txt";
// Opt-in: the ethanol read runs only when this file exists; its first word picks
// the source (obd, mhd or auto; see ethanol_source.h).
const char kEthanolFile[] = "/var/opt/hmi/data/ID5/Plugins/BoostDeck/ethanol-on";

// Gateway address written by the installer next to the worker (one line, 169.254.x.x).
bool installed_gateway(char (&host)[16]) {
    FILE* file = fopen(kGatewayFile, "r");
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

// Third row (hub "deck" frames). Bars run over these imperial ranges.
enum { kExtraCount = 4 };
const struct ExtraScale { double minimum, maximum; } kExtraScales[kExtraCount] = {
    {0, 500},   // power, hp
    {0, 500},   // torque, lb-ft
    {0, 100},   // throttle, %
    {100, 300}  // oil temperature, F
};
const unsigned kExtraLabelModel = 70u, kExtraBarModel = 74u, kExtraBarComponent = 210u;
const unsigned kBarCount = sizeof(kDialScales)/sizeof(kDialScales[0]);
#ifdef PERF_LAB
typedef char dial_count_check[kBarCount + 1 == BD_COUNT ? 1 : -1];
const unsigned kEthanolFirst = 56100u;
const unsigned kEthanolSteps = 20u;
#else
typedef char dial_count_check[kBarCount == BD_COUNT ? 1 : -1];
#endif
}

class BoostDeck : public bmw::PluginApp {
    int fd_;
    unsigned poll_generation_;
    bd_frame frame_;
    uint64_t received_;
    pid_t collector_;
    bool stopping_;
    bool gauges_visible_;
    unsigned color_;
#ifdef PERF_LAB
    bool metric_;
#endif
    uint64_t retry_at_;
    void startVisibleCollector();
    void selectColor(unsigned color);
    char labels_[BD_COUNT][128];
    unsigned images_[BD_COUNT];
    int deck_fd_;
    deck_frame deck_;
    uint64_t deck_at_;
    char extra_labels_[kExtraCount][48];
    unsigned extra_images_[kExtraCount];
#ifdef PERF_LAB
    // Session peaks: the highest value each tile reached since BoostDeck was
    // opened (a short hide, like the options menu, keeps them).
    bool show_max_;
    double peak_[kBarCount], extra_peak_[kExtraCount];
    bool has_peak_[kBarCount], has_extra_peak_[kExtraCount];
    uint64_t hidden_at_;
    void resetPeaks() {
        for (unsigned i=0;i<kBarCount;++i) has_peak_[i]=false;
        for (unsigned i=0;i<kExtraCount;++i) has_extra_peak_[i]=false;
    }
    template <bool Max> bool showMax(const bmw::Parcel&) { show_max_=Max; refresh(); return true; }
    bool clearPeaks(const bmw::Parcel&) { resetPeaks(); refresh(); return true; }
#endif
    void postPoll();
    void refreshExtras();
public:
    BoostDeck(bmw::IDispatcher& d, bmw::BasicString<char> name, unsigned maj, unsigned mn,
              bmw::RHMIInterfaceVersion rhmi, const bmw::Bundle& cfg, const bmw::Bundle& res)
        : bmw::PluginApp(d,name,maj,mn,rhmi,cfg,res), fd_(-1), poll_generation_(0),
          received_(0), collector_(-1), stopping_(false), gauges_visible_(false),
          color_(0),
#ifdef PERF_LAB
          metric_(false),
#endif
          retry_at_(0), deck_fd_(-1) {
#ifdef PERF_LAB
        show_max_=false; hidden_at_=0;
        resetPeaks();
#endif
        resetDisplay();
    }
    virtual ~BoostDeck() { shutdown(); }
    virtual bool onInit();
    virtual bool onFinalize() { shutdown(); return true; }
    virtual bool invokeDataUpdate(unsigned key, const bmw::Parcel& data);
    bool gaugeVisibility(const bmw::Parcel&);
    template <unsigned Color> bool changeColor(const bmw::Parcel&) {
        selectColor(Color);
        return true;
    }
#ifdef PERF_LAB
    template <bool Metric> bool changeUnits(const bmw::Parcel&) {
        metric_=Metric;
        refresh();
        return true;
    }
#endif
    void reapCollector() {
        if (collector_ <= 0) return;
        pid_t ended = waitpid(collector_, 0, WNOHANG);
        if (ended == collector_ || (ended < 0 && errno == ECHILD)) {
            collector_ = -1;
            stopping_ = false;
        }
    }
    void stopCollector() {
        reapCollector();
        if (collector_ > 0 && !stopping_) {
            kill(collector_, SIGTERM);
            stopping_ = true;
        }
    }
    void shutdown() {
        gauges_visible_=false;
        stopCollector();
        if (fd_>=0) { close(fd_); fd_=-1; }
        if (deck_fd_>=0) { close(deck_fd_); deck_fd_=-1; }
    }
    void resetDisplay() {
        memset(&frame_,0,sizeof frame_); memset(labels_,0,sizeof labels_);
        memset(images_,0,sizeof images_);
        memset(&deck_,0,sizeof deck_); memset(extra_labels_,0,sizeof extra_labels_);
        memset(extra_images_,0,sizeof extra_images_);
        received_=0; deck_at_=0;
    }
    void refresh();
    void poll();
};

bool BoostDeck::onInit() {
    shutdown();
    resetDisplay();
    struct sockaddr_in a;
    fd_=socket(AF_INET,SOCK_DGRAM,0);
    memset(&a,0,sizeof a); a.sin_family=AF_INET; a.sin_port=htons(BD_UDP_PORT);
    a.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    if (fd_>=0 && (fcntl(fd_,F_SETFL,O_NONBLOCK)<0 || fcntl(fd_,F_SETFD,FD_CLOEXEC)<0 ||
        bind(fd_,reinterpret_cast<struct sockaddr*>(&a),sizeof a)<0)) {
        close(fd_); fd_=-1;
    }
    deck_fd_=socket(AF_INET,SOCK_DGRAM,0);
    memset(&a,0,sizeof a); a.sin_family=AF_INET; a.sin_port=htons(DECK_PORT);
    a.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    if (deck_fd_>=0 && (fcntl(deck_fd_,F_SETFL,O_NONBLOCK)<0 || fcntl(deck_fd_,F_SETFD,FD_CLOEXEC)<0 ||
        bind(deck_fd_,reinterpret_cast<struct sockaddr*>(&a),sizeof a)<0)) {
        close(deck_fd_); deck_fd_=-1;
    }
    bmw::Delegate<bool,const bmw::Parcel&> action;
    action.m_obj=this;
    // BMW sends event 11 with a bool when this HMI state becomes visible or hidden.
    action.m_exec=&bmw::Delegate<bool,const bmw::Parcel&>::execImpl<BoostDeck,&BoostDeck::gaugeVisibility>;
    addHandler(100u,11u,action);
    typedef bmw::Delegate<bool,const bmw::Parcel&> Action;
    typedef bool (*Handler)(void*,const bmw::Parcel&);
    static const Handler colorHandlers[] = {
        &Action::execImpl<BoostDeck,&BoostDeck::changeColor<0> >,
        &Action::execImpl<BoostDeck,&BoostDeck::changeColor<1> >,
        &Action::execImpl<BoostDeck,&BoostDeck::changeColor<2> >,
        &Action::execImpl<BoostDeck,&BoostDeck::changeColor<3> >,
        &Action::execImpl<BoostDeck,&BoostDeck::changeColor<4> >,
        &Action::execImpl<BoostDeck,&BoostDeck::changeColor<5> >,
        &Action::execImpl<BoostDeck,&BoostDeck::changeColor<6> >,
        &Action::execImpl<BoostDeck,&BoostDeck::changeColor<7> >,
        &Action::execImpl<BoostDeck,&BoostDeck::changeColor<kMulticolor> >
    };
    typedef char color_handler_count[(sizeof colorHandlers/sizeof colorHandlers[0] ==
                                      sizeof kColorNames/sizeof kColorNames[0]) ? 1 : -1];
    (void)sizeof(color_handler_count);
    for (unsigned i=0;i<sizeof colorHandlers/sizeof colorHandlers[0];++i) {
        action.m_exec=colorHandlers[i];
        addHandler(40u+i,action);
    }
#ifdef PERF_LAB
    action.m_exec=&Action::execImpl<BoostDeck,&BoostDeck::changeUnits<false> >;
    addHandler(49u,action);
    action.m_exec=&Action::execImpl<BoostDeck,&BoostDeck::changeUnits<true> >;
    addHandler(59u,action);
    action.m_exec=&Action::execImpl<BoostDeck,&BoostDeck::showMax<true> >;
    addHandler(120u,action);
    action.m_exec=&Action::execImpl<BoostDeck,&BoostDeck::showMax<false> >;
    addHandler(121u,action);
    action.m_exec=&Action::execImpl<BoostDeck,&BoostDeck::clearPeaks>;
    addHandler(122u,action);
#endif
    selectColor(color_);
    bmw::Parcel visible(true);
    for (unsigned i=0;i<kBarCount;++i) setProperty(200u+i,3u,visible);
    for (unsigned i=0;i<kExtraCount;++i) setProperty(kExtraBarComponent+i,3u,visible);
    refresh();
    if (fd_>=0) {
        ++poll_generation_;
        postPoll();
    }
    return true;
}

bool BoostDeck::gaugeVisibility(const bmw::Parcel& event) {
    bool visible;
    if (!event.readBool(visible) || gauges_visible_==visible) return true;
    gauges_visible_=visible;
    if (visible) {
#ifdef PERF_LAB
        if (hidden_at_ && bd_mono_ms()-hidden_at_>10000) resetPeaks(); /* a new session */
#endif
        retry_at_=0;
        startVisibleCollector();
    } else {
#ifdef PERF_LAB
        hidden_at_=bd_mono_ms();
#endif
        stopCollector();
        received_=0; deck_at_=0;
        memset(&frame_,0,sizeof frame_);
        memset(&deck_,0,sizeof deck_);
        refresh();
    }
    return true;
}

void BoostDeck::startVisibleCollector() {
    if (!gauges_visible_ || collector_>0 || fd_<0 ||
        bd_mono_ms()<retry_at_) return;
    char host[16];
    retry_at_=bd_mono_ms()+2000;
    if (!installed_gateway(host)) return;
    const char* ethanol=ethanol_mode(kEthanolFile);
    const char *args[]={kCollector,"--host",host,"--client","deck",ethanol ? "--ethanol" : 0,ethanol,0};
    collector_=launch(args,-1);
}

void BoostDeck::selectColor(unsigned color) {
    color_=color;
    char label[48];
    snprintf(label,sizeof label,"Selected: %s",kColorNames[color_]);
    updateModel(110u,bmw::BasicString<char>(label));
    refresh();
}

void BoostDeck::postPoll() {
    // The framework copies the Message and Parcel into its own queue. Entries
    // carry only data, so clearing the queue needs no code from this module.
    bmw::Parcel data;
    data.writeUInt32(kPollKey);
    data.writeUInt32(poll_generation_);
    bmw::Message message(kDataUpdateMessage, data);
    post(message, collector_ > 0 ? kActivePollIntervalMs : kIdlePollIntervalMs);
}

bool BoostDeck::invokeDataUpdate(unsigned key, const bmw::Parcel& data) {
    if (key!=kPollKey) return bmw::PluginApp::invokeDataUpdate(key,data);
    unsigned generation;
    if (!data.readUInt32(generation)) return false;
    if (fd_<0 || generation!=poll_generation_) return true;
    poll();
    if (fd_>=0 && generation==poll_generation_) postPoll();
    return true;
}

void BoostDeck::poll() {
    reapCollector();
    startVisibleCollector();
    // A bounded non-blocking drain: at most 8 * 512 bytes per UI tick.
    char data[BD_MAX_PACKET];
    for (unsigned n=0;fd_>=0 && n<8;++n) {
        ssize_t k=recv(fd_,data,sizeof data,0);
        if (k<=0) break;
        bd_frame next;
        if (gauges_visible_ && !stopping_ && bd_parse(data,static_cast<size_t>(k),&next) &&
            next.sent_ms && next.sent_ms<=bd_mono_ms() &&
            (!received_ || next.sent_ms>=received_)) {
            frame_=next; received_=next.sent_ms; // queued UDP does not reset sample age
        }
    }
    for (unsigned n=0;deck_fd_>=0 && n<8;++n) {
        deck_frame next;
        ssize_t k=recv(deck_fd_,&next,sizeof next,0);
        if (k<=0) break;
        const uint64_t now=bd_mono_ms();
        if (k==static_cast<ssize_t>(sizeof next) && gauges_visible_ && !stopping_ &&
            next.sampled_ms && next.sampled_ms<=now && next.sampled_ms>=deck_.sampled_ms) {
            deck_=next; deck_at_=next.sampled_ms;
        }
    }
    refresh();
}

void BoostDeck::refresh() {
    uint64_t elapsed=received_ ? bd_mono_ms()-received_ : static_cast<uint64_t>(BD_STALE_MS);
    for (unsigned i=0;i<kBarCount;++i) {
        char label[128];
        const DialScale& scale = kDialScales[i];
        unsigned image = kBarFirst;
#ifdef PERF_LAB
        const bool fresh = received_ && bd_is_fresh(&frame_,i,elapsed);
        // AFR keeps its live value: its session high is just decel fuel cut.
        const bool peak_tile = i!=BD_AFR;
        if (fresh && peak_tile && (!has_peak_[i] || frame_.value[i]>peak_[i])) {
            peak_[i]=frame_.value[i]; has_peak_[i]=true;
        }
        const bool max_shown = show_max_ && peak_tile;
        if (max_shown ? has_peak_[i] : fresh) {
            double x=max_shown ? peak_[i] : frame_.value[i];
            const char *max_prefix = max_shown ? "Max " : "";
#else
        if (received_ && bd_is_fresh(&frame_,i,elapsed)) {
            double x=frame_.value[i];
#endif
            const char *unit_space = *scale.unit ? " " : "";
            double reading = bd_display_value(i,x,0);
            if (i==BD_BOOST && fabs(reading)<0.05) reading=0;
#ifdef PERF_LAB
            if (metric_) {
                const char* units=i==BD_AFR ? "" : i==BD_TIMING ? "deg" :
                    i==BD_IAT || i==BD_TRANS || i==BD_COOLANT ? "\260C" : "bar";
                snprintf(label,sizeof label,"%s%.1f%s%s",max_prefix,bd_display_value(i,x,1),*units ? " " : "",units);
            } else snprintf(label,sizeof label,i==BD_RAIL ? "%s%.0f%s%s" : "%s%.1f%s%s",max_prefix,reading,unit_space,scale.unit);
#else
            const char *prefix = frame_.demo && i==BD_BOOST ? "Demo " : "";
            const char *format = i==BD_BOOST ? "%s%s   %+.1f%s%s"
                                : i==BD_RAIL ? "%s%s   %.0f%s%s"
                                : "%s%s   %.1f%s%s";
            snprintf(label,sizeof label,format,prefix,scale.name,reading,unit_space,scale.unit);
#endif
            double fraction=(x-scale.minimum)/(scale.maximum-scale.minimum);
            if (fraction<0) fraction=0;
            if (fraction>1) fraction=1;
            unsigned step=static_cast<unsigned>(fraction*kDialSteps+.5);
            if (step) {
                const unsigned palette = color_ == kMulticolor ? kMulticolor + i : color_;
                image += palette*kDialSteps+step;
            }
        } else {
#ifdef PERF_LAB
            strcpy(label,"--");
#else
            snprintf(label,sizeof label,"%s   --",scale.name);
#endif
        }
        if (image!=images_[i]) {
            updateModel(60u+i,bmw::ImageID(image));
            images_[i]=image;
        }
        if (strcmp(label,labels_[i])) {
            updateModel(50u+i,bmw::BasicString<char>(label));
            strcpy(labels_[i],label);
        }
    }
#ifdef PERF_LAB
    const bool deck_live = deck_at_ && bd_mono_ms()-deck_at_ < DECK_STALE_MS;
    const double ethanol_pct = deck_.ethanol/10.;
    const bool valid = deck_live && deck_.ethanol!=HUB_NONE && ethanol_pct>=0 && ethanol_pct<=100;
    char ethanol[128];
    if (valid) snprintf(ethanol,sizeof ethanol,"%.0f%%",ethanol_pct);
    else strcpy(ethanol,"--");
    const unsigned step=valid ? static_cast<unsigned>(ethanol_pct*kEthanolSteps/100.+.5) : 0;
    const unsigned ethanolImage=kEthanolFirst+(step ? color_*kEthanolSteps+step : 0);
    if (images_[BD_ETHANOL]!=ethanolImage) {
        updateModel(68u,bmw::ImageID(ethanolImage));
        images_[BD_ETHANOL]=ethanolImage;
    }
    if (strcmp(labels_[BD_ETHANOL],ethanol)) {
        updateModel(58u,bmw::BasicString<char>(ethanol));
        strcpy(labels_[BD_ETHANOL],ethanol);
    }
    refreshExtras();
#endif
}

#ifdef PERF_LAB
// Third row: power, torque, throttle, oil temperature (hub "deck" frames).
void BoostDeck::refreshExtras() {
    const bool live = deck_at_ && bd_mono_ms()-deck_at_ < DECK_STALE_MS;
    const int32_t raw[kExtraCount] = {deck_.power, deck_.torque, deck_.throttle, deck_.oil_temp};
    for (unsigned i=0;i<kExtraCount;++i) {
        char label[48];
        unsigned image=kBarFirst;
        if (live && raw[i]!=HUB_NONE && (!has_extra_peak_[i] || raw[i]/10.>extra_peak_[i])) {
            extra_peak_[i]=raw[i]/10.; has_extra_peak_[i]=true;
        }
        if (show_max_ ? has_extra_peak_[i] : live && raw[i]!=HUB_NONE) {
            const double x=show_max_ ? extra_peak_[i] : raw[i]/10.;
            double imperial=x, shown=x;
            const char* format="%.0f %s";
            const char* unit="%";
            switch (i) {
            case 0: imperial=x*1.34102; shown=metric_ ? x : imperial; unit=metric_ ? "kW" : "hp"; break;
            case 1: imperial=x*.737562; shown=metric_ ? x : imperial; unit=metric_ ? "Nm" : "lb-ft"; break;
            case 2: format="%.0f%s"; break;
            default: imperial=x*1.8+32; shown=metric_ ? x : imperial; format="%.1f %s"; unit=metric_ ? "\260C" : "\260F";
            }
            char value[40];
            snprintf(value,sizeof value,format,shown,unit);
            snprintf(label,sizeof label,"%s%s",show_max_ ? "Max " : "",value);
            double fraction=(imperial-kExtraScales[i].minimum)/(kExtraScales[i].maximum-kExtraScales[i].minimum);
            if (fraction<0) fraction=0;
            if (fraction>1) fraction=1;
            const unsigned bar=static_cast<unsigned>(fraction*kDialSteps+.5);
            if (bar) {
                const unsigned palette = color_ == kMulticolor ? kMulticolor + (kBarCount+i)%kMulticolor : color_;
                image += palette*kDialSteps+bar;
            }
        } else strcpy(label,"--");
        if (image!=extra_images_[i]) {
            updateModel(kExtraBarModel+i,bmw::ImageID(image));
            extra_images_[i]=image;
        }
        if (strcmp(label,extra_labels_[i])) {
            updateModel(kExtraLabelModel+i,bmw::BasicString<char>(label));
            strcpy(extra_labels_[i],label);
        }
    }
}
#endif

extern "C" {
PluginAppInfo pluginAppInfo = {1, "BoostDeck", "owner-development", 1,
                              create_app<BoostDeck>, destroy_app};
}
