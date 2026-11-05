#include "voice/GameVersion.h"
#include "voice/SafeMem.h"

#include <sys/mman.h>
#include <unistd.h>

#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
using namespace voicechat;

int main() {
    // --- safeRead: valid memory works
    float src[3] = {1.5f, 64.f, -3.f}, dst[3] = {};
    assert(safeRead(src, dst, sizeof src) && dst[1] == 64.f);

    // --- safeRead: bad addresses fail instead of crashing (the exact garbage pointer from the crash dump)
    char buf[16];
    assert(!safeRead(nullptr, buf, 8));
    assert(!safeRead(reinterpret_cast<void *>(0x1000), buf, 8));
    assert(!safeRead(reinterpret_cast<void *>(0xb5dc4b6c37981bULL), buf, 12));
    assert(!safeRead(reinterpret_cast<void *>(~0ULL - 7), buf, 8));

    // --- straddling the end of a mapping: partial read must fail cleanly and not poison later reads
    const long page = sysconf(_SC_PAGESIZE);
    char *m = static_cast<char *>(mmap(nullptr, 2 * page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    assert(m != MAP_FAILED);
    assert(mprotect(m + page, page, PROT_NONE) == 0);
    std::memset(m, 7, page);
    char big[64];
    assert(!safeRead(m + page - 8, big, 16));        // 8 bytes valid, 8 not
    assert(safeRead(m + page - 16, big, 16));        // fully inside the valid page
    assert(big[0] == 7 && big[15] == 7);
    float again[3] = {};
    assert(safeRead(src, again, sizeof src) && again[0] == 1.5f); // pipe still healthy afterwards

    // --- plausibility filter
    float p1[3] = {100.f, 64.f, -200.f}, r1[2] = {10.f, 90.f};
    assert(plausiblePlayerSample(p1, r1));
    float p2[3] = {NAN, 64.f, 0.f}; assert(!plausiblePlayerSample(p2, r1));
    float p3[3] = {1e12f, 64.f, 0.f}; assert(!plausiblePlayerSample(p3, r1));
    float r2[2] = {400.f, 0.f}; assert(!plausiblePlayerSample(p1, r2));

    // --- version parsing / gate
    auto v = parseGameVersion("/storage/emulated/0/Android/media/org.levimc.launcher/minecraft/Minecraft_1.26.60.4/mods/Proximity Voice Chat");
    assert(v.valid && v.text == "1.26.60.4" && isSupportedGameVersion(v));
    auto w = parseGameVersion("/x/minecraft/Minecraft_1.26.52.3/mods/y");
    assert(w.valid && w.text == "1.26.52.3" && !isSupportedGameVersion(w));
    assert(!parseGameVersion("/no/version/here").valid);
    assert(!isSupportedGameVersion(parseGameVersion("/x/Minecraft_1.26/mods")));
    assert(isSupportedGameVersion(parseGameVersion("/x/Minecraft_1.26.60/mods"))); // 3-part also ok

    puts("safety tests ok");
}
