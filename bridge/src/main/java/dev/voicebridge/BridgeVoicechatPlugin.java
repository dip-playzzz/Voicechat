package dev.voicebridge;

import de.maxhenkel.voicechat.api.VoicechatConnection;
import de.maxhenkel.voicechat.api.VoicechatPlugin;
import de.maxhenkel.voicechat.api.VoicechatServerApi;
import de.maxhenkel.voicechat.api.events.EventRegistration;
import de.maxhenkel.voicechat.api.events.MicrophonePacketEvent;
import de.maxhenkel.voicechat.api.events.PlayerConnectedEvent;
import de.maxhenkel.voicechat.api.events.PlayerDisconnectedEvent;
import de.maxhenkel.voicechat.api.events.VoicechatServerStartedEvent;
import de.maxhenkel.voicechat.api.packets.LocationalSoundPacket;
import de.maxhenkel.voicechat.api.packets.MicrophonePacket;
import dev.voicebridge.relay.ForwardedVoice;
import dev.voicebridge.relay.RelayConfig;
import dev.voicebridge.relay.RelayLink;

import java.nio.charset.StandardCharsets;
import java.util.UUID;
import java.util.concurrent.ConcurrentHashMap;
import java.util.logging.Logger;

/**
 * Glue between Simple Voice Chat and the relay.
 *   Java player's mic  -> relay (with their position)      so Bedrock mod users nearby hear them
 *   relay (Bedrock)    -> that Java player as a positioned SVC sound
 * Opus packets are passed through untouched: Opus decoders accept packets from any encoder sample rate.
 */
public final class BridgeVoicechatPlugin implements VoicechatPlugin {
    private final Logger log;
    private final RelayConfig relay;
    private final float radius;
    final ConcurrentHashMap<UUID, Participant> participants = new ConcurrentHashMap<>();
    private final ConcurrentHashMap<Integer, UUID> senderUuids = new ConcurrentHashMap<>();
    private volatile VoicechatServerApi api;

    BridgeVoicechatPlugin(Logger log, RelayConfig relay, float radius) {
        this.log = log;
        this.relay = relay;
        this.radius = radius;
    }

    @Override public String getPluginId() { return "voicechat_bridge"; }

    @Override
    public void registerEvents(EventRegistration r) {
        r.registerEvent(VoicechatServerStartedEvent.class, e -> api = e.getVoicechat());
        r.registerEvent(PlayerConnectedEvent.class, this::onConnected);
        r.registerEvent(PlayerDisconnectedEvent.class, this::onDisconnected);
        r.registerEvent(MicrophonePacketEvent.class, this::onMic);
    }

    // ---------------------------------------------------------------- players come and go

    private void onConnected(PlayerConnectedEvent e) {
        if (api == null) api = e.getVoicechat();
        VoicechatConnection c = e.getConnection();
        UUID uuid = c.getPlayer().getUuid();
        int salt = 0;
        int id = Participant.idFor(uuid, salt);
        while (isIdTaken(id)) id = Participant.idFor(uuid, ++salt);

        Participant p = new Participant(uuid, id, c);
        RelayLink link = new RelayLink(relay, id, m -> log.info("[" + uuid.toString().substring(0, 8) + "] " + m),
                v -> onRelayVoice(p, v));
        p.link = link;
        Participant old = participants.put(uuid, p);
        if (old != null) old.close();
        link.start();
    }

    private boolean isIdTaken(int id) {
        for (Participant q : participants.values()) if (q.relayId == id) return true;
        return false;
    }

    private void onDisconnected(PlayerDisconnectedEvent e) {
        Participant p = participants.remove(e.getPlayerUuid());
        if (p != null) p.close();
    }

    void closeAll() {
        for (Participant p : participants.values()) p.close();
        participants.clear();
    }

    /** Called ~10x/s from the server thread with each player's current position. */
    void updatePosition(UUID uuid, int dim, float x, float y, float z, float yaw) {
        Participant p = participants.get(uuid);
        if (p == null) return;
        p.spot = new Participant.Spot(true, dim, x, y, z, yaw);
        RelayLink l = p.link;
        if (l != null) l.sendPosition(dim, x, y, z, yaw);
    }

    // ---------------------------------------------------------------- Java player speaks -> relay

    private void onMic(MicrophonePacketEvent e) {
        VoicechatConnection c = e.getSenderConnection();
        if (c == null) return;
        Participant p = participants.get(c.getPlayer().getUuid());
        if (p == null) return;
        MicrophonePacket pk = e.getPacket();
        byte[] opus = pk.getOpusEncodedData();
        // Whispers and private groups must not leak onto the proximity relay; empty = end-of-speech marker.
        if (opus == null || opus.length == 0 || pk.isWhispering() || c.isInGroup() || c.isDisabled()) return;
        Participant.Spot s = p.spot;
        RelayLink l = p.link;
        if (!s.valid() || l == null) return;
        l.sendVoice(p.micSeq.getAndIncrement() & 0xffff, s.dim(), s.x(), s.y(), s.z(), s.yaw(), opus);
    }

    // ---------------------------------------------------------------- relay speaker -> this Java player

    private void onRelayVoice(Participant listener, ForwardedVoice v) {
        VoicechatServerApi server = api;
        if (server == null) return;
        // Voices from other Java players already reach this player through Simple Voice Chat itself.
        if ((v.senderId() & Participant.BRIDGE_BIT) != 0) return;
        try {
            UUID speaker = senderUuids.computeIfAbsent(v.senderId(),
                    id -> UUID.nameUUIDFromBytes(("voicechat-bridge:" + id).getBytes(StandardCharsets.UTF_8)));
            LocationalSoundPacket packet = server.createPacket().locationalSoundPacketBuilder()
                    .channelId(speaker)
                    .sender(speaker)
                    .opusEncodedData(v.opus())
                    .sequenceNumber(listener.unwrap(v.senderId(), v.seq()))
                    .position(server.createPosition(v.x(), v.y(), v.z()))
                    .distance(radius)
                    .build();
            server.sendLocationalSoundPacketTo(listener.connection, packet);
        } catch (RuntimeException ex) {
            log.warning("Could not deliver relay voice: " + ex);
        }
    }
}
