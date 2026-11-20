#pragma once
// Shows a "Proximity Voice Chat" card in LeviLauncher's Mod Menu.
//
// The launcher's Mod Menu API changes between launcher versions (newer ones read extra fields at the end of
// ModuleInfo). Passing a struct that is too SMALL would make the launcher read garbage, so we pass a larger
// struct (the official one + the newer trailing fields + spare room): extra bytes are harmless to a launcher
// that doesn't know them. The functions are looked up by name at runtime so a launcher without a Mod Menu
// just means "no card", never a failed load.
#include <pl/ModMenu.hpp>

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

namespace voicechat {

struct MenuState {
    bool enabled = true;
    bool useRelay = false;
    int micGainPercent = 100;
    int micGate = 350;
    int radius = 32;
};

struct ModuleInfoCompat : pl::modmenu::ModuleInfo {
    std::function<void(std::string_view moduleId, std::string_view key, bool isDown)> onKeybind; // newer launchers
    unsigned char spare[256]{};                                                                  // future fields
};

class ModMenuLink {
  public:
    using RegisterFn = bool (*)(const pl::modmenu::ModuleInfo &);
    using UnregisterFn = void (*)(std::string_view);
    using ToggleFn = std::function<void(bool enabled)>;
    using ChangeFn = std::function<void(std::string_view key, std::string_view value)>;

    static constexpr const char *kModuleId = "proximity_voice_chat";

    ModMenuLink();
    ~ModMenuLink();
    ModMenuLink(const ModMenuLink &) = delete;
    ModMenuLink &operator=(const ModMenuLink &) = delete;

    // Finds the launcher's functions and registers the card. `why` explains a failure.
    bool connect(std::string_view modId, const MenuState &state, ToggleFn onToggle, ChangeFn onChange,
                 std::string *why = nullptr);
    // Same, with the launcher's functions supplied by the caller (used by tests).
    bool connectWith(RegisterFn reg, UnregisterFn unreg, std::string_view modId, const MenuState &state,
                     ToggleFn onToggle, ChangeFn onChange);
    void disconnect(); // after this returns, no callback of ours is ever run again
    bool connected() const;

    static bool parseBool(std::string_view v, bool fallback);
    static int parseInt(std::string_view v, int fallback, int lo, int hi);

  private:
    struct Shared; // what launcher-invoked callbacks touch; outlives this object on purpose
    std::shared_ptr<Shared> shared_;
    UnregisterFn unreg_ = nullptr;
    bool registered_ = false;
    void *lib_ = nullptr;
};

} // namespace voicechat
