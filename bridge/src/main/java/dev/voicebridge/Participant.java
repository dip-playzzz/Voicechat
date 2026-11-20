package dev.voicebridge;

import de.maxhenkel.voicechat.api.VoicechatConnection;
import dev.voicebridge.relay.RelayLink;

import java.util.HashMap;
import java.util.Map;
import java.util.UUID;
import java.util.concurrent.atomic.AtomicInteger;

/** One Simple Voice Chat player represented on the relay. */
final class Participant {
    /** Relay ids with this bit set belong to the bridge; Bedrock mod users never use it. */
    static final int BRIDGE_BIT = 0x80000000;

    record Spot(boolean valid, int dim, float x, float y, float z, float yaw) {}

    final UUID uuid;
    final int relayId;
    final VoicechatConnection connection;
    volatile RelayLink link;
    volatile Spot spot = new Spot(false, 0, 0, 0, 0, 0);
    final AtomicInteger micSeq = new AtomicInteger();

    // Used only from this participant's relay thread: turns the relay's 16-bit sequence into a rising 64-bit one.
    private final Map<Integer, Long> lastSeq = new HashMap<>();

    Participant(UUID uuid, int relayId, VoicechatConnection connection) {
        this.uuid = uuid;
        this.relayId = relayId;
        this.connection = connection;
    }

    static int idFor(UUID uuid, int salt) {
        return BRIDGE_BIT | ((uuid.hashCode() + salt) & 0x7fffffff);
    }

    long unwrap(int senderId, int seq16) {
        if (lastSeq.size() > 256) lastSeq.clear();
        long last = lastSeq.getOrDefault(senderId, (long) seq16);
        long ext = last + (short) (seq16 - (short) last);
        if (ext > last || !lastSeq.containsKey(senderId)) lastSeq.put(senderId, ext);
        return ext;
    }

    void close() {
        RelayLink l = link;
        link = null;
        if (l != null) l.close();
    }
}
