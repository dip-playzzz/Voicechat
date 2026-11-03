#include "voice/Proximity.h"
#include <cassert>
#include <cmath>
#include <cstdio>
using namespace voicechat;
static bool near(float a, float b) { return std::fabs(a - b) < 1e-3f; }
int main() {
    // gain
    assert(near(proximityGain(0, 32), 1.f));
    assert(near(proximityGain(16, 32), 0.25f));
    assert(proximityGain(32, 32) == 0.f && proximityGain(100, 32) == 0.f);
    // yaw 0 = facing +Z (south). Right-hand side is -X (west).
    assert(near(stereoPan(0, 0, 0, 0, 10), 0.f));   // straight ahead
    assert(near(stereoPan(0, 0, 0, -10, 0), 1.f));  // west = right
    assert(near(stereoPan(0, 0, 0, 10, 0), -1.f));  // east = left
    assert(near(stereoPan(0, 0, 0, 0, -10), 0.f));  // directly behind -> centre
    // yaw 90 = facing -X (west). Right-hand side is -Z (north).
    assert(near(stereoPan(0, 0, 90, -10, 0), 0.f));
    assert(near(stereoPan(0, 0, 90, 0, -10), 1.f));
    assert(near(stereoPan(0, 0, 90, 0, 10), -1.f));
    assert(near(stereoPan(5, 5, 33, 5, 5), 0.f));   // same spot
    puts("proximity ok");
}
