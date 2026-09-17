package dev.recompkit;

import android.content.ContentResolver;
import android.content.Context;
import android.content.Intent;
import android.content.pm.ActivityInfo;
import android.database.Cursor;
import android.net.Uri;
import android.os.Build;
import android.os.ParcelFileDescriptor;
import android.os.VibrationEffect;
import android.os.Vibrator;
import android.os.VibratorManager;
import android.provider.DocumentsContract;
import android.provider.OpenableColumns;
import android.view.HapticFeedbackConstants;
import android.view.WindowManager;

import java.util.ArrayList;

import org.libsdl.app.SDLActivity;

/**
 * Loads the host library, which links SDL statically, and gives the launcher
 * (host/launcher/launcher_platform_android.cpp) the system pickers, content
 * URIs as file descriptors, and the import service.
 */
public class RecompActivity extends SDLActivity {
    static final int PICK_TREE = 0x5201;
    static final int PICK_ZIP = 0x5202;
    static final int PICK_SAVES = 0x5203;
    static final int CREATE_EXPORT = 0x5204;

    static RecompActivity sActivity;
    static String sViewUri;

    /** A picker answered: `uris` is empty when the player cancelled. */
    static native void nativePicked(int request, String[] uris, String[] names);

    @Override
    protected String[] getLibraries() {
        return new String[] {"main"};
    }

