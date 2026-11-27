package com.tjcap.android.companion;

import android.Manifest;
import android.app.Activity;
import android.app.PendingIntent;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.pm.PackageManager;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.hardware.usb.UsbAccessory;
import android.hardware.usb.UsbManager;
import android.media.projection.MediaProjectionManager;
import android.os.Build;
import android.os.Bundle;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.View;
import android.widget.Button;
import android.widget.CheckBox;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;

public final class MainActivity extends Activity {

    private static final int REQ_RECORD_AUDIO = 1001;
    private static final int REQ_MEDIA_PROJECTION = 1002;
    private static final String ACTION_USB_PERMISSION = "com.tjcap.android.companion.USB_PERMISSION";

    private TextView txtAccessoryBadge;
    private TextView txtServiceStatus;
    private CheckBox chkAudio;
    private Button btnStartStop;

    private boolean autoPromptTriggered = false;

    private final BroadcastReceiver receiver = new BroadcastReceiver() {
        @Override
        public void onReceive(Context context, Intent intent) {
            if (intent == null) return;
            String act = intent.getAction();
            if (UsbManager.ACTION_USB_ACCESSORY_DETACHED.equals(act)) {
                Intent stopSvc = new Intent(MainActivity.this, TJCapCaptureService.class);
                stopSvc.setAction(TJCapCaptureService.ACTION_STOP);
                startService(stopSvc);
            }
            refreshUiState();
        }
    };

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        if (Build.VERSION.SDK_INT >= 21) {
            getWindow().setStatusBarColor(Color.parseColor("#071912"));
            getWindow().setNavigationBarColor(Color.parseColor("#071912"));
        }

        buildGreenThemeLayout();
        registerReceiversCompat();

