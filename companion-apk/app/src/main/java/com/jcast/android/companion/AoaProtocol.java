package com.jcast.android.companion;

import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.StandardCharsets;

public final class AoaProtocol {

    public static final int MAGIC = 0x4A43414F; // 'JCAO'
    public static final byte VERSION = 1;

    // PC -> Android control commands (24-byte fixed frame)
    public static final int CTRL_FRAME_BYTES = 24;
    public static final byte CMD_START_STREAM = 0x01;
    public static final byte CMD_STOP_STREAM  = 0x02;
    public static final byte CMD_PING         = 0x03;

    public static final int CTRL_FLAG_ENABLE_AUDIO     = 1 << 0;
    public static final int CTRL_FLAG_LOCK_ORIENTATION = 1 << 1;

    // Android -> PC multiplexed stream packet types (20-byte header + payload)
    public static final int STREAM_HEADER_BYTES = 20;
    public static final byte PKT_HELLO_DEVICE_META    = 0x10;
    public static final byte PKT_VIDEO_CONFIG_SPS_PPS = 0x20;
    public static final byte PKT_VIDEO_H264_KEYFRAME  = 0x21;
    public static final byte PKT_VIDEO_H264_FRAME     = 0x22;
    public static final byte PKT_VIDEO_SIZE_CHANGED   = 0x23;
    public static final byte PKT_AUDIO_PCM_S16LE      = 0x30;
    public static final byte PKT_AUDIO_UNSUPPORTED    = 0x31;
    public static final byte PKT_HEARTBEAT            = 0x40;

    public static final class StartConfig {
        public int maxFps = 60;
        public int maxDim = 2560;
        public int videoBitrateBps = 20_000_000;
        public boolean enableAudio = true;
        public boolean lockOrientation = true;
    }

    private AoaProtocol() {}

    public static void writePacket(
            OutputStream out,
            Object writeLock,
            byte pktType,
            long ptsUsOrMeta,
            byte[] payload,
            int offset,
            int length) throws IOException {
        ByteBuffer hdr = ByteBuffer.allocate(STREAM_HEADER_BYTES).order(ByteOrder.BIG_ENDIAN);
        hdr.putInt(MAGIC);
        hdr.put(pktType);
        hdr.put((byte) 0);
        hdr.putShort((short) 0);
        hdr.putLong(ptsUsOrMeta);
        hdr.putInt(length);

        synchronized (writeLock) {
            out.write(hdr.array(), 0, STREAM_HEADER_BYTES);
            if (length > 0 && payload != null) {
                out.write(payload, offset, length);
            }
            out.flush();
        }
    }

    public static void writeHelloDeviceMeta(
            OutputStream out,
            Object writeLock,
            String modelName,
            int sdkInt,
            int width,
            int height) throws IOException {
        ByteBuffer body = ByteBuffer.allocate(76).order(ByteOrder.BIG_ENDIAN);
        byte[] nameBytes = (modelName != null ? modelName : "Android Device")
                .getBytes(StandardCharsets.UTF_8);
        int copyLen = Math.min(nameBytes.length, 63);
        body.put(nameBytes, 0, copyLen);
        body.position(64);
        body.putInt(sdkInt);
        body.putInt(width);
        body.putInt(height);

        writePacket(out, writeLock, PKT_HELLO_DEVICE_META, 0L, body.array(), 0, 76);
    }

    public static void writeSizeChanged(
            OutputStream out,
            Object writeLock,
            int width,
            int height) throws IOException {
        ByteBuffer body = ByteBuffer.allocate(8).order(ByteOrder.BIG_ENDIAN);
        body.putInt(width);
        body.putInt(height);
        writePacket(out, writeLock, PKT_VIDEO_SIZE_CHANGED, 0L, body.array(), 0, 8);
    }

    public static boolean readExact(InputStream in, byte[] buf, int len) throws IOException {
        int pos = 0;
        while (pos < len) {
            int nr = in.read(buf, pos, len - pos);
            if (nr <= 0) {
                return false;
            }
            pos += nr;
        }
        return true;
    }

    public static byte parseControlFrame(byte[] frame24, StartConfig outCfg) {
        if (frame24 == null || frame24.length < CTRL_FRAME_BYTES) {
            return 0;
        }
        ByteBuffer bb = ByteBuffer.wrap(frame24).order(ByteOrder.BIG_ENDIAN);
        int magic = bb.getInt();
        if (magic != MAGIC) {
            return 0;
        }
        byte ver = bb.get();
        if (ver != VERSION) {
            return 0;
        }
        byte cmd = bb.get();
        int fps = bb.getShort() & 0xFFFF;
        int maxDim = bb.getInt();
        int bitrate = bb.getInt();
        int flags = bb.get() & 0xFF;

        if (cmd == CMD_START_STREAM && outCfg != null) {
            if (fps >= 24 && fps <= 144) outCfg.maxFps = fps;
            if (maxDim >= 480 && maxDim <= 3840) outCfg.maxDim = maxDim;
            if (bitrate >= 1_000_000 && bitrate <= 80_000_000) outCfg.videoBitrateBps = bitrate;
            outCfg.enableAudio = (flags & CTRL_FLAG_ENABLE_AUDIO) != 0;
            outCfg.lockOrientation = (flags & CTRL_FLAG_LOCK_ORIENTATION) != 0;
        }
        return cmd;
    }
}
