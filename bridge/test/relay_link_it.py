"""Runs the Java RelayLink against the real relay/server.py, including a server restart.
   python3 test/relay_link_it.py   (from the VoiceChatBridge folder; needs javac and the mod project's relay/ folder)"""
import os, socket, subprocess, sys, time

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RELAY = os.environ.get("RELAY_DIR") or (os.path.join(HERE, "..", "relay") if os.path.isdir(os.path.join(HERE, "..", "relay")) else os.path.join(HERE, "..", "VoiceChatMod", "relay"))
SECRET = "test-secret"

def free_port():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); s.bind(("127.0.0.1", 0)); p = s.getsockname()[1]; s.close(); return p

def start_server(port):
    p = subprocess.Popen([sys.executable, "server.py", "--host", "127.0.0.1", "--port", str(port), "--radius", "32",
                          "--secret", SECRET], cwd=RELAY, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    p.stdout.readline(); return p

out_dir = "/tmp/relaylink_classes"
os.makedirs(out_dir, exist_ok=True)
src = os.path.join(HERE, "src/main/java/dev/voicebridge/relay")
subprocess.check_call(["javac", "-d", out_dir] + [os.path.join(src, f) for f in os.listdir(src) if f.endswith(".java")]
                      + [os.path.join(HERE, "test/RelayLinkTest.java")])
port = free_port(); server = start_server(port)
t = subprocess.Popen(["java", "-cp", out_dir, "RelayLinkTest", str(port), SECRET], stdin=subprocess.PIPE,
                     stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
lines = []
try:
    deadline = time.time() + 90
    for line in t.stdout:
        line = line.rstrip(); lines.append(line); print(line)
        if line.startswith("PHASE1_OK"):
            server.terminate(); server.wait(); time.sleep(1.0)
            server = start_server(port)
            t.stdin.write("RESTARTED\n"); t.stdin.flush()
        if time.time() > deadline:
            t.kill(); print("TIMEOUT"); sys.exit(1)
    rc = t.wait()
finally:
    server.terminate()
ok = rc == 0 and any(l.startswith("PHASE2_OK") for l in lines)
print("JAVA RELAY LINK", "PASSED" if ok else "FAILED")
sys.exit(0 if ok else 1)
