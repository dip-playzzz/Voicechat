// Checks ModMenuLink against two fake launchers: one with the older ModuleInfo layout (official header)
// and one with the newer layout that has extra fields at the end. Needs the preloader headers:
//   g++ -std=c++20 -I<preloader-android>/include -Isrc tests/modmenu_test.cpp src/mod/ModMenuLink.cpp
#include "mod/ModMenuLink.h"

#include <cstdio>
#include <cstdlib>
#include <string>

using namespace voicechat;
#define CHECK(c, msg) do { if (!(c)) { std::printf("FAIL: %s (line %d)\n", msg, __LINE__); std::exit(1); } } while (0)

// What a NEWER launcher's ModuleInfo looks like (extra onKeybind at the end).
struct NewLayout {
    std::string moduleId, displayName, description, modId;
    bool defaultEnabled{}, hideInHudEditor{};
    std::vector<pl::modmenu::ConfigEntry> configs;
    std::function<void(std::string_view, bool)> onToggle;
    std::function<void(std::string_view, std::string_view, std::string_view)> onConfigChanged;
    std::function<void(std::string_view, std::string_view, bool)> onKeybind;
};

static NewLayout gNew;
static pl::modmenu::ModuleInfo gOld;
static std::string gUnregistered;
static bool fakeRegisterNew(const NewLayout &m) { gNew = m; return true; }                       // copies the WHOLE newer struct
static bool fakeRegisterOld(const pl::modmenu::ModuleInfo &m) { gOld = m; return true; }
static bool fakeReject(const pl::modmenu::ModuleInfo &) { return false; }
static void fakeUnregister(std::string_view id) { gUnregistered = std::string(id); }

int main() {
    // The extra fields really are placed AFTER the official struct, never inside it.
    { ModuleInfoCompat c;
      const auto off = reinterpret_cast<char *>(&c.onKeybind) - reinterpret_cast<char *>(&c);
      CHECK(static_cast<size_t>(off) >= sizeof(pl::modmenu::ModuleInfo), "extension starts after the official struct");
      CHECK(sizeof(ModuleInfoCompat) >= sizeof(NewLayout), "our struct is at least as big as the newer launcher's"); }

    MenuState st; st.enabled = true; st.useRelay = true; st.micGainPercent = 120; st.micGate = 400; st.radius = 40;

    // ---- newer launcher
    {
        bool toggled = true; std::string k, v;
        ModMenuLink link;
        CHECK(link.connectWith(reinterpret_cast<ModMenuLink::RegisterFn>(&fakeRegisterNew), &fakeUnregister, "mod-id", st,
                               [&](bool e) { toggled = e; }, [&](std::string_view a, std::string_view b) { k = a; v = b; }),
              "registers with a newer launcher");
        CHECK(gNew.moduleId == "proximity_voice_chat" && gNew.modId == "mod-id" && gNew.displayName == "Proximity Voice Chat", "identity fields");
        CHECK(gNew.defaultEnabled && gNew.hideInHudEditor, "flags");
        CHECK(gNew.configs.size() == 4 && gNew.configs[0].key == "useRelay" && gNew.configs[0].defaultValue == "true" &&
              gNew.configs[1].key == "micGainPercent" && gNew.configs[1].defaultValue == "120" && gNew.configs[1].maxValue == "400" &&
              gNew.configs[3].key == "radius" && gNew.configs[3].defaultValue == "40", "config entries");
        CHECK(static_cast<bool>(gNew.onKeybind), "newer launcher finds a valid (empty) keybind handler");
        gNew.onKeybind("proximity_voice_chat", "x", true);      // must be callable, does nothing
        gNew.onToggle("proximity_voice_chat", false);
        CHECK(toggled == false, "toggle reaches our handler");
        gNew.onConfigChanged("proximity_voice_chat", "micGainPercent", "150");
        CHECK(k == "micGainPercent" && v == "150", "setting change reaches our handler");

        auto lateToggle = gNew.onToggle; auto lateChange = gNew.onConfigChanged;     // launcher keeps these
        link.disconnect();
        CHECK(gUnregistered == "proximity_voice_chat", "unregistered on disconnect");
        toggled = true; k.clear();
        lateToggle("proximity_voice_chat", false); lateChange("proximity_voice_chat", "radius", "9");
        CHECK(toggled == true && k.empty(), "late callbacks after disconnect do nothing");
    }
    // ---- callbacks that outlive the whole ModMenuLink object (launcher UI thread racing our shutdown)
    {
        std::function<void(std::string_view, bool)> late;
        { ModMenuLink link;
          link.connectWith(reinterpret_cast<ModMenuLink::RegisterFn>(&fakeRegisterNew), &fakeUnregister, "m", st, [](bool) {}, [](auto, auto) {});
          late = gNew.onToggle; }
        late("proximity_voice_chat", true);          // object is gone; must not touch freed memory (ASan checks)
    }
    // ---- older launcher (official header layout)
    {
        std::string k;
        ModMenuLink link;
        CHECK(link.connectWith(&fakeRegisterOld, &fakeUnregister, "mod-id", st, [](bool) {}, [&](std::string_view a, std::string_view) { k = a; }),
              "registers with an older launcher");
        CHECK(gOld.moduleId == "proximity_voice_chat" && gOld.configs.size() == 4, "older layout reads the same data");
        gOld.onConfigChanged("proximity_voice_chat", "micGate", "10");
        CHECK(k == "micGate", "older launcher callbacks work");
    }
    // ---- launcher refuses
    { ModMenuLink link; CHECK(!link.connectWith(&fakeReject, &fakeUnregister, "m", st, [](bool) {}, [](auto, auto) {}), "rejection reported"); CHECK(!link.connected(), "not connected"); }

    // ---- value parsing
    CHECK(ModMenuLink::parseBool("true", false) && ModMenuLink::parseBool("1", false) && !ModMenuLink::parseBool("false", true) &&
          !ModMenuLink::parseBool("0", true) && ModMenuLink::parseBool("???", true), "parseBool");
    CHECK(ModMenuLink::parseInt("150", 0, 0, 400) == 150 && ModMenuLink::parseInt("150.0", 0, 0, 400) == 150 &&
          ModMenuLink::parseInt("999", 0, 0, 400) == 400 && ModMenuLink::parseInt("-5", 7, 0, 400) == 0 &&
          ModMenuLink::parseInt("abc", 77, 0, 400) == 77 && ModMenuLink::parseInt("", 5, 0, 10) == 5, "parseInt");
    std::printf("modmenu tests ok\n");
}
