"""Run the actual mod wiring against a real relay and simulated Android audio/event APIs.

Build the CMake target voicechat_mod_integration_test with
-DVOICECHAT_BUILD_HOST_INTEGRATION=ON, then:
    python3 tests/voice_mod_integration.py path/to/voicechat_mod_integration_test
"""
import os
import socket
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: voice_mod_integration.py path/to/voicechat_mod_integration_test")
    exe = os.path.abspath(sys.argv[1])
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        s.bind(("127.0.0.1", 0))
        port = s.getsockname()[1]
    server = subprocess.Popen(
        [sys.executable, "server.py", "--host", "127.0.0.1", "--port", str(port), "--secret", "secret"],
        cwd=os.path.join(ROOT, "relay"), stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
    )
    try:
        ready = server.stdout.readline()
        if "voice relay on" not in ready:
            raise RuntimeError("relay failed to start: " + ready + server.stdout.read())
        with tempfile.TemporaryDirectory(prefix="voicechat-mod-test-") as temp:
            result = subprocess.run([exe, str(port), temp], text=True, timeout=60)
        return result.returncode
    finally:
        server.terminate()
        server.wait(timeout=5)


if __name__ == "__main__":
    sys.exit(main())
