package com.tjcap.android.companion;

import android.app.Activity;
import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Context;
import android.content.Intent;
import android.content.pm.ServiceInfo;
import android.hardware.display.DisplayManager;
import android.hardware.display.VirtualDisplay;
import android.hardware.usb.UsbAccessory;
import android.hardware.usb.UsbManager;
import android.media.AudioAttributes;
import android.media.AudioFormat;
import android.media.AudioPlaybackCaptureConfiguration;
import android.media.AudioRecord;
import android.media.MediaCodec;
import android.media.MediaCodecInfo;
import android.media.MediaFormat;
import android.media.projection.MediaProjection;
import android.media.projection.MediaProjectionManager;
import android.os.Build;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.os.ParcelFileDescriptor;
import android.util.DisplayMetrics;
import android.view.Surface;
import android.view.WindowManager;

import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.ByteBuffer;
import java.util.concurrent.atomic.AtomicBoolean;

public final class TJCapCaptureService extends Service {

    public static final String ACTION_START = "com.tjcap.android.companion.ACTION_START";
    public static final String ACTION_STOP  = "com.tjcap.android.companion.ACTION_STOP";
    public static final String ACTION_STATE_BROADCAST = "com.tjcap.android.companion.STATE";

    public static final String EXTRA_RESULT_CODE  = "result_code";
    public static final String EXTRA_RESULT_DATA  = "result_data";
    public static final String EXTRA_ENABLE_AUDIO = "enable_audio";
    public static final String EXTRA_MAX_DIM      = "max_dim";
    public static final String EXTRA_MAX_FPS      = "max_fps";

    private static final String CHANNEL_ID = "tjcap_usb_capture";
    private static final int NOTIF_ID = 2026;

    private static volatile boolean sRunning = false;
    private static volatile String sStatusText = "待命中";

    public static boolean isRunning() {
        return sRunning;
    }

    public static String getStatusText() {
        return sStatusText;
    }

    private final AtomicBoolean stopFlag = new AtomicBoolean(false);
    private final Object usbWriteLock = new Object();

    private ParcelFileDescriptor accessoryPfd;
    private MediaProjection mediaProjection;
    private VirtualDisplay virtualDisplay;
    private MediaCodec videoEncoder;
    private Surface inputSurface;
    private Thread videoThread;
    private Thread audioThread;
    private Thread controlThread;

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        if (intent == null || ACTION_STOP.equals(intent.getAction())) {
            stopSelfCleanly("已手動停止 USB-C 畫面傳輸");
            return START_NOT_STICKY;
        }

