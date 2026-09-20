package com.selaco.game;

import android.content.Intent;
import android.content.pm.ActivityInfo;
import android.net.Uri;
import android.os.Bundle;
import android.os.Environment;
import android.provider.Settings;
import android.util.Log;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.view.WindowManager;
import android.widget.Toast;

import org.libsdl.app.SDLActivity;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;

/**
 * Entry point for the Android build.
 *
 * SDLActivity dlopen()s the libraries named by getLibraries() and then calls
 * SDL_main in the last one, which is the engine's main() in
 * common/platform/posix/sdl/i_main.cpp.
 */
public class SelacoActivity extends SDLActivity {
    private static final String TAG = "Selaco";

    // The asset subdirectory device profiles are packaged into, and the directory name they
    // are extracted to. Must match ProfileSubdir in i_auxprofile.cpp, which is what reads
    // them back out of progdir.
    private static final String PROFILE_DIR = "profiles";

    @Override
    protected String[] getLibraries() {
        // Order matters: dependencies first. libSelaco.so has DT_NEEDED entries
        // for SDL2, zmusic and omp, and libzmusic in turn needs sndfile (which
        // carries ogg/vorbis/FLAC/opus statically). openal is not linked at all
        // but is dlopen()d by name at runtime (DYN_OPENAL), so it has to be
        // loaded from here for the packaged copy to be the one that gets found.
        return new String[] {
            "omp",
            "SDL2",
            "sndfile",
            "zmusic",
            "openal",
            "Selaco"
        };
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        // The engine looks for its resources next to "progdir", which on Android
        // is the external files dir. Unpack the build's pk3s there before any
        // native code runs.
        try {
            extractAssets();
        } catch (IOException e) {
            Log.e(TAG, "Failed to extract game resources", e);
        }

        // Ask for all-files access before the engine starts looking for Selaco.ipk3,
        // otherwise the first launch cannot see the public folders and fails even when
        // the file is sitting in /sdcard/Selaco.
        //
        // Only ask when there is no game data in our own dir. Anyone who pushed the
        // ipk3 with adb is already working and must not be nagged, and a user who
        // declines is not trapped - they just get the engine's "cannot find a game
        // IWAD" dialog, which names the folders to use.
        if (!hasGameData() && !hasAllFilesAccess()) {
            // super.onCreate() BEFORE finish(), and it is not optional. Activity.performCreate
            // clears mCalled, calls onCreate, then throws SuperNotCalledException if mCalled is
            // still false - and finish() does not set it. Returning from here without the super
            // call crashed the process on the FIRST LAUNCH of every non-adb install, which is
            // the one path a developer never sees: hasGameData() is true as soon as you have
            // adb-pushed the ipk3.
            super.onCreate(savedInstanceState);
            requestAllFilesAccess();
            finish();
            return;
        }

        super.onCreate(savedInstanceState);

        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);

