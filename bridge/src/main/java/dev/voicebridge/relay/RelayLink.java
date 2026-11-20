package dev.voicebridge.relay;

import javax.crypto.Mac;
import javax.crypto.spec.SecretKeySpec;
import java.io.IOException;
import java.net.DatagramPacket;
import java.net.DatagramSocket;
import java.net.InetAddress;
import java.net.SocketTimeoutException;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.StandardCharsets;
import java.security.GeneralSecurityException;
import java.security.SecureRandom;
import java.util.Arrays;
import java.util.function.Consumer;

/**
 * One participant's UDP connection to the voice relay (protocol v2, relay/protocol.py).
 * Connects, keeps the session alive, reconnects on its own, and passes forwarded voice to a handler.
 * All send methods are safe to call from any thread.
 */
public final class RelayLink implements AutoCloseable {
    public interface VoiceHandler { void onVoice(ForwardedVoice v); }

    private static final byte VERSION = 2;
    private static final int HELLO = 1, VOICE = 2, POS = 3, BYE = 4, ACCEPT = 5, REJECT = 6, VOICE_FWD = 7, PING = 8;
    private static final String[] REASONS = {"?", "wrong room secret", "clock off by too much (or replayed hello)", "room full",
            "rate limited", "server full", "malformed hello", "server restarted (reconnecting)"};

    private final RelayConfig cfg;
    private final int playerId;
    private final Consumer<String> log;
    private final VoiceHandler handler;
    private final SecureRandom rng = new SecureRandom();

    private volatile boolean stop;
    private volatile boolean connected;
    private volatile boolean helloNow;
    private volatile long session;
    private volatile DatagramSocket socket;
    private volatile int connectCount;
    private Thread worker;
    private String lastNoteKey = "";
    private long lastNoteAt;

    public RelayLink(RelayConfig cfg, int playerId, Consumer<String> log, VoiceHandler handler) {
        this.cfg = cfg;
        this.playerId = playerId;
        this.log = log == null ? s -> {} : log;
        this.handler = handler;
    }

    public synchronized void start() {
        if (worker != null) return;
        stop = false;
        worker = new Thread(this::run, "voice-relay-" + Integer.toUnsignedString(playerId));
        worker.setDaemon(true);
        worker.start();
    }

    public boolean isConnected() { return connected; }
    public int connectCount() { return connectCount; }
    public int playerId() { return playerId; }

    @Override
    public void close() {
        stop = true;
        Thread w;
        synchronized (this) { w = worker; worker = null; }
        if (w != null) {
            try { w.join(2000); } catch (InterruptedException e) { Thread.currentThread().interrupt(); }
        }
        if (connected) {
            connected = false;
            send(header(BYE, 8).putLong(session));
        }
        closeSocket();
    }

    // ------------------------------------------------------------------ sending

    public boolean sendPosition(int dim, float x, float y, float z, float yaw) {
        if (!connected) return false;
        return send(header(POS, 8 + 4 + 16).putLong(session).putInt(dim).putFloat(x).putFloat(y).putFloat(z).putFloat(yaw));
    }

    public boolean sendVoice(int seq, int dim, float x, float y, float z, float yaw, byte[] opus) {
        if (!connected || opus == null || opus.length == 0 || opus.length > 400) return false;
        ByteBuffer b = header(VOICE, 8 + 2 + 4 + 16 + 2 + opus.length)
                .putLong(session).putShort((short) seq).putInt(dim)
                .putFloat(x).putFloat(y).putFloat(z).putFloat(yaw)
                .putShort((short) opus.length).put(opus);
        return send(b);
    }

    private static ByteBuffer header(int type, int bodyLen) {
        ByteBuffer b = ByteBuffer.allocate(4 + bodyLen).order(ByteOrder.LITTLE_ENDIAN);
        return b.put((byte) 'V').put((byte) 'C').put(VERSION).put((byte) type);
    }

    private boolean send(ByteBuffer b) {
        DatagramSocket s = socket;
        if (s == null) return false;
        try {
            s.send(new DatagramPacket(b.array(), b.position()));
            return true;
        } catch (IOException e) {
            return false;
        }
    }

    private void sendHello() {
        byte[] room = cfg.room().getBytes(StandardCharsets.UTF_8);
        int ts = (int) (System.currentTimeMillis() / 1000);
        long nonce = rng.nextLong();

        ByteBuffer m = ByteBuffer.allocate(8 + 4 + room.length + 4 + 8).order(ByteOrder.LITTLE_ENDIAN);
        m.put("VC2HELLO".getBytes(StandardCharsets.US_ASCII)).putInt(playerId).put(room).putInt(ts).putLong(nonce);
        byte[] mac;
        try {
            Mac h = Mac.getInstance("HmacSHA256");
            h.init(new SecretKeySpec(cfg.secret().getBytes(StandardCharsets.UTF_8), "HmacSHA256"));
            mac = Arrays.copyOf(h.doFinal(m.array()), 16);
        } catch (GeneralSecurityException e) {
            throw new IllegalStateException(e);
        }
        send(header(HELLO, 4 + 1 + room.length + 4 + 8 + 16).putInt(playerId).put((byte) room.length).put(room)
                .putInt(ts).putLong(nonce).put(mac));
    }