        if (Build.VERSION.SDK_INT >= 33) {
            if (checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS)
                    != PackageManager.PERMISSION_GRANTED) {
                requestPermissions(new String[]{Manifest.permission.POST_NOTIFICATIONS}, 1003);
            }
        }

        handleIncomingIntent(getIntent());
    }

    @Override
    protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        setIntent(intent);
        handleIncomingIntent(intent);
    }

    @Override
    protected void onResume() {
        super.onResume();
        refreshUiState();
    }

    @Override
    protected void onDestroy() {
        try {
            unregisterReceiver(receiver);
        } catch (Throwable ignored) {
        }
        super.onDestroy();
    }

    private void registerReceiversCompat() {
        IntentFilter filter = new IntentFilter();
        filter.addAction(TJCapCaptureService.ACTION_STATE_BROADCAST);
        filter.addAction(ACTION_USB_PERMISSION);
        filter.addAction(UsbManager.ACTION_USB_ACCESSORY_ATTACHED);
        filter.addAction(UsbManager.ACTION_USB_ACCESSORY_DETACHED);
        if (Build.VERSION.SDK_INT >= 33) {
            registerReceiver(receiver, filter, Context.RECEIVER_NOT_EXPORTED);
        } else {
            registerReceiver(receiver, filter);
        }
    }

    private int dp(int v) {
        return Math.round(TypedValue.applyDimension(
                TypedValue.COMPLEX_UNIT_DIP, v, getResources().getDisplayMetrics()));
    }

    private static GradientDrawable roundedBox(String fillHex, String strokeHex, int radiusDp, Context ctx) {
        float r = TypedValue.applyDimension(
                TypedValue.COMPLEX_UNIT_DIP, radiusDp, ctx.getResources().getDisplayMetrics());
        int sw = Math.max(1, Math.round(TypedValue.applyDimension(
                TypedValue.COMPLEX_UNIT_DIP, 1.2f, ctx.getResources().getDisplayMetrics())));
        GradientDrawable gd = new GradientDrawable();
        gd.setColor(Color.parseColor(fillHex));
        gd.setCornerRadius(r);
        gd.setStroke(sw, Color.parseColor(strokeHex));
        return gd;
    }

    private void buildGreenThemeLayout() {
        ScrollView scroll = new ScrollView(this);
        scroll.setBackgroundColor(Color.parseColor("#071912"));
        scroll.setFillViewport(true);

        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setPadding(dp(20), dp(26), dp(20), dp(26));

        // Header Card
        LinearLayout headerCard = new LinearLayout(this);
        headerCard.setOrientation(LinearLayout.VERTICAL);
        headerCard.setPadding(dp(18), dp(18), dp(18), dp(18));
        headerCard.setBackground(roundedBox("#0E291E", "#10B981", 16, this));

        TextView title = new TextView(this);
        title.setText("TJCap Companion");
        title.setTextColor(Color.parseColor("#34D399"));
        title.setTextSize(TypedValue.COMPLEX_UNIT_SP, 23);
        title.setTypeface(Typeface.DEFAULT_BOLD);
        headerCard.addView(title);

        TextView sub = new TextView(this);
        sub.setText("TJCap for Android 專屬免開 USB 偵錯配件端 (v1.0.0)\n免 Root · 免開開發者選項 · USB-C 硬體 H.264 直傳");
        sub.setTextColor(Color.parseColor("#D1FAE5"));
        sub.setTextSize(TypedValue.COMPLEX_UNIT_SP, 13.5f);
        sub.setPadding(0, dp(6), 0, 0);
        headerCard.addView(sub);

        root.addView(headerCard);

        // Status Card
        LinearLayout statusCard = new LinearLayout(this);
        statusCard.setOrientation(LinearLayout.VERTICAL);
        statusCard.setPadding(dp(18), dp(16), dp(18), dp(16));
        statusCard.setBackground(roundedBox("#0B2118", "#1F513B", 14, this));
        LinearLayout.LayoutParams statusLp = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT);
        statusLp.topMargin = dp(16);
        statusCard.setLayoutParams(statusLp);

        txtAccessoryBadge = new TextView(this);
        txtAccessoryBadge.setTextColor(Color.parseColor("#6EE7B7"));
        txtAccessoryBadge.setTextSize(TypedValue.COMPLEX_UNIT_SP, 15);
        txtAccessoryBadge.setTypeface(Typeface.DEFAULT_BOLD);
        statusCard.addView(txtAccessoryBadge);

        txtServiceStatus = new TextView(this);
        txtServiceStatus.setTextColor(Color.parseColor("#A7F3D0"));
        txtServiceStatus.setTextSize(TypedValue.COMPLEX_UNIT_SP, 13.5f);
        txtServiceStatus.setPadding(0, dp(8), 0, 0);
        statusCard.addView(txtServiceStatus);

        chkAudio = new CheckBox(this);
        chkAudio.setText(" 同步傳輸手機系統內錄音訊 (48kHz 立體聲)");
        chkAudio.setTextColor(Color.parseColor("#ECFDF5"));
        chkAudio.setTextSize(TypedValue.COMPLEX_UNIT_SP, 14);
        chkAudio.setChecked(Build.VERSION.SDK_INT >= 29);
        chkAudio.setEnabled(Build.VERSION.SDK_INT >= 29);
        LinearLayout.LayoutParams chkLp = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT);
        chkLp.topMargin = dp(12);
        chkAudio.setLayoutParams(chkLp);
        statusCard.addView(chkAudio);

        btnStartStop = new Button(this);
        btnStartStop.setAllCaps(false);
        btnStartStop.setTextSize(TypedValue.COMPLEX_UNIT_SP, 16);
        btnStartStop.setTypeface(Typeface.DEFAULT_BOLD);
        btnStartStop.setTextColor(Color.parseColor("#042F1E"));
        btnStartStop.setBackground(roundedBox("#34D399", "#6EE7B7", 12, this));
        btnStartStop.setPadding(dp(16), dp(14), dp(16), dp(14));
        LinearLayout.LayoutParams btnLp = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT);
        btnLp.topMargin = dp(16);
        btnStartStop.setLayoutParams(btnLp);
        btnStartStop.setOnClickListener((View v) -> onPrimaryButtonClicked());
        statusCard.addView(btnStartStop);

        root.addView(statusCard);

        // Quick Guide Card
        LinearLayout guideCard = new LinearLayout(this);
        guideCard.setOrientation(LinearLayout.VERTICAL);
        guideCard.setPadding(dp(18), dp(16), dp(18), dp(16));
        guideCard.setBackground(roundedBox("#0B2118", "#1F513B", 14, this));
        LinearLayout.LayoutParams guideLp = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT);
        guideLp.topMargin = dp(16);
        guideCard.setLayoutParams(guideLp);

        TextView guideTitle = new TextView(this);
        guideTitle.setText("📖 免開 USB 偵錯模式 — 3 步連線教學");
        guideTitle.setTextColor(Color.parseColor("#34D399"));
        guideTitle.setTextSize(TypedValue.COMPLEX_UNIT_SP, 15);
        guideTitle.setTypeface(Typeface.DEFAULT_BOLD);
        guideCard.addView(guideTitle);

        TextView guideBody = new TextView(this);
        guideBody.setText(
                "1. 用 USB-C 傳輸線將手機連接至 Windows 電腦。\n" +
                "2. 在電腦開啟「TJCap for Android」，連線模式選擇「🛡️ 免開 USB 偵錯模式 (Companion APK + USB AOA)」並點擊「🚀 智慧一鍵啟動」。\n" +
                "3. 電腦會自動將手機切換為 AOA 高速配件並喚醒本 App，點擊上方綠色按鈕授權「開始錄製」即可直傳 OBS Studio！");
        guideBody.setTextColor(Color.parseColor("#D1FAE5"));
        guideBody.setTextSize(TypedValue.COMPLEX_UNIT_SP, 13.5f);
        guideBody.setLineSpacing(dp(3), 1.0f);
        guideBody.setPadding(0, dp(8), 0, 0);
        guideCard.addView(guideBody);

        root.addView(guideCard);
        scroll.addView(root);
        setContentView(scroll);
    }

    private void handleIncomingIntent(Intent intent) {
        if (intent != null && UsbManager.ACTION_USB_ACCESSORY_ATTACHED.equals(intent.getAction())) {
            refreshUiState();
            if (!TJCapCaptureService.isRunning() && !autoPromptTriggered) {
                autoPromptTriggered = true;
                beginCaptureAuthorizationFlow();
            }
        }
    }

    private UsbAccessory getAttachedAccessory() {
        UsbManager usbMgr = (UsbManager) getSystemService(Context.USB_SERVICE);
        if (usbMgr == null) return null;
        UsbAccessory[] list = usbMgr.getAccessoryList();
        return (list != null && list.length > 0) ? list[0] : null;
    }

    private void refreshUiState() {
        UsbAccessory acc = getAttachedAccessory();
        boolean running = TJCapCaptureService.isRunning();

        if (acc != null) {
            txtAccessoryBadge.setText("🟢 已連通電腦端 TJCap USB-C 配件通道 (" + acc.getModel() + ")");
            txtAccessoryBadge.setTextColor(Color.parseColor("#34D399"));
        } else {
            txtAccessoryBadge.setText("🔌 等待電腦端啟動「免開 USB 偵錯 (AOA)」模式...");
            txtAccessoryBadge.setTextColor(Color.parseColor("#FBBF24"));
        }

        txtServiceStatus.setText("目前狀態：" + TJCapCaptureService.getStatusText());

        if (running) {
            btnStartStop.setText("■ 停止 USB-C 畫面直播傳輸");
            btnStartStop.setTextColor(Color.parseColor("#FEF2F2"));
            btnStartStop.setBackground(roundedBox("#DC2626", "#F87171", 12, this));
            chkAudio.setEnabled(false);
        } else {
            btnStartStop.setText("🚀 授權螢幕錄製並開始 USB-C 直播");
            btnStartStop.setTextColor(Color.parseColor("#042F1E"));
            btnStartStop.setBackground(roundedBox("#34D399", "#6EE7B7", 12, this));
            chkAudio.setEnabled(Build.VERSION.SDK_INT >= 29);
        }
    }

    private void onPrimaryButtonClicked() {
        if (TJCapCaptureService.isRunning()) {
            Intent stopIntent = new Intent(this, TJCapCaptureService.class);
            stopIntent.setAction(TJCapCaptureService.ACTION_STOP);
            startService(stopIntent);
            refreshUiState();
            return;
        }
        beginCaptureAuthorizationFlow();
    }

    private void beginCaptureAuthorizationFlow() {
        UsbManager usbMgr = (UsbManager) getSystemService(Context.USB_SERVICE);
        UsbAccessory acc = getAttachedAccessory();
        if (usbMgr == null || acc == null) {
            Toast.makeText(
                    this,
                    "請先插上 USB-C 線，並在電腦端 TJCap 選擇「免開 USB 偵錯模式 (AOA)」點擊啟動！",
                    Toast.LENGTH_LONG).show();
            return;
        }

        if (!usbMgr.hasPermission(acc)) {
            Intent piIntent = new Intent(ACTION_USB_PERMISSION);
            piIntent.setPackage(getPackageName()); // Required on Android 14+ for FLAG_MUTABLE!
            int flags = PendingIntent.FLAG_UPDATE_CURRENT;
            if (Build.VERSION.SDK_INT >= 31) {
                flags |= PendingIntent.FLAG_MUTABLE;
            }
            PendingIntent pi = PendingIntent.getBroadcast(this, 0, piIntent, flags);
            usbMgr.requestPermission(acc, pi);
            Toast.makeText(this, "請在彈出視窗允許存取 TJCap USB 配件，再點一次啟動", Toast.LENGTH_SHORT).show();
            return;
        }

        if (chkAudio.isChecked() && Build.VERSION.SDK_INT >= 29) {
            if (checkSelfPermission(Manifest.permission.RECORD_AUDIO)
                    != PackageManager.PERMISSION_GRANTED) {
                requestPermissions(new String[]{Manifest.permission.RECORD_AUDIO}, REQ_RECORD_AUDIO);
                return;
            }
        }

        requestMediaProjectionConsent();
    }

    private void requestMediaProjectionConsent() {
        MediaProjectionManager mpm =
                (MediaProjectionManager) getSystemService(Context.MEDIA_PROJECTION_SERVICE);
        if (mpm != null) {
            startActivityForResult(mpm.createScreenCaptureIntent(), REQ_MEDIA_PROJECTION);
        }
    }

    @Override
    public void onRequestPermissionsResult(int requestCode, String[] permissions, int[] grantResults) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults);
        if (requestCode == REQ_RECORD_AUDIO) {
            if (grantResults.length == 0 || grantResults[0] != PackageManager.PERMISSION_GRANTED) {
                chkAudio.setChecked(false);
                Toast.makeText(this, "未授權錄音權限，將以純畫面模式傳輸", Toast.LENGTH_SHORT).show();
            }
            requestMediaProjectionConsent();
        }
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode == REQ_MEDIA_PROJECTION) {
            if (resultCode == Activity.RESULT_OK && data != null) {
                Intent svc = new Intent(this, TJCapCaptureService.class);
                svc.setAction(TJCapCaptureService.ACTION_START);
                svc.putExtra(TJCapCaptureService.EXTRA_RESULT_CODE, resultCode);
                svc.putExtra(TJCapCaptureService.EXTRA_RESULT_DATA, data);
                svc.putExtra(TJCapCaptureService.EXTRA_ENABLE_AUDIO, chkAudio.isChecked());
                if (Build.VERSION.SDK_INT >= 26) {
                    startForegroundService(svc);
                } else {
                    startService(svc);
                }
            } else {
                Toast.makeText(this, "已取消螢幕錄製授權", Toast.LENGTH_SHORT).show();
            }
        }
    }
}
