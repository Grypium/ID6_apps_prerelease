// FaultCodes: read-only fault memory of every module, shown under My Vehicle.
//
// Pressing "Read fault codes" starts fault-codes-qnx (collector.c built with
// -DFAULT_SCAN_ONLY), which sends one functional ReadDTCInformation request
// through the diagnostic gateway and prints a summary line followed by one
// "EE CCCCCC SS" line per fault (module address, code, status). This plugin
// pages through them four at a time. Nothing is ever cleared or written.
#include "plugin_factory.h"
#include "diagnostic_worker.h"
#include "dme_descriptions.h"
#include <stdio.h>
#include <sys/wait.h>

namespace {
const unsigned kPollKey = 0x46435343u; // "FCSC"
const unsigned kPageSize = 4;
const char kWorker[] = "/var/opt/hmi/data/ID5/Plugins/FaultCodes/fault-codes-qnx";

// Common F-series diagnostic addresses. Unknown addresses show as "Module XX".
struct ModuleName { unsigned char address; const char* brief; const char* full; };
const ModuleName kModules[] = {
    {0x01, "ACSM", "Airbag / crash safety module"},
    {0x10, "ZGM", "Central gateway"},
    {0x12, "DME", "Engine electronics"},
    {0x18, "EGS", "Transmission"},
    {0x1C, "ICM", "Integrated chassis management"},
    {0x29, "DSC", "Dynamic stability control"},
    {0x30, "EPS", "Electric power steering"},
    {0x40, "FEM", "Body / front electronics"},
    {0x56, "FZD", "Roof function center"},
    {0x5D, "KAFAS", "Camera driver assistance"},
    {0x60, "KOMBI", "Instrument cluster"},
    {0x63, "HU", "Head unit"},
    {0x78, "IHKA", "Climate control"},
};

const ModuleName* module_name(unsigned address) {
    for (unsigned i = 0; i < sizeof kModules / sizeof kModules[0]; ++i)
        if (kModules[i].address == address) return &kModules[i];
    return 0;
}

int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

// Parses exactly `digits` hex digits at s; returns false on anything else.
bool parse_hex(const char* s, int digits, unsigned& value) {
    value = 0;
    for (int i = 0; i < digits; ++i) {
        int d = hex_digit(s[i]);
        if (d < 0) return false;
        value = value * 16u + static_cast<unsigned>(d);
    }
    return true;
}

const char* status_word(unsigned status) {
    if (status & 0x01u) return "Active";
    if ((status & 0x04u) && !(status & 0x08u)) return "Pending";
    return "Stored";
}

// "EE CCCCCC SS" -> left "CCCCCC  BRIEF", right "Status - description".
// Lines that are not in that form (worker messages) are shown as they are.
void format_line(const char* line, int length, char* left, size_t left_size,
                 char* right, size_t right_size) {
    unsigned ecu, code, status;
    right[0] = 0;
    if (length != 12 || line[2] != ' ' || line[9] != ' ' || !parse_hex(line, 2, ecu) ||
        !parse_hex(line + 3, 6, code) || !parse_hex(line + 10, 2, status)) {
        snprintf(left, left_size, "%.*s", length, line);
        return;
    }
    const ModuleName* module = module_name(ecu);
    char brief[16];
    if (module) snprintf(brief, sizeof brief, "%s", module->brief);
    else snprintf(brief, sizeof brief, "Module %02X", ecu);
    snprintf(left, left_size, "%06X  %s", code, brief);

    const char* description = 0;
    if (ecu == 0x12) {
        for (unsigned i = 0; i < sizeof kDmeDescriptions / sizeof kDmeDescriptions[0]; ++i)
            if (kDmeDescriptions[i].code == code) { description = kDmeDescriptions[i].text; break; }
    }
    if (description) snprintf(right, right_size, "%s - %s", status_word(status), description);
    else if (module) snprintf(right, right_size, "%s - %s", status_word(status), module->full);
    else snprintf(right, right_size, "%s - module address %02X", status_word(status), ecu);
}

// Gateway address written by the installer next to the worker (one line, 169.254.x.x).
bool installed_gateway(const char* worker, char (&host)[16]) {
    char path[192];
    const char* last = strrchr(worker, '/');
    if (!last) return false;
    int length = snprintf(path, sizeof path, "%.*s/gateway-ip.txt", static_cast<int>(last - worker), worker);
    if (length < 0 || static_cast<size_t>(length) >= sizeof path) return false;
    FILE* file = fopen(path, "r");
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

#if defined(__arm__)
typedef char fault_message_size[(sizeof(bmw::Message) == 80 && __alignof__(bmw::Message) == 8) ? 1 : -1];
#endif

class FaultCodes : public bmw::PluginApp {
    pid_t pid_;
    int fd_;
    char result_[4096];
    size_t size_;
    unsigned page_;
    unsigned generation_;

    void reap() {
        if (pid_ <= 0) return;
        pid_t ended = waitpid(pid_, 0, WNOHANG);
        if (ended == pid_ || (ended < 0 && errno == ECHILD)) pid_ = -1;
    }
    void stopWorker() {
        ++generation_;
        if (fd_ >= 0) { close(fd_); fd_ = -1; }
        reap();
        if (pid_ > 0) kill(pid_, SIGTERM);
    }
    void postPoll() {
        bmw::Parcel data;
        data.writeUInt32(kPollKey);
        data.writeUInt32(generation_);
        post(bmw::Message(0x40000003u, data), 100u);
    }
    void header(const char* text) { updateModel(80u, bmw::BasicString<char>(text)); }
    void clearRows() {
        for (unsigned i = 0; i < kPageSize; ++i) {
            updateModel(81u + i, bmw::BasicString<char>(""));
            updateModel(91u + i, bmw::BasicString<char>(""));
        }
    }
    void startScan();
    void pollScan();
    void render();

public:
    FaultCodes(bmw::IDispatcher& dispatcher, bmw::BasicString<char> name, unsigned major,
               unsigned minor, bmw::RHMIInterfaceVersion version, const bmw::Bundle& config,
               const bmw::Bundle& resources)
        : PluginApp(dispatcher, name, major, minor, version, config, resources),
          pid_(-1), fd_(-1), size_(0), page_(0), generation_(0) {}
    virtual ~FaultCodes() { stopWorker(); }

    virtual bool onInit() {
        stopWorker();
        typedef bmw::Delegate<bool, const bmw::Parcel&> Action;
        Action action;
        action.m_obj = this;
        action.m_exec = &Action::execImpl<FaultCodes, &FaultCodes::readCodes>;
        addHandler(23u, action);
        action.m_exec = &Action::execImpl<FaultCodes, &FaultCodes::nextPage>;
        addHandler(29u, action);
        size_ = 0;
        page_ = 0;
        clearRows();
        header("Select Read fault codes (read only, about 5 seconds)");
        return true;
    }
    virtual bool onFinalize() { stopWorker(); return true; }
    virtual bool invokeDataUpdate(unsigned key, const bmw::Parcel& data) {
        if (key != kPollKey) return PluginApp::invokeDataUpdate(key, data);
        unsigned generation;
        if (!data.readUInt32(generation)) return false;
        if (generation != generation_) return true;
        pollScan();
        if (fd_ >= 0 || pid_ > 0) postPoll();
        return true;
    }

    bool readCodes(const bmw::Parcel&) { startScan(); return true; }
    bool nextPage(const bmw::Parcel&) {
        if (fd_ < 0 && size_) { ++page_; render(); }
        return true;
    }
};

void FaultCodes::startScan() {
    reap();
    if (fd_ >= 0 || pid_ > 0) return;
    char host[16];
    if (!installed_gateway(kWorker, host)) {
        header("Missing gateway-ip.txt. Reinstall FaultCodes.");
        return;
    }
    int channel[2];
    if (pipe(channel) < 0) { header("Could not start diagnostics"); return; }
    const char* args[] = {kWorker, "--host", host, 0};
    if (fcntl(channel[0], F_SETFL, O_NONBLOCK) >= 0 && fcntl(channel[0], F_SETFD, FD_CLOEXEC) >= 0 &&
        fcntl(channel[1], F_SETFD, FD_CLOEXEC) >= 0)
        pid_ = launch(args, channel[1]);
    close(channel[1]);
    if (pid_ < 0) {
        close(channel[0]);
        header("Could not start diagnostics");
        return;
    }
    fd_ = channel[0];
    size_ = 0;
    page_ = 0;
    result_[0] = 0;
    ++generation_;
    clearRows();
    header("Reading fault memory of all modules...");
    postPoll();
}

void FaultCodes::pollScan() {
    if (fd_ >= 0) {
        ssize_t n = read(fd_, result_ + size_, sizeof result_ - 1 - size_);
        if (n > 0) {
            size_ += static_cast<size_t>(n);
            result_[size_] = 0;
            if (size_ < sizeof result_ - 1) return;
        } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
            return;
        }
        close(fd_);
        fd_ = -1;
        if (size_) render();
        else header("No diagnostic result returned");
    }
    reap();
}

void FaultCodes::render() {
    if (!size_) return;
    const char* first = strchr(result_, '\n');
    unsigned lines = 0;
    for (const char* line = first; line && line[1]; line = strchr(line + 1, '\n')) ++lines;
    // No division: the head unit's libc may not export the ARM EABI divide helpers.
    unsigned pages = 1;
    for (unsigned covered = kPageSize; covered < lines; covered += kPageSize) ++pages;
    if (page_ >= pages) page_ = 0;
    char text[160];
    int length = first ? static_cast<int>(first - result_) : static_cast<int>(size_);
    if (length > 110) length = 110;
    if (pages > 1) snprintf(text, sizeof text, "%.*s  (page %u/%u)", length, result_, page_ + 1, pages);
    else snprintf(text, sizeof text, "%.*s", length, result_);
    header(text);
    const char* line = first ? first + 1 : "";
    for (unsigned i = 0; i < page_ * kPageSize && *line; ++i) {
        const char* end = strchr(line, '\n');
        line = end ? end + 1 : "";
    }
    for (unsigned i = 0; i < kPageSize; ++i) {
        const char* end = strchr(line, '\n');
        int n = end ? static_cast<int>(end - line) : static_cast<int>(strlen(line));
        char left[64], right[160];
        format_line(line, n, left, sizeof left, right, sizeof right);
        updateModel(81u + i, bmw::BasicString<char>(left));
        updateModel(91u + i, bmw::BasicString<char>(right));
        line = end ? end + 1 : "";
    }
}

extern "C" {
PluginAppInfo pluginAppInfo = {1, "FaultCodes", "owner-development", 1,
                               create_app<FaultCodes>, destroy_app};
}