    // ------------------------------------------------------------------ receiving

    private void handle(byte[] d, int n) {
        if (n < 4 || d[0] != 'V' || d[1] != 'C' || d[2] != VERSION) return;
        ByteBuffer b = ByteBuffer.wrap(d, 4, n - 4).order(ByteOrder.LITTLE_ENDIAN);
        try {
            switch (d[3]) {
                case ACCEPT -> {
                    session = b.getLong();
                    if (!connected) {
                        connected = true;
                        connectCount++;
                        log.accept("Relay: connected to " + cfg.host() + ":" + cfg.port() + " room '" + cfg.room() + "'");
                    }
                }
                case REJECT -> {
                    int reason = b.get() & 0xff;
                    long serverTs = b.getInt() & 0xffffffffL;
                    String text = reason < REASONS.length ? REASONS[reason] : "unknown reason";
                    if (reason == 7) {
                        if (connected) { connected = false; helloNow = true; log.accept("Relay: " + text); }
                    } else {
                        String extra = reason == 2 ? " (clock off by " + (System.currentTimeMillis() / 1000 - serverTs) + " s)" : "";
                        note("rej" + reason, "Relay rejected us: " + text + extra);
                    }
                }
                case VOICE_FWD -> {
                    int sender = b.getInt();
                    int seq = b.getShort() & 0xffff;
                    int dim = b.getInt();
                    float x = b.getFloat(), y = b.getFloat(), z = b.getFloat(), yaw = b.getFloat();
                    int len = b.getShort() & 0xffff;
                    if (len == 0 || len > 400 || b.remaining() != len) return;
                    byte[] opus = new byte[len];
                    b.get(opus);
                    if (handler != null) handler.onVoice(new ForwardedVoice(sender, seq, dim, x, y, z, yaw, opus));
                }
                default -> { /* PONG and unknown: only proves the server is alive */ }
            }
        } catch (RuntimeException e) {
            // truncated or malformed packet: ignore
        }
    }

    private void note(String key, String msg) {
        long t = System.currentTimeMillis();
        if (key.equals(lastNoteKey) && t - lastNoteAt < 30_000) return;
        lastNoteKey = key;
        lastNoteAt = t;
        log.accept(msg);
    }

    // ------------------------------------------------------------------ worker

    private boolean open() {
        try {
            InetAddress addr = InetAddress.getByName(cfg.host());
            DatagramSocket s = new DatagramSocket();
            s.connect(addr, cfg.port()); // only accept datagrams from the relay
            s.setSoTimeout(100);
            socket = s;
            return true;
        } catch (IOException e) {
            note("open", "Relay: cannot reach " + cfg.host() + ":" + cfg.port() + " (" + e.getMessage() + ")");
            return false;
        }
    }

    private void closeSocket() {
        DatagramSocket s = socket;
        socket = null;
        if (s != null) s.close();
    }

    private void run() {
        long lastHello = 0, lastPing = System.currentTimeMillis(), lastRecv = System.currentTimeMillis();
        long helloGap = 1000;
        byte[] buf = new byte[2048];
        while (!stop) {
            DatagramSocket s = socket;
            if (s == null) {
                if (!open()) { sleep(3000); continue; }
                s = socket;
                lastHello = 0; helloGap = 1000; lastRecv = System.currentTimeMillis();
            }
            try {
                DatagramPacket p = new DatagramPacket(buf, buf.length);
                s.receive(p);
                lastRecv = System.currentTimeMillis();
                handle(p.getData(), p.getLength());
            } catch (SocketTimeoutException ignored) {
                // normal: wake up to do timers
            } catch (IOException e) {
                if (!stop) closeSocket();
                continue;
            }
            long t = System.currentTimeMillis();
            if (!connected) {
                if (helloNow || t - lastHello >= helloGap) {
                    helloNow = false;
                    sendHello();
                    lastHello = t;
                    helloGap = Math.min(helloGap * 2, 5000); // back off while the server is unreachable
                }
                if (t - lastRecv > 20_000) closeSocket();    // re-resolve DNS / reopen
            } else {
                helloGap = 1000;
                if (t - lastPing >= 2000) { send(header(PING, 8).putLong(session)); lastPing = t; }
                if (t - lastRecv > 7000) {
                    connected = false;
                    log.accept("Relay: lost connection, reconnecting");
                    closeSocket();
                }
            }
        }
    }

    private static void sleep(long ms) {
        try { Thread.sleep(ms); } catch (InterruptedException e) { Thread.currentThread().interrupt(); }
    }
}
