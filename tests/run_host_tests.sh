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
g++ -std=c++20 $SAN -Itests/host_opus -Isrc tests/voice_pipeline_test.cpp src/voice/VoiceSender.cpp src/voice/RemoteVoices.cpp $OPUSLIB -pthread -o /tmp/vc_t5 && /tmp/vc_t5
(cd relay && python3 test_server.py)
python3 tests/relay_integration.py | tail -1
python3 tests/voice_e2e.py
