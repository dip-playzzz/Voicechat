#pragma once

namespace voicechat::audio_defaults {

// Microphone monitoring can create a loud acoustic feedback loop. It is opt-in,
// with a conservative starting level and no extra gain for the standalone test.
// These settings do not guarantee safety on every device: use headphones and
// begin with the device volume low when deliberately testing self-hearing.
inline constexpr bool selfHearing = false;
inline constexpr float selfHearingVolume = 0.1f;
inline constexpr float selfHearingMicGainDb = 0.0f;

} // namespace voicechat::audio_defaults
