package dev.voicebridge.relay;

/** One voice packet the relay forwarded to us (the speaker's position is where they were standing). */
public record ForwardedVoice(int senderId, int seq, int dim, float x, float y, float z, float yaw, byte[] opus) {}
