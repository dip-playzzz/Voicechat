package dev.voicebridge;

import de.maxhenkel.voicechat.api.BukkitVoicechatService;
import dev.voicebridge.relay.RelayConfig;
import org.bukkit.Location;
import org.bukkit.World;
import org.bukkit.entity.Player;
import org.bukkit.plugin.java.JavaPlugin;

import java.util.UUID;

/** Paper plugin entry point. Needs Simple Voice Chat. Not Folia-compatible (uses the main-thread scheduler). */
public final class BridgePlugin extends JavaPlugin {
    private BridgeVoicechatPlugin bridge;

    @Override
    public void onEnable() {
        saveDefaultConfig();
        String secret = getConfig().getString("relay.secret", "");
        if (secret == null || secret.isBlank()) {
            getLogger().warning("relay.secret is empty in plugins/VoiceChatBridge/config.yml - bridge stays off until you set it.");
            getServer().getPluginManager().disablePlugin(this);
            return;
        }
        RelayConfig relay = new RelayConfig(
                getConfig().getString("relay.host", "127.0.0.1"),
                getConfig().getInt("relay.port", 24454),
                getConfig().getString("relay.room", "default"),
                secret);
        float radius = (float) getConfig().getDouble("radius", 32.0);

        BukkitVoicechatService service = getServer().getServicesManager().load(BukkitVoicechatService.class);
        if (service == null) {
            getLogger().severe("Simple Voice Chat was not found (is it installed and 2.5+?). Disabling.");
            getServer().getPluginManager().disablePlugin(this);
            return;
        }
        bridge = new BridgeVoicechatPlugin(getLogger(), relay, radius);
        service.registerPlugin(bridge);

        // Positions must be read on the server thread: every 2 ticks = 10 per second.
        getServer().getScheduler().runTaskTimer(this, this::sendPositions, 40L, 2L);
        getLogger().info("Voice bridge ready: relay " + relay.host() + ":" + relay.port() + ", room '" + relay.room() + "', radius " + radius);
    }

    private void sendPositions() {
        for (UUID id : bridge.participants.keySet()) {
            Player p = getServer().getPlayer(id);
            if (p == null) continue;
            Location l = p.getLocation();
            World w = l.getWorld();
            int dim = 0;
            if (w != null) {
                switch (w.getEnvironment()) {
                    case NETHER -> dim = 1;
                    case THE_END -> dim = 2;
                    default -> dim = 0;
                }
            }
            bridge.updatePosition(id, dim, (float) l.getX(), (float) l.getY(), (float) l.getZ(), l.getYaw());
        }
    }

    @Override
    public void onDisable() {
        if (bridge != null) bridge.closeAll();
    }
}
