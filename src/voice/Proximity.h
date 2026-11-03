#pragma once
// Pure math for proximity voice: distance falloff and stereo pan.
// Minecraft convention: yaw 0 faces +Z, yaw 90 faces -X. Forward = (-sin, cos) in (x, z).
#include <algorithm>
#include <cmath>

namespace voicechat {

// 1.0 right next to the speaker, 0.0 at/after `radius`. Squared falloff sounds more natural.
inline float proximityGain(float distance, float radius) {
    if (radius <= 0.f || distance >= radius) return 0.f;
    const float t = 1.f - std::max(distance, 0.f) / radius;
    return t * t;
}

// -1 = fully left, 0 = centre, +1 = fully right, relative to where the listener is facing.
inline float stereoPan(float listenerX, float listenerZ, float listenerYawDeg,
                       float sourceX, float sourceZ) {
    const float dx = sourceX - listenerX, dz = sourceZ - listenerZ;
    const float len = std::sqrt(dx * dx + dz * dz);
    if (len < 1e-4f) return 0.f; // standing on top of each other
    const float yaw = listenerYawDeg * 3.14159265358979f / 180.f;
    const float rightX = -std::cos(yaw), rightZ = -std::sin(yaw); // listener's right-hand side
    return std::clamp((dx * rightX + dz * rightZ) / len, -1.f, 1.f);
}

} // namespace voicechat
