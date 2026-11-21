#!/bin/sh
# PC-side tests for the bridge. Needs a JDK 21 and python3.
set -e
cd "$(dirname "$0")/.."
TEST_DIR=$(mktemp -d "${TMPDIR:-/tmp}/voicechat-bridge-tests.XXXXXX")
trap 'rm -rf "$TEST_DIR"' EXIT HUP INT TERM

# Exercise the id/sequence code in every run. The only SVC type it needs is a
# test-only placeholder; no external plugin API or Paper server is required.
mkdir "$TEST_DIR/logic"
javac -d "$TEST_DIR/logic" test/stubs/de/maxhenkel/voicechat/api/VoicechatConnection.java \
  src/main/java/dev/voicebridge/relay/*.java src/main/java/dev/voicebridge/Participant.java test/ParticipantTest.java
java -cp "$TEST_DIR/logic" dev.voicebridge.ParticipantTest

# Optionally compile the entire plugin against checked-out real SVC sources and
# Paper stand-ins; this still does not replace a real server/plugin build.
SVC_API="${SVC_API_SRC:-/tmp/svc/api/src/main/java}"
if [ -d "$SVC_API" ] && [ -d /tmp/pstubs ]; then
  mkdir "$TEST_DIR/plugin"
  javac -d "$TEST_DIR/plugin" $(find "$SVC_API" -name '*.java' | grep -v package-info) $(find /tmp/pstubs -name '*.java') $(find src/main/java -name '*.java')
  echo "Full bridge plugin compile check passed (Paper stand-ins)"
else
  echo "(skipping full bridge plugin compile: SVC sources and Paper stubs not provided)"
fi
python3 test/relay_link_it.py
