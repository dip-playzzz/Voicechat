package dev.voicebridge.relay;

public record RelayConfig(String host, int port, String room, String secret) {}
