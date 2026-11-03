#pragma once
// Microphone permission helpers. The mod runs INSIDE the LeviLauncher app's process,
// so Android's permission system applies to that app (org.levimc.launcher), not to
// the Minecraft package. These helpers talk to the Java side through JNI.
#include <string>

namespace voicechat {

struct MicStatus {
    bool ok = false;       // false if we couldn't query yet (no JavaVM / no Application)
    std::string package;   // app the permission applies to
    bool declared = false; // RECORD_AUDIO appears in that app's manifest
    bool granted = false;  // user has allowed it
    std::string error;
};

MicStatus queryMicPermission();

// Best effort: asks Android to show the microphone prompt for the foreground activity.
// Returns a short description of what happened (for the log).
std::string requestMicPermission();

} // namespace voicechat