        // Second-screen panel, on hardware that has one. Started after the super call and after the
        // early-return path above, so a launch that bounces to the storage-permission screen never
        // touches it.
        mAuxPanel = new AuxPanel(this);
        mAuxPanel.start();
    }

    private AuxPanel mAuxPanel;

    @Override
    protected void onDestroy() {
        // A Presentation must not outlive the activity that owns it, or the window leaks and the
        // framework complains. Tear it down here rather than in onPause: finishing it on every pause
        // would also tear it down when the user just glances at the notification shade.
        if (mAuxPanel != null) {
            mAuxPanel.stop();
            mAuxPanel = null;
        }
        super.onDestroy();
    }

    /** True if Selaco.ipk3 is already in our own external files dir (the adb route). */
    private boolean hasGameData() {
        File dir = getExternalFilesDir(null);
        return dir != null && new File(dir, "Selaco.ipk3").isFile();
    }

    private boolean hasAllFilesAccess() {
        // No version guard: MANAGE_EXTERNAL_STORAGE and isExternalStorageManager() are both API 30,
        // and minSdk is 30 for exactly that reason - it is the lowest level at which this app's
        // storage model works at all. Below it the only route was READ_EXTERNAL_STORAGE, which is a
        // RUNTIME permission that nothing here ever requested, so 26-29 believed it had access and
        // then could not read /sdcard/Selaco.
        return Environment.isExternalStorageManager();
    }

    /**
     * Send the user to Settings to grant all-files access.
     *
     * MANAGE_EXTERNAL_STORAGE cannot be granted by a runtime permission dialog - it is
     * a special access toggle in Settings - so there is no callback to wait on. The
     * activity finishes and the user relaunches once the toggle is on.
     */
    private void requestAllFilesAccess() {
        Toast.makeText(this,
                "Selaco needs file access to find Selaco.ipk3.\n"
                        + "Turn on \"Allow access to manage all files\", then reopen Selaco.\n"
                        + "Copy Selaco.ipk3 into Internal storage/Selaco/",
                Toast.LENGTH_LONG).show();

        Intent intent = new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
                Uri.parse("package:" + getPackageName()));
        try {
            startActivity(intent);
        } catch (Exception e) {
            // Some vendor images ship no per-app screen for this; fall back to the
            // global list rather than dying.
            Log.w(TAG, "Per-app all-files-access screen unavailable", e);
            try {
                startActivity(new Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION));
            } catch (Exception e2) {
                Log.e(TAG, "Cannot open storage permission settings", e2);
            }
        }
    }

    /**
     * Pin the display to landscape.
     *
     * SDLActivity recomputes the requested orientation when the window is created
     * and calls setRequestedOrientation itself, which overrides whatever the
     * manifest asked for. The engine creates its window with
     * SDL_WINDOW_RESIZABLE (sdlglvideo.cpp), and for a resizable window SDL can
     * settle on SCREEN_ORIENTATION_FULL_USER - which follows the system rotation
     * lock and lets the game come up in portrait.
     *
     * Overriding this hook drops SDL's decision entirely. The Thor is a
     * landscape handheld, so there is nothing to negotiate. Use
     * SCREEN_ORIENTATION_SENSOR_LANDSCAPE instead if you want the 180-degree flip.
     */
    @Override
    public void setOrientationBis(int w, int h, boolean resizable, String hint) {
        setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_LANDSCAPE);
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) {
            goImmersive();
            disableFocusHighlight();
        }
    }

    /**
     * Suppress Android's default focus highlight.
     *
     * SDLSurface calls setFocusable(true) + requestFocus() (SDLSurface.java:48-50).
     * The Thor's built-in controller enumerates with a KEYBOARD class, and a
     * physical keyboard takes Android out of touch mode - which turns on focus
     * highlights for focusable Views. The result is an olive-green ring composited
     * over the game on the top, left and right edges (the navigation bar clips the
     * bottom one).
     *
     * It is not a rendering bug: the colour is identical across wildly different
     * scenes and survives a magenta windowBackground, so it is drawn above the
     * surface rather than showing through it.
     *
     * setDefaultFocusHighlightEnabled is the purpose-built API for this and landed
     * in API 26, well below our minSdk of 30, so no version guard is needed.
     */
    private void disableFocusHighlight() {
        View decor = getWindow().getDecorView();
        decor.setDefaultFocusHighlightEnabled(false);
        clearFocusHighlight(decor);
    }

    private void clearFocusHighlight(View view) {
        view.setDefaultFocusHighlightEnabled(false);
        if (view instanceof ViewGroup) {
            ViewGroup group = (ViewGroup) view;
            for (int i = 0; i < group.getChildCount(); i++) {
                clearFocusHighlight(group.getChildAt(i));
            }
        }
    }

    /** Hide the status and navigation bars; they steal touches at the screen edges. */
    private void goImmersive() {
        // Draw into the display cutout as well, so a notch does not letterbox the game. Set
        // programmatically rather than in the theme because windowLayoutInDisplayCutoutMode is
        // itself an API 27 attribute and the theme has no version-qualified variant here.
        //
        // setAttributes(), not just mutating the object getAttributes() returns: that returns the
        // live LayoutParams and changing a field on it does not schedule the re-layout that makes
        // the change take effect.
        WindowManager.LayoutParams lp = getWindow().getAttributes();
        lp.layoutInDisplayCutoutMode =
            WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;
        getWindow().setAttributes(lp);

        // WindowInsetsController, not setSystemUiVisibility. The SYSTEM_UI_FLAG_* constants are
        // deprecated as of API 30 and no longer reliably take effect - they appeared to work at
        // first launch only because the theme's windowFullscreen had already given us the whole
        // screen, and then silently did nothing when re-applied after a resume.
        //
        // That is exactly what went wrong on an Android 13 device: after background -> foreground
        // the system bar inset came back, so Android reported the app content area as 1920x1025 on
        // a 1920x1080 display. The engine and the Vulkan swapchain were both still at 1920x1080 and
        // consistent - the top 55 px was simply no longer presented, which read as "the fps counter
        // disappeared" because that is where it draws.
        //
        // No version guard: this is API 30 and so is minSdk. The legacy flag path it replaced is
        // gone rather than kept as dead code.
        getWindow().setDecorFitsSystemWindows(false);
        WindowInsetsController insets = getWindow().getInsetsController();
        if (insets != null) {
            insets.hide(WindowInsets.Type.systemBars());
            // Sticky-immersive equivalent: a swipe shows the bars transiently instead of resizing
            // the window, so the surface never changes size underneath us.
            insets.setSystemBarsBehavior(
                WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
        }
    }

    /**
     * Copy the engine .pk3s out of the APK into external storage.
     *
     * They go to the external files dir rather than internal storage because
     * that is what i_main.cpp sets progdir to, and it is also where the user
     * drops the Selaco game data - so the engine finds everything in one place.
     *
     * Files are only rewritten when the size differs, which keeps warm starts
     * fast while still picking up a new build.
     */
    private void extractAssets() throws IOException {
        File targetDir = getExternalFilesDir(null);
        if (targetDir == null) {
            throw new IOException("External storage is not available");
        }
        if (!targetDir.exists() && !targetDir.mkdirs()) {
            throw new IOException("Could not create " + targetDir);
        }

        String[] assets = getAssets().list("");
        if (assets == null) {
            return;
        }

        for (String name : assets) {
            boolean isPk3 = name.endsWith(".pk3");
            boolean isConfig = name.equals("autoexec.cfg");
            if (!isPk3 && !isConfig) {
                continue;
            }

            File target = new File(targetDir, name);

            // autoexec.cfg is our shipped Android default config. Write it once and
            // never again, so the player's own edits survive a reinstall - delete
            // the file on the device to get the defaults back. The pk3s are engine
            // resources and must track the build, so those are refreshed whenever
            // the size differs.
            if (isConfig && target.exists()) {
                continue;
            }

            try (InputStream in = getAssets().open(name)) {
                if (target.exists() && target.length() == in.available()) {
                    continue;
                }
            }

            Log.i(TAG, "Extracting " + name + " to " + target);
            try (InputStream in = getAssets().open(name);
                 OutputStream out = new FileOutputStream(target)) {
                copyStream(in, out);
            }
        }

        extractProfiles(targetDir);
    }

    /**
     * Copy the per-device profiles out of the APK into &lt;externalFilesDir&gt;/profiles.
     *
     * They live in an asset subdirectory rather than at the root, so the root listing
     * above does not see them - getAssets().list() is not recursive.
     *
     * Unlike autoexec.cfg these are ALWAYS overwritten, and unlike the pk3s they are
     * overwritten unconditionally rather than only when the size differs. The repo is the
     * source of truth for profiles: a PR that retunes a device has to reach existing
     * installs, and a size comparison would let an edited-on-device file of the same
     * length win over the packaged one. They are a few KB in total, so rewriting them on
     * every launch costs nothing worth protecting. profiles/README.md tells contributors
     * that local edits do not survive a reinstall, so this is the behaviour it promises.
     *
     * Failures here are logged and swallowed: a profile is only read when the player asks
     * for one by name, so not having them must not stop the game from starting.
     */
    private void extractProfiles(File targetDir) {
        try {
            String[] profiles = getAssets().list(PROFILE_DIR);
            if (profiles == null || profiles.length == 0) {
                Log.i(TAG, "No " + PROFILE_DIR + " assets to extract");
                return;
            }

            File profileDir = new File(targetDir, PROFILE_DIR);
            if (!profileDir.exists() && !profileDir.mkdirs()) {
                Log.w(TAG, "Could not create " + profileDir + " - no device profiles");
                return;
            }

            for (String name : profiles) {
                String assetPath = PROFILE_DIR + "/" + name;
                File target = new File(profileDir, name);
                Log.i(TAG, "Extracting " + assetPath + " to " + target);
                try (InputStream in = getAssets().open(assetPath);
                     OutputStream out = new FileOutputStream(target)) {
                    copyStream(in, out);
                }
            }
        } catch (IOException e) {
            Log.w(TAG, "Could not extract device profiles", e);
        }
    }

    private static void copyStream(InputStream in, OutputStream out) throws IOException {
        byte[] buffer = new byte[64 * 1024];
        int read;
        while ((read = in.read(buffer)) != -1) {
            out.write(buffer, 0, read);
        }
    }
}
