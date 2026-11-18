"""Builds and runs the full-voice-path test against the real relay.   python3 tests/voice_e2e.py"""
import os, socket, subprocess, sys
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); s.bind(("127.0.0.1", 0)); port = s.getsockname()[1]; s.close()
opus = os.environ.get("OPUSLIB", "-l:libopus.so.0")
subprocess.check_call(["g++", "-std=c++20", "-g", "-O1", "-fsanitize=address,undefined", "-I" + ROOT + "/tests/host_opus",
                       "-I" + ROOT + "/src", ROOT + "/tests/voice_e2e_test.cpp", ROOT + "/src/voice/RelayClient.cpp",
                       ROOT + "/src/voice/RemoteVoices.cpp", ROOT + "/src/voice/VoiceSender.cpp", opus, "-pthread", "-o", "/tmp/voice_e2e"])
srv = subprocess.Popen([sys.executable, "server.py", "--host", "127.0.0.1", "--port", str(port), "--radius", "32", "--secret", "k"],
                       cwd=ROOT + "/relay", stdout=subprocess.PIPE, text=True)
srv.stdout.readline()
try:
    r = subprocess.run(["/tmp/voice_e2e", str(port), "k"], capture_output=True, text=True, timeout=60)
    print(r.stdout.strip(), r.stderr.strip()[:300])
    sys.exit(0 if r.returncode == 0 and "VOICE E2E OK" in r.stdout else 1)
finally:
    srv.terminate()