    @Override
    protected void onCreate(android.os.Bundle savedInstanceState) {
        // The manifest allows any orientation (phones rotate live to
        // portrait for the touch controls; tablets stay landscape). SDL sets
        // its own per-device-class orientation hint once it has a display to
        // measure (platform_ui_create_window, host/sdl/platform_ui_desktop.cpp)
        // and that hint is what actually holds a tablet in landscape once
        // SDL's (resizable) window exists. Before that - between this
        // activity starting and SDL's first window - there is no hint yet,
        // so this is the fallback that keeps a tablet from ever showing a
        // frame in portrait.
        if (getResources().getConfiguration().smallestScreenWidthDp >= 600)
            setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_USER_LANDSCAPE);
        super.onCreate(savedInstanceState);
        sActivity = this;
        takeViewIntent(getIntent());
    }

    @Override
    protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        takeViewIntent(intent);
    }

    private static void takeViewIntent(Intent intent) {
        if (intent != null && Intent.ACTION_VIEW.equals(intent.getAction()) && intent.getData() != null)
            sViewUri = intent.getData().toString();
    }

    /** A ZIP the app was opened with ("Open with" in Files), once. */
    public static String launcherTakeViewUri() {
        String uri = sViewUri;
        sViewUri = null;
        return uri;
    }

    public static void launcherPick(int request, String suggestedName) {
        final RecompActivity a = sActivity;
        if (a == null) {
            nativePicked(request, new String[0], new String[0]);
            return;
        }
        a.runOnUiThread(() -> {
            Intent intent;
            if (request == PICK_TREE) {
                intent = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
            } else if (request == CREATE_EXPORT) {
                intent = new Intent(Intent.ACTION_CREATE_DOCUMENT);
                intent.addCategory(Intent.CATEGORY_OPENABLE);
                intent.setType("application/zip");
                intent.putExtra(Intent.EXTRA_TITLE, suggestedName);
            } else {
                intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
                intent.addCategory(Intent.CATEGORY_OPENABLE);
                intent.setType("application/zip");
                intent.putExtra(Intent.EXTRA_MIME_TYPES,
                        new String[] {"application/zip", "application/x-zip-compressed", "application/octet-stream"});
            }
            try {
                a.startActivityForResult(intent, request);
            } catch (Exception e) {
                nativePicked(request, new String[0], new String[0]);
            }
        });
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        if (requestCode < PICK_TREE || requestCode > CREATE_EXPORT) {
            super.onActivityResult(requestCode, resultCode, data);
            return;
        }
        if (resultCode != RESULT_OK || data == null || data.getData() == null) {
            nativePicked(requestCode, new String[0], new String[0]);
            return;
        }
        Uri uri = data.getData();
        if (requestCode == PICK_TREE) {
            try {
                getContentResolver().takePersistableUriPermission(uri, Intent.FLAG_GRANT_READ_URI_PERMISSION);
            } catch (SecurityException ignored) {
            }
        }
        nativePicked(requestCode, new String[] {uri.toString()}, new String[] {displayName(uri)});
    }

    static String displayName(Uri uri) {
        String name = uri.getLastPathSegment();
        try (Cursor c = sActivity.getContentResolver().query(uri, new String[] {OpenableColumns.DISPLAY_NAME},
                null, null, null)) {
            if (c != null && c.moveToFirst() && !c.isNull(0))
                name = c.getString(0);
        } catch (Exception ignored) {
        }
        return name;
    }

    /** A descriptor the caller owns, or -1. `mode` is "r" or "w". */
    public static int launcherOpenFd(String uri, String mode) {
        try {
            ParcelFileDescriptor pfd = sActivity.getContentResolver().openFileDescriptor(
                    Uri.parse(uri), mode.equals("w") ? "wt" : "r");
            return pfd == null ? -1 : pfd.detachFd();
        } catch (Exception e) {
            return -1;
        }
    }

    /**
     * Every document under a tree the player picked, as
     * "relative\tsize\tmtime\tdocumentUri\tisDirectory".
     */
    public static String[] launcherListTree(String treeUri) {
        ArrayList<String> out = new ArrayList<>();
        try {
            Uri tree = Uri.parse(treeUri);
            String rootId = DocumentsContract.getTreeDocumentId(tree);
            listChildren(sActivity.getContentResolver(), tree, rootId, "", out, 0);
        } catch (Exception e) {
            return null;
        }
        return out.toArray(new String[0]);
    }

    private static void listChildren(ContentResolver resolver, Uri tree, String parentId, String prefix,
                                     ArrayList<String> out, int depth) {
        if (depth > 16)
            return;
        Uri children = DocumentsContract.buildChildDocumentsUriUsingTree(tree, parentId);
        String[] columns = {DocumentsContract.Document.COLUMN_DOCUMENT_ID,
                            DocumentsContract.Document.COLUMN_DISPLAY_NAME,
                            DocumentsContract.Document.COLUMN_MIME_TYPE,
                            DocumentsContract.Document.COLUMN_SIZE,
                            DocumentsContract.Document.COLUMN_LAST_MODIFIED};
        try (Cursor c = resolver.query(children, columns, null, null, null)) {
            if (c == null)
                return;
            while (c.moveToNext()) {
                String id = c.getString(0);
                String name = c.getString(1);
                boolean dir = DocumentsContract.Document.MIME_TYPE_DIR.equals(c.getString(2));
                long size = c.isNull(3) ? 0 : c.getLong(3);
                long mtime = c.isNull(4) ? 0 : c.getLong(4) / 1000;
                String relative = prefix.isEmpty() ? name : prefix + "/" + name;
                Uri doc = DocumentsContract.buildDocumentUriUsingTree(tree, id);
                out.add(relative + "\t" + size + "\t" + mtime + "\t" + doc + "\t" + (dir ? 1 : 0));
                if (dir)
                    listChildren(resolver, tree, id, relative, out, depth + 1);
            }
        }
    }

    /** The import started, advanced or ended: keep the screen on and the process alive. */
    public static void launcherImportActivity(boolean active, long done, long total, String current) {
        final RecompActivity a = sActivity;
        if (a == null)
            return;
        a.runOnUiThread(() -> {
            if (active)
                a.getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
            else
                a.getWindow().clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        });
        Intent service = new Intent(a, ImportService.class);
        if (!active) {
            a.stopService(service);
            return;
        }
        service.putExtra("done", done);
        service.putExtra("total", total);
        service.putExtra("current", current);
        try {
            if (Build.VERSION.SDK_INT >= 26)
                a.startForegroundService(service);
            else
                a.startService(service);
        } catch (Exception ignored) {
            // A service the system refuses leaves an import that still runs
            // while the app is in front.
        }
        if (Build.VERSION.SDK_INT >= 33 && done == 0
                && a.checkSelfPermission("android.permission.POST_NOTIFICATIONS")
                        != android.content.pm.PackageManager.PERMISSION_GRANTED)
            a.runOnUiThread(() -> a.requestPermissions(new String[] {"android.permission.POST_NOTIFICATIONS"}, 0x5210));
    }

    public static void launcherOpenUrl(String url) {
        final RecompActivity a = sActivity;
        if (a == null)
            return;
        try {
            Intent intent = new Intent(Intent.ACTION_VIEW, Uri.parse(url));
            intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
            a.startActivity(intent);
        } catch (Exception ignored) {
        }
    }

    // -- Haptics: a light tick for on-screen control presses, and the device's
    // own motor standing in for game rumble when no controller is connected
    // (host/sdl/platform_ui_desktop.cpp calls these over JNI). ---------------

    static Vibrator sVibrator;
    // The amplitude bucket (0..15) currently driving the motor, or -1 while
    // idle: a repeat call in the same bucket leaves the running effect alone
    // instead of restarting it every frame the guest asks for the same rumble.
    static int sRumbleBucket = -1;

    private static Vibrator vibrator() {
        if (sVibrator == null) {
            final RecompActivity a = sActivity;
            if (a == null)
                return null;
            if (Build.VERSION.SDK_INT >= 31) {
                VibratorManager manager =
                        (VibratorManager) a.getSystemService(Context.VIBRATOR_MANAGER_SERVICE);
                sVibrator = manager != null ? manager.getDefaultVibrator() : null;
            } else {
                sVibrator = (Vibrator) a.getSystemService(Context.VIBRATOR_SERVICE);
            }
        }
        return sVibrator;
    }

    /** A light tap tick for an on-screen control press. */
    public static void hapticTap() {
        final RecompActivity a = sActivity;
        if (a == null)
            return;
        a.runOnUiThread(() -> a.getWindow().getDecorView()
                .performHapticFeedback(HapticFeedbackConstants.KEYBOARD_TAP));
    }

    /**
     * The device's own motor, standing in for game rumble when no controller
     * is connected. `low`/`high` are the guest's low/high-frequency motor
     * strengths (0..65535, as SDL_GetGamepadRumble takes them); 0,0 stops it.
     */
    public static void deviceRumble(int low, int high) {
        final RecompActivity a = sActivity;
        final Vibrator vibrator = vibrator();
        if (a == null || vibrator == null)
            return;
        final int strength = Math.max(low, high);
        if (strength <= 0) {
            sRumbleBucket = -1;
            a.runOnUiThread(vibrator::cancel);
            return;
        }
        final int amplitude = Math.max(1, strength * 255 / 65535);
        final int bucket = amplitude / 16;
        if (bucket == sRumbleBucket)
            return;
        sRumbleBucket = bucket;
        a.runOnUiThread(() -> {
            if (vibrator.hasAmplitudeControl())
                vibrator.vibrate(VibrationEffect.createOneShot(60000, amplitude));
            else
                vibrator.vibrate(VibrationEffect.createOneShot(60000, VibrationEffect.DEFAULT_AMPLITUDE));
        });
    }
}
