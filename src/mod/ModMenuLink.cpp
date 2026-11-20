#include "mod/ModMenuLink.h"

#include <dlfcn.h>

#include <algorithm>
#include <charconv>
#include <fstream>

namespace voicechat {

// The launcher may invoke our callbacks from its UI thread at any moment, including just after we asked it
// to unregister. Callbacks therefore only touch this object (kept alive by the shared_ptr they hold) and
// check `active` under a lock; disconnect() clears the handlers and flips `active` first.
struct ModMenuLink::Shared {
    std::mutex mu;
    bool active = true;
    ToggleFn onToggle;
    ChangeFn onChange;
};

namespace {
std::string text(bool v) { return v ? "true" : "false"; }
std::string text(int v) { return std::to_string(v); }

// libpreloader.so is already loaded by the launcher; find it even if a plain name lookup fails.
void *findLoaded(const char *soname) {
    if (void *h = dlopen(soname, RTLD_NOW | RTLD_NOLOAD)) return h;
    std::ifstream maps("/proc/self/maps");
    std::string line;
    while (std::getline(maps, line)) {
        if (line.find(soname) == std::string::npos) continue;
        const auto slash = line.find('/');
        if (slash == std::string::npos) continue;
        if (void *h = dlopen(line.substr(slash).c_str(), RTLD_NOW | RTLD_NOLOAD)) return h;
    }
    return nullptr;
}

void *symbol(void *lib, const char *name) {
    if (void *p = dlsym(RTLD_DEFAULT, name)) return p;
    return lib ? dlsym(lib, name) : nullptr;
}
} // namespace

ModMenuLink::ModMenuLink() = default;
ModMenuLink::~ModMenuLink() { disconnect(); }

bool ModMenuLink::connected() const { return registered_; }

bool ModMenuLink::parseBool(std::string_view v, bool fallback) {
    if (v == "true" || v == "1" || v == "on" || v == "yes" || v == "True") return true;
    if (v == "false" || v == "0" || v == "off" || v == "no" || v == "False") return false;
    return fallback;
}

int ModMenuLink::parseInt(std::string_view v, int fallback, int lo, int hi) {
    int out = fallback;
    if (auto [p, ec] = std::from_chars(v.data(), v.data() + v.size(), out); ec != std::errc()) {
        // sliders may send "150.0": accept a decimal and drop the fraction
        double d = 0;
        try { d = std::stod(std::string(v)); } catch (...) { return fallback; }
        out = static_cast<int>(d);
    }
    return std::clamp(out, lo, hi);
}

bool ModMenuLink::connect(std::string_view modId, const MenuState &state, ToggleFn onToggle, ChangeFn onChange,
                          std::string *why) {
    lib_ = findLoaded("libpreloader.so");
    auto reg = reinterpret_cast<RegisterFn>(symbol(lib_, "_ZN2pl7modmenu14registerModuleERKNS0_10ModuleInfoE"));
    auto unreg = reinterpret_cast<UnregisterFn>(symbol(
        lib_, "_ZN2pl7modmenu16unregisterModuleENSt6__ndk117basic_string_viewIcNS1_11char_traitsIcEEEE"));
    if (!reg || !unreg) {
        if (why) *why = lib_ ? "this LeviLauncher has no Mod Menu (registerModule not found)"
                             : "libpreloader.so not found in the process";
        if (lib_) { dlclose(lib_); lib_ = nullptr; }
        return false;
    }
    if (!connectWith(reg, unreg, modId, state, std::move(onToggle), std::move(onChange))) {
        if (why) *why = "the launcher rejected the module registration";
        return false;
    }
    return true;
}

bool ModMenuLink::connectWith(RegisterFn reg, UnregisterFn unreg, std::string_view modId, const MenuState &state,
                              ToggleFn onToggle, ChangeFn onChange) {
    if (registered_) return true;
    shared_ = std::make_shared<Shared>();
    shared_->onToggle = std::move(onToggle);
    shared_->onChange = std::move(onChange);
    auto shared = shared_; // callbacks keep this alive even if we are destroyed first

    ModuleInfoCompat m;
    m.moduleId = kModuleId;
    m.displayName = "Proximity Voice Chat";
    m.description = "Voice chat with nearby players over the internet relay. Room and secret are set in the mod's config.json.";
    m.modId = std::string(modId);
    m.defaultEnabled = state.enabled;
    m.hideInHudEditor = true;
    m.configs.push_back({"useRelay", "Use internet relay", pl::modmenu::ConfigType::Toggle, text(state.useRelay), "", "", ""});
    m.configs.push_back({"micGainPercent", "Microphone gain (%)", pl::modmenu::ConfigType::SliderInt,
                         text(state.micGainPercent), "0", "400", ""});
    m.configs.push_back({"micGate", "Voice gate (higher = ignores more background noise)",
                         pl::modmenu::ConfigType::SliderInt, text(state.micGate), "0", "2000", ""});
    m.configs.push_back({"radius", "Voice radius (blocks)", pl::modmenu::ConfigType::SliderInt, text(state.radius), "8", "128", ""});
    m.onToggle = [shared](std::string_view, bool enabled) {
        ToggleFn f;
        { std::lock_guard<std::mutex> lock(shared->mu); if (!shared->active) return; f = shared->onToggle; }
        if (f) f(enabled);
    };
    m.onConfigChanged = [shared](std::string_view, std::string_view key, std::string_view value) {
        ChangeFn f;
        { std::lock_guard<std::mutex> lock(shared->mu); if (!shared->active) return; f = shared->onChange; }
        if (f) f(key, value);
    };
    m.onKeybind = [](std::string_view, std::string_view, bool) {};

    if (!reg(m)) {
        std::lock_guard<std::mutex> lock(shared->mu);
        shared->active = false;
        shared->onToggle = nullptr;
        shared->onChange = nullptr;
        return false;
    }
    unreg_ = unreg;
    registered_ = true;
    return true;
}

void ModMenuLink::disconnect() {
    if (shared_) {
        std::lock_guard<std::mutex> lock(shared_->mu);
        shared_->active = false; // callbacks already in flight become no-ops
        shared_->onToggle = nullptr;
        shared_->onChange = nullptr;
    }
    if (registered_ && unreg_) unreg_(kModuleId);
    registered_ = false;
    unreg_ = nullptr;
    if (lib_) { dlclose(lib_); lib_ = nullptr; }
}

} // namespace voicechat
