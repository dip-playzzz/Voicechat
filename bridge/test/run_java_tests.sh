#!/bin/sh
# PC-side tests for the bridge. Needs a JDK 21 and python3.
set -e
cd "$(dirname "$0")/.."
SVC_API="${SVC_API_SRC:-/tmp/svc/api/src/main/java}"   # git clone https://github.com/henkelmax/simple-voice-chat (api/ folder)
if [ -d "$SVC_API" ] && [ -d /tmp/pstubs ]; then
  rm -rf /tmp/bridge_classes && mkdir /tmp/bridge_classes
  javac -d /tmp/bridge_classes $(find "$SVC_API" -name '*.java' | grep -v package-info) $(find /tmp/pstubs -name '*.java') $(find src/main/java -name '*.java') test/ParticipantTest.java
  java -cp /tmp/bridge_classes dev.voicebridge.ParticipantTest
fi
python3 test/relay_link_it.py
