#!/bin/sh
# Runs the PC-side tests. Needs g++, python3 and libopus.so.0 (or set OPUSLIB).
set -eu
cd "$(dirname "$0")/.."
OPUSLIB="${OPUSLIB:--l:libopus.so.0}"
SAN="${SANITIZERS:--fsanitize=address,undefined -g -O1}"
TMPDIR_VC=$(mktemp -d "${TMPDIR:-/tmp}/voicechat-tests.XXXXXX")
trap 'rm -rf "$TMPDIR_VC"' EXIT HUP INT TERM

g++ -std=c++20 -Isrc tests/proximity_test.cpp -o "$TMPDIR_VC/proximity"
"$TMPDIR_VC/proximity"
g++ -std=c++20 $SAN -Isrc tests/safety_test.cpp -o "$TMPDIR_VC/safety"
"$TMPDIR_VC/safety"
g++ -std=c++20 -DVOICECHAT_TEST $SAN -Isrc -Ithird_party/bedrocktools/include \
    tests/tracker_lifetime_test.cpp src/voice/PlayerTracker.cpp -pthread -o "$TMPDIR_VC/tracker"
"$TMPDIR_VC/tracker" 1000
g++ -std=c++20 $SAN -Isrc tests/mic_socket_test.cpp src/voice/MicSocketSource.cpp -pthread -o "$TMPDIR_VC/mic"
"$TMPDIR_VC/mic"

PRE="${PRELOADER_INC:-/tmp/deps/pre/include}"
if [ -d "$PRE" ]; then
    g++ -std=c++20 $SAN -I"$PRE" -Isrc tests/modmenu_test.cpp src/mod/ModMenuLink.cpp -pthread -o "$TMPDIR_VC/modmenu"
    "$TMPDIR_VC/modmenu"
else
    echo "(skipping modmenu test: set PRELOADER_INC to preloader-android/include)"
fi

g++ -std=c++20 $SAN -Itests/host_opus -Isrc tests/voice_pipeline_test.cpp src/voice/VoiceSender.cpp \
    src/voice/RemoteVoices.cpp $OPUSLIB -pthread -o "$TMPDIR_VC/pipeline"
"$TMPDIR_VC/pipeline"
(cd relay && python3 test_server.py && python3 test_multiplayer.py)
python3 tests/relay_integration.py
OPUSLIB="$OPUSLIB" python3 tests/voice_e2e.py
if command -v javac >/dev/null && command -v java >/dev/null; then
    sh bridge/test/run_java_tests.sh
else
    echo "(skipping Java bridge tests: JDK not installed)"
fi
