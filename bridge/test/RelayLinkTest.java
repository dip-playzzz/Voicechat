import dev.voicebridge.relay.*;

import java.io.BufferedReader;
import java.io.InputStreamReader;
import java.util.List;
import java.util.concurrent.CopyOnWriteArrayList;
import java.util.function.BooleanSupplier;

/** Java client vs the real Python relay. usage: RelayLinkTest <port> <secret> (see relay_link_it.py) */
public class RelayLinkTest {
    static void check(boolean ok, String msg) {
        if (!ok) { System.out.println("FAIL: " + msg); System.out.flush(); System.exit(1); }
    }
    static boolean waitFor(BooleanSupplier p, long ms) throws Exception {
        long end = System.currentTimeMillis() + ms;
        while (System.currentTimeMillis() < end) { if (p.getAsBoolean()) return true; Thread.sleep(20); }
        return p.getAsBoolean();
    }
    static final class Peer {
        final List<ForwardedVoice> heard = new CopyOnWriteArrayList<>();
        final RelayLink link;
        Peer(int id, int port, String secret, String name) {
            link = new RelayLink(new RelayConfig("127.0.0.1", port, "room1", secret), id,
                    m -> System.out.println("  [" + name + "] " + m), heard::add);
        }
    }

    public static void main(String[] a) throws Exception {
        int port = Integer.parseInt(a[0]);
        String secret = a[1];
        Peer A = new Peer(1, port, secret, "A"), B = new Peer(2, port, secret, "B"),
             C = new Peer(3, port, secret, "C"), D = new Peer(4, port, "WRONG", "D");
        Peer[] all = {A, B, C, D};
        for (Peer p : all) p.link.start();

        check(waitFor(() -> A.link.isConnected() && B.link.isConnected() && C.link.isConnected(), 5000),
                "A, B, C connect with the right secret");
        Thread.sleep(1500);
        check(!D.link.isConnected(), "wrong secret must not connect");

        Runnable place = () -> {
            A.link.sendPosition(0, 0, 64, 0, 0);
            B.link.sendPosition(0, 10, 64, 0, 0);
            C.link.sendPosition(0, 100, 64, 0, 0);
        };
        place.run();
        Thread.sleep(300);

        byte[] opus = {9, 8, 7, 6, 5};
        check(A.link.sendVoice(42, 0, 0, 64, 0, 90f, opus), "A sendVoice");
        check(waitFor(() -> B.heard.size() >= 1, 2000), "B (10 blocks) hears A");
        Thread.sleep(300);
        check(B.heard.size() == 1, "exactly one packet");
        ForwardedVoice v = B.heard.get(0);
        check(v.senderId() == 1 && v.seq() == 42 && v.dim() == 0 && v.yaw() == 90f && v.x() == 0f && v.y() == 64f
                && java.util.Arrays.equals(v.opus(), opus), "forwarded fields intact");
        check(C.heard.isEmpty(), "C (100 blocks) hears nothing");
        check(A.heard.isEmpty(), "A does not hear itself");

        B.heard.clear();
        B.link.sendPosition(1, 10, 64, 0, 0);          // B moves to the Nether
        Thread.sleep(300);
        A.link.sendVoice(43, 0, 0, 64, 0, 0, opus);
        Thread.sleep(500);
        check(B.heard.isEmpty(), "other dimension is silent");
        B.link.sendPosition(0, 10, 64, 0, 0);
        Thread.sleep(300);

        System.out.println("PHASE1_OK"); System.out.flush();

        BufferedReader in = new BufferedReader(new InputStreamReader(System.in));
        check("RESTARTED".equals(in.readLine()), "harness handshake");
        check(waitFor(() -> A.link.connectCount() > 1 && B.link.connectCount() > 1 && A.link.isConnected() && B.link.isConnected(), 20000),
                "A and B reconnect after the server restarts");
        place.run();
        Thread.sleep(400);
        B.heard.clear();
        A.link.sendVoice(44, 0, 0, 64, 0, 0, opus);
        check(waitFor(() -> B.heard.size() >= 1, 2000), "voice flows again after reconnect");

        System.out.println("PHASE2_OK"); System.out.flush();
        for (Peer p : all) p.link.close();
    }
}
