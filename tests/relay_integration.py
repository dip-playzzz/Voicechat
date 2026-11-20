"""Builds and runs the C++ RelayClient against the real Python relay, including a server restart.
   python3 tests/relay_integration.py            (from the project root)"""
import os, socket, subprocess, sys, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SECRET = "test-secret"

def free_port():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); s.bind(("127.0.0.1", 0)); p = s.getsockname()[1]; s.close(); return p

def start_server(port):
    p = subprocess.Popen([sys.executable, "server.py", "--host", "127.0.0.1", "--port", str(port),
                          "--radius", "32", "--secret", SECRET], cwd=os.path.join(ROOT, "relay"),
                         stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    p.stdout.readline()   # "voice relay on ..."
    return p

def main():
    exe = "/tmp/relay_client_test"
    subprocess.check_call(["g++", "-std=c++20", "-g", "-O1", "-fsanitize=address,undefined", "-Wall",
                           "-I" + os.path.join(ROOT, "src"),
                           os.path.join(ROOT, "tests/relay_client_test.cpp"),
                           os.path.join(ROOT, "src/voice/RelayClient.cpp"), "-pthread", "-o", exe])
    port = free_port()
    server = start_server(port)
    t = subprocess.Popen([exe, str(port), SECRET], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                         stderr=subprocess.STDOUT, text=True)
    out = []
    try:
        deadline = time.time() + 90
        while True:
            line = t.stdout.readline()
            if not line:
                break
            out.append(line.rstrip()); print(line.rstrip())
            if line.startswith("PHASE1_OK"):
                server.terminate(); server.wait(); time.sleep(1.0)
                server = start_server(port)
                t.stdin.write("RESTARTED\n"); t.stdin.flush()
            if time.time() > deadline:
                t.kill(); print("TIMEOUT"); return 1
        rc = t.wait()
    finally:
        server.terminate()
    ok = rc == 0 and any(l.startswith("PHASE2_OK") for l in out)
    print("RELAY INTEGRATION", "PASSED" if ok else "FAILED")
    return 0 if ok else 1

if __name__ == "__main__":
    sys.exit(main())