        if (ACTION_START.equals(intent.getAction())) {
            int resultCode = intent.getIntExtra(EXTRA_RESULT_CODE, Activity.RESULT_CANCELED);
            Intent resultData = getParcelableIntentCompat(intent, EXTRA_RESULT_DATA);
            boolean userAudioPref = intent.getBooleanExtra(EXTRA_ENABLE_AUDIO, true);
            int defaultMaxDim = intent.getIntExtra(EXTRA_MAX_DIM, 2560);
            int defaultMaxFps = intent.getIntExtra(EXTRA_MAX_FPS, 60);

            startForegroundWithType();

            if (resultData == null) {
                stopSelfCleanly("未取得螢幕錄製授權");
                return START_NOT_STICKY;
            }

            startStreamingPipeline(resultCode, resultData, userAudioPref, defaultMaxDim, defaultMaxFps);
        }
        return START_NOT_STICKY;
    }

    @SuppressWarnings("deprecation")
    private static Intent getParcelableIntentCompat(Intent src, String key) {
        if (Build.VERSION.SDK_INT >= 33) {
            return src.getParcelableExtra(key, Intent.class);
        }
        return src.getParcelableExtra(key);
    }

    private void startForegroundWithType() {
        NotificationManager nm = (NotificationManager) getSystemService(Context.NOTIFICATION_SERVICE);
        if (Build.VERSION.SDK_INT >= 26 && nm != null) {
            NotificationChannel ch = new NotificationChannel(
                    CHANNEL_ID,
                    "TJCap USB-C 螢幕擷取服務",
                    NotificationManager.IMPORTANCE_LOW);
            ch.setDescription("透過 USB-C AOA 配件通道將畫面低延遲傳送至電腦 TJCap for Android");
            nm.createNotificationChannel(ch);
        }

        Intent stopIntent = new Intent(this, TJCapCaptureService.class);
        stopIntent.setAction(ACTION_STOP);
        PendingIntent stopPi = PendingIntent.getService(
                this,
                1,
                stopIntent,
                PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE);

        Intent openIntent = new Intent(this, MainActivity.class);
        PendingIntent openPi = PendingIntent.getActivity(
                this,
                2,
                openIntent,
                PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE);

        Notification.Builder nb = (Build.VERSION.SDK_INT >= 26)
                ? new Notification.Builder(this, CHANNEL_ID)
                : new Notification.Builder(this);

        nb.setSmallIcon(android.R.drawable.presence_video_online)
                .setContentTitle("TJCap for Android 正在透過 USB-C 直播畫面")
                .setContentText("免開 USB 偵錯 · AOA 高速配件直連運作中")
                .setContentIntent(openPi)
                .setOngoing(true)
                .addAction(new Notification.Action.Builder(
                        android.R.drawable.ic_media_pause, "停止傳輸", stopPi).build());

        Notification notif = nb.build();
        if (Build.VERSION.SDK_INT >= 29) {
            startForeground(NOTIF_ID, notif, ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PROJECTION);
        } else {
            startForeground(NOTIF_ID, notif);
        }
    }

    private void startStreamingPipeline(
            final int resultCode,
            final Intent resultData,
            final boolean userAudioPref,
            final int defaultMaxDim,
            final int defaultMaxFps) {
        stopPipelineResources();
        stopFlag.set(false);

        UsbManager usbMgr = (UsbManager) getSystemService(Context.USB_SERVICE);
        UsbAccessory[] accList = (usbMgr != null) ? usbMgr.getAccessoryList() : null;
        if (accList == null || accList.length == 0) {
            stopSelfCleanly("尚未偵測到電腦端 TJCap AOA 配件通道（請先在電腦點擊啟動）");
            return;
        }

        UsbAccessory targetAcc = accList[0];
        if (!usbMgr.hasPermission(targetAcc)) {
            stopSelfCleanly("尚未取得 USB 配件存取權限，請點擊允許 USB 配件視窗");
            return;
        }

        accessoryPfd = usbMgr.openAccessory(targetAcc);
        if (accessoryPfd == null) {
            stopSelfCleanly("無法開啟 USB AOA 配件檔案描述子");
            return;
        }

        MediaProjectionManager mpm =
                (MediaProjectionManager) getSystemService(Context.MEDIA_PROJECTION_SERVICE);
        if (mpm == null) {
            stopSelfCleanly("無法取得系統 MediaProjectionManager");
            return;
        }

        mediaProjection = mpm.getMediaProjection(resultCode, resultData);
        if (mediaProjection == null) {
            stopSelfCleanly("建立 MediaProjection 失敗，請重新授權");
            return;
        }

        // Android 14+ (API 34+) strictly requires registering a callback BEFORE createVirtualDisplay!
        mediaProjection.registerCallback(new MediaProjection.Callback() {
            @Override
            public void onStop() {
                stopSelfCleanly("系統螢幕錄製已結束");
            }
        }, new Handler(Looper.getMainLooper()));

        sRunning = true;
        updateStatus("🟢 USB-C 高速擷取串流中");

        final FileInputStream usbIn = new FileInputStream(accessoryPfd.getFileDescriptor());
        final FileOutputStream usbOut = new FileOutputStream(accessoryPfd.getFileDescriptor());

        final AoaProtocol.StartConfig cfg = new AoaProtocol.StartConfig();
        cfg.maxDim = defaultMaxDim;
        cfg.maxFps = defaultMaxFps;
        cfg.enableAudio = userAudioPref;

        // Control reader thread (receives optional PC bitrate/max_dim/stop commands)
        final AtomicBoolean gotStartCmd = new AtomicBoolean(false);
        controlThread = new Thread(() -> {
            byte[] frame = new byte[AoaProtocol.CTRL_FRAME_BYTES];
            try {
                while (!stopFlag.get()) {
                    if (!AoaProtocol.readExact(usbIn, frame, AoaProtocol.CTRL_FRAME_BYTES)) {
                        break;
                    }
                    byte cmd = AoaProtocol.parseControlFrame(frame, cfg);
                    if (cmd == AoaProtocol.CMD_START_STREAM) {
                        gotStartCmd.set(true);
                    } else if (cmd == AoaProtocol.CMD_STOP_STREAM) {
                        stopSelfCleanly("電腦端已停止投影擷取");
                        break;
                    }
                }
            } catch (IOException ignored) {
            }
        }, "tjcap-aoa-ctrl");
        controlThread.start();

        videoThread = new Thread(() -> {
            try {
                // Brief 180ms window to pick up PC's exact locked maxDim/bitrate/fps if sent
                for (int i = 0; i < 9 && !gotStartCmd.get() && !stopFlag.get(); ++i) {
                    try {
                        Thread.sleep(20);
                    } catch (InterruptedException ignored) {
                        break;
                    }
                }
                runVideoAndAudioLoop(usbOut, cfg);
            } catch (Throwable t) {
                if (!stopFlag.get()) {
                    stopSelfCleanly("USB 傳輸中斷：" + t.getMessage());
                }
            }
        }, "tjcap-aoa-video");
        videoThread.start();
    }

    @SuppressWarnings("deprecation")
    private DisplayMetrics queryRealMetrics() {
        DisplayMetrics dm = new DisplayMetrics();
        WindowManager wm = (WindowManager) getSystemService(Context.WINDOW_SERVICE);
        if (wm != null && wm.getDefaultDisplay() != null) {
            wm.getDefaultDisplay().getRealMetrics(dm);
        } else {
            dm.widthPixels = 1920;
            dm.heightPixels = 1080;
            dm.densityDpi = 420;
        }
        return dm;
    }

    private void runVideoAndAudioLoop(FileOutputStream usbOut, AoaProtocol.StartConfig cfg)
            throws IOException {
        DisplayMetrics dm = queryRealMetrics();
        int rawW = Math.max(64, dm.widthPixels);
        int rawH = Math.max(64, dm.heightPixels);
        int dpi = Math.max(120, dm.densityDpi);

        int maxLong = Math.max(480, Math.min(3840, cfg.maxDim));
        int curLong = Math.max(rawW, rawH);
        int encW = rawW;
        int encH = rawH;
        if (curLong > maxLong) {
            encW = (rawW * maxLong) / curLong;
            encH = (rawH * maxLong) / curLong;
        }
        // Align to 16px macroblock boundary for universal hardware H.264 encoder compatibility
        encW = Math.max(128, (encW + 7) & ~15);
        encH = Math.max(128, (encH + 7) & ~15);

        String fullModel = Build.MANUFACTURER + " " + Build.MODEL;
        AoaProtocol.writeHelloDeviceMeta(usbOut, usbWriteLock, fullModel, Build.VERSION.SDK_INT, encW, encH);
        AoaProtocol.writeSizeChanged(usbOut, usbWriteLock, encW, encH);

        // Configure low-latency hardware H.264 encoder
        videoEncoder = MediaCodec.createEncoderByType(MediaFormat.MIMETYPE_VIDEO_AVC);
        MediaFormat fmt = MediaFormat.createVideoFormat(MediaFormat.MIMETYPE_VIDEO_AVC, encW, encH);
        fmt.setInteger(MediaFormat.KEY_COLOR_FORMAT, MediaCodecInfo.CodecCapabilities.COLOR_FormatSurface);
        fmt.setInteger(MediaFormat.KEY_BIT_RATE, cfg.videoBitrateBps);
        fmt.setInteger(MediaFormat.KEY_FRAME_RATE, cfg.maxFps);
        fmt.setInteger(MediaFormat.KEY_I_FRAME_INTERVAL, 2);
        fmt.setLong(MediaFormat.KEY_REPEAT_PREVIOUS_FRAME_AFTER, 100_000L);
        if (Build.VERSION.SDK_INT >= 30) {
            fmt.setInteger(MediaFormat.KEY_LOW_LATENCY, 1);
        }
        fmt.setInteger(MediaFormat.KEY_PRIORITY, 0);

        try {
            videoEncoder.configure(fmt, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE);
        } catch (RuntimeException ex) {
            // Fallback without optional low-latency hint for older/strict vendor codecs
            videoEncoder.reset();
            MediaFormat fallbackFmt =
                    MediaFormat.createVideoFormat(MediaFormat.MIMETYPE_VIDEO_AVC, encW, encH);
            fallbackFmt.setInteger(
                    MediaFormat.KEY_COLOR_FORMAT,
                    MediaCodecInfo.CodecCapabilities.COLOR_FormatSurface);
            fallbackFmt.setInteger(MediaFormat.KEY_BIT_RATE, cfg.videoBitrateBps);
            fallbackFmt.setInteger(MediaFormat.KEY_FRAME_RATE, cfg.maxFps);
            fallbackFmt.setInteger(MediaFormat.KEY_I_FRAME_INTERVAL, 2);
            fallbackFmt.setLong(MediaFormat.KEY_REPEAT_PREVIOUS_FRAME_AFTER, 100_000L);
            videoEncoder.configure(fallbackFmt, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE);
        }

        inputSurface = videoEncoder.createInputSurface();
        videoEncoder.start();

        virtualDisplay = mediaProjection.createVirtualDisplay(
                "TJCapUsbDisplay",
                encW,
                encH,
                dpi,
                DisplayManager.VIRTUAL_DISPLAY_FLAG_AUTO_MIRROR,
                inputSurface,
                null,
                null);

        updateStatus("🟢 串流中：" + encW + "×" + encH + " @ " + cfg.maxFps + "fps");

        if (cfg.enableAudio) {
            startInternalAudioThread(usbOut);
        } else {
            AoaProtocol.writePacket(
                    usbOut, usbWriteLock, AoaProtocol.PKT_AUDIO_UNSUPPORTED, 0L, null, 0, 0);
        }

        MediaCodec.BufferInfo info = new MediaCodec.BufferInfo();
        byte[] transferScratch = new byte[256 * 1024];

        while (!stopFlag.get()) {
            int outIndex = videoEncoder.dequeueOutputBuffer(info, 150_000L);
            if (outIndex == MediaCodec.INFO_TRY_AGAIN_LATER) {
                AoaProtocol.writePacket(
                        usbOut, usbWriteLock, AoaProtocol.PKT_HEARTBEAT, 0L, null, 0, 0);
                continue;
            }
            if (outIndex < 0) {
                continue;
            }

            ByteBuffer encoded = videoEncoder.getOutputBuffer(outIndex);
            if (encoded != null && info.size > 0) {
                encoded.position(info.offset);
                encoded.limit(info.offset + info.size);
                if (transferScratch.length < info.size) {
                    transferScratch = new byte[info.size + 32768];
                }
                encoded.get(transferScratch, 0, info.size);

                boolean isConfig = (info.flags & MediaCodec.BUFFER_FLAG_CODEC_CONFIG) != 0;
                boolean isKey = (info.flags & MediaCodec.BUFFER_FLAG_KEY_FRAME) != 0;

                if (isConfig) {
                    long whMeta = (((long) encW) << 32) | (((long) encH) & 0xFFFFFFFFL);
                    AoaProtocol.writePacket(
                            usbOut,
                            usbWriteLock,
                            AoaProtocol.PKT_VIDEO_CONFIG_SPS_PPS,
                            whMeta,
                            transferScratch,
                            0,
                            info.size);
                } else {
                    byte ptype = isKey
                            ? AoaProtocol.PKT_VIDEO_H264_KEYFRAME
                            : AoaProtocol.PKT_VIDEO_H264_FRAME;
                    AoaProtocol.writePacket(
                            usbOut,
                            usbWriteLock,
                            ptype,
                            Math.max(0L, info.presentationTimeUs),
                            transferScratch,
                            0,
                            info.size);
                }
            }
            videoEncoder.releaseOutputBuffer(outIndex, false);
        }
    }

    private void startInternalAudioThread(final FileOutputStream usbOut) {
        if (Build.VERSION.SDK_INT < 29 || mediaProjection == null) {
            try {
                AoaProtocol.writePacket(
                        usbOut, usbWriteLock, AoaProtocol.PKT_AUDIO_UNSUPPORTED, 0L, null, 0, 0);
            } catch (IOException ignored) {
            }
            return;
        }

        audioThread = new Thread(() -> {
            AudioRecord recorder = null;
            try {
                AudioPlaybackCaptureConfiguration apcc =
                        new AudioPlaybackCaptureConfiguration.Builder(mediaProjection)
                                .addMatchingUsage(AudioAttributes.USAGE_MEDIA)
                                .addMatchingUsage(AudioAttributes.USAGE_GAME)
                                .addMatchingUsage(AudioAttributes.USAGE_UNKNOWN)
                                .build();

                AudioFormat af = new AudioFormat.Builder()
                        .setEncoding(AudioFormat.ENCODING_PCM_16BIT)
                        .setSampleRate(48000)
                        .setChannelMask(AudioFormat.CHANNEL_IN_STEREO)
                        .build();

                int minBuf = AudioRecord.getMinBufferSize(
                        48000, AudioFormat.CHANNEL_IN_STEREO, AudioFormat.ENCODING_PCM_16BIT);
                int bufBytes = Math.max(minBuf * 2, 4096 * 4);

                recorder = new AudioRecord.Builder()
                        .setAudioFormat(af)
                        .setBufferSizeInBytes(bufBytes)
                        .setAudioPlaybackCaptureConfig(apcc)
                        .build();

                if (recorder.getState() != AudioRecord.STATE_INITIALIZED) {
                    AoaProtocol.writePacket(
                            usbOut, usbWriteLock, AoaProtocol.PKT_AUDIO_UNSUPPORTED, 0L, null, 0, 0);
                    return;
                }

                recorder.startRecording();
                byte[] pcmBuf = new byte[4096]; // 1024 stereo frames (~21.3ms)
                long samplesRead = 0;

                while (!stopFlag.get()) {
                    int nr = recorder.read(pcmBuf, 0, pcmBuf.length);
                    if (nr > 0) {
                        int aligned = nr & ~3;
                        if (aligned > 0) {
                            long ptsUs = (samplesRead * 1_000_000L) / 48000L;
                            samplesRead += (aligned / 4);
                            AoaProtocol.writePacket(
                                    usbOut,
                                    usbWriteLock,
                                    AoaProtocol.PKT_AUDIO_PCM_S16LE,
                                    ptsUs,
                                    pcmBuf,
                                    0,
                                    aligned);
                        }
                    } else if (nr < 0) {
                        break;
                    }
                }
            } catch (SecurityException | IllegalArgumentException | IOException e) {
                try {
                    AoaProtocol.writePacket(
                            usbOut, usbWriteLock, AoaProtocol.PKT_AUDIO_UNSUPPORTED, 0L, null, 0, 0);
                } catch (IOException ignored) {
                }
            } finally {
                if (recorder != null) {
                    try {
                        recorder.stop();
                    } catch (Throwable ignored) {
                    }
                    recorder.release();
                }
            }
        }, "tjcap-aoa-audio");
        audioThread.start();
    }

    private void updateStatus(String msg) {
        sStatusText = msg;
        Intent b = new Intent(ACTION_STATE_BROADCAST);
        b.setPackage(getPackageName());
        sendBroadcast(b);
    }

    private void stopPipelineResources() {
        stopFlag.set(true);

        if (accessoryPfd != null) {
            try {
                accessoryPfd.close();
            } catch (IOException ignored) {
            }
            accessoryPfd = null;
        }

        if (virtualDisplay != null) {
            virtualDisplay.release();
            virtualDisplay = null;
        }

        if (videoEncoder != null) {
            try {
                videoEncoder.stop();
            } catch (Throwable ignored) {
            }
            try {
                videoEncoder.release();
            } catch (Throwable ignored) {
            }
            videoEncoder = null;
        }

        if (inputSurface != null) {
            inputSurface.release();
            inputSurface = null;
        }

        if (mediaProjection != null) {
            try {
                mediaProjection.stop();
            } catch (Throwable ignored) {
            }
            mediaProjection = null;
        }
    }

    private void stopSelfCleanly(String reason) {
        sRunning = false;
        stopPipelineResources();
        updateStatus(reason);
        stopForeground(true);
        stopSelf();
    }

    @Override
    public void onDestroy() {
        sRunning = false;
        stopPipelineResources();
        super.onDestroy();
    }
}
