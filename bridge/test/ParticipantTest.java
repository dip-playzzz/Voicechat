package dev.voicebridge;

import java.util.UUID;

/** Pure-logic checks: id ranges and sequence unwrapping. Run via test/run_java_tests.sh */
public class ParticipantTest {
    static void check(boolean ok, String msg) { if (!ok) { System.out.println("FAIL: " + msg); System.exit(1); } }

    public static void main(String[] args) {
        // bridge ids always carry the high bit; Bedrock-mod ids (masked to 31 bits) never do
        for (int i = 0; i < 1000; i++) {
            int id = Participant.idFor(UUID.randomUUID(), 0);
            check((id & Participant.BRIDGE_BIT) != 0, "bridge id has the bridge bit");
        }
        UUID u = UUID.randomUUID();
        check(Participant.idFor(u, 0) != Participant.idFor(u, 1), "salt gives a different id on collision");

        Participant p = new Participant(u, Participant.idFor(u, 0), null);
        check(p.unwrap(7, 65530) == 65530, "first packet keeps its sequence");
        long prev = 65530;
        for (int seq : new int[]{65531, 65532, 65533, 65534, 65535, 0, 1, 2, 3}) {
            long ext = p.unwrap(7, seq);
            check(ext == prev + 1, "sequence continues across the 16-bit wrap: " + seq + " -> " + ext);
            prev = ext;
        }
        long late = p.unwrap(7, 1);               // a late duplicate must not move the counter backwards permanently
        check(late < prev, "late packet maps below the newest");
        check(p.unwrap(7, 4) == prev + 1, "stream continues after a late packet");
        check(p.unwrap(8, 100) == 100, "separate speakers are tracked separately");
        System.out.println("PARTICIPANT TESTS OK");
    }
}
