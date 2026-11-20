#!/bin/sh
# Runs the PC-side tests. Needs g++, python3 and libopus.so.0 (or edit OPUSLIB below).
set -e
cd "$(dirname "$0")/.."
OPUSLIB="${OPUSLIB:--l:libopus.so.0}"
SAN="-fsanitize=address,undefined -g -O1"
g++ -std=c++20 -Isrc tests/proximity_test.cpp -o /tmp/vc_t1 && /tmp/vc_t1
g++ -std=c++20 $SAN -Isrc tests/safety_test.cpp -o /tmp/vc_t2 && /tmp/vc_t2
g++ -std=c++20 -DVOICECHAT_TEST $SAN -Isrc -Ithird_party/bedrocktools/include tests/tracker_lifetime_test.cpp src/voice/PlayerTracker.cpp -pthread -o /tmp/vc_t3 && /tmp/vc_t3 1000
g++ -std=c++20 $SAN -Isrc tests/mic_socket_test.cpp src/voice/MicSocketSource.cpp -pthread -o /tmp/vc_t4 && /tmp/vc_t4 | tail -1
PRE="${PRELOADER_INC:-/tmp/deps/pre/include}"   # git clone --branch 0.2.2 https://github.com/LiteDev/preloader-android
if [ -d "$PRE" ]; then
  g++ -std=c++20 $SAN -I"$PRE" -Isrc tests/modmenu_test.cpp src/mod/ModMenuLink.cpp -o /tmp/vc_t6 && /tmp/vc_t6
else echo "(skipping modmenu test: set PRELOADER_INC to preloader-android/include)"; fi
g++ -std=c++20 $SAN -Itests/host_opus -Isrc tests/voice_pipeline_test.cpp src/voice/VoiceSender.cpp src/voice/RemoteVoices.cpp $OPUSLIB -pthread -o /tmp/vc_t5 && /tmp/vc_t5
(cd relay && python3 test_server.py)
python3 tests/relay_integration.py | tail -1
python3 tests/voice_e2e.py
[ -d bridge/test ] && command -v javac >/dev/null && sh bridge/test/run_java_tests.sh || true
