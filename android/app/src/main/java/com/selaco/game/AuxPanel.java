package com.selaco.game;

import android.app.Presentation;
import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.Canvas;
import android.graphics.Rect;
import android.hardware.display.DisplayManager;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.view.Display;
import android.view.View;
import android.view.WindowManager;

import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.nio.ByteBuffer;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;

/**
 * Second-screen panel for dual-screen Android handhelds (the AYN Thor is a 3DS-style clamshell, so
 * both panels face the player).
 *
 * The engine knows nothing about this. No Vulkan surface, no swapchain, no present queue - the panel
 * is an ordinary Presentation drawing an ordinary View, and native pushes it a finished RGBA readback
 * of an offscreen canvas. An optional second screen must not be able to take the main one down, and
 * sharing no long-lived graphics resource with the main swapchain is what guarantees that; the
 * readback itself is not free, and i_auxcanvas.cpp documents what it costs.
 *
 * What the panel shows, in order: the codex readback if one has arrived, else Selaco's own STARTUP.png
 * decoded out of the player's ipk3, else black.
 */
public final class AuxPanel implements DisplayManager.DisplayListener {

    private static final String TAG = "selaco-ea";

    /** Java -> native, the only direction. Tells the engine whether a panel is actually up. */
    private static native void nativeAuxEnable(boolean on);

    /**
     * native -> Java: a completed canvas readback, called from the GAME thread when the canvas
     * content changes (and on mode 4's publish interval), never at full frame rate - each readback
     * costs a full device stall, see i_auxpanel.cpp.
     *
     * Buffer-safety discipline for the JNI side: {@code buffer} aliases the engine's persistent
     * staging mapping. copyPixelsFromBuffer below completes synchronously, so by the time this
     * method returns every byte has been copied out of native memory - the game thread is then free
     * to start its next readback into that same native buffer, and the UI thread never touches
     * native memory at all, only a Bitmap copy made here.
     *
     * That copy is double-buffered on purpose. A single reused Bitmap would let this call's
     * copyPixelsFromBuffer mutate pixels that AuxView.onDraw is concurrently reading via drawBitmap
     * on the other thread - a torn frame, not a crash, but the device-verification step for this
     * transport reads a screencap to judge orientation and channel order, and a tear at that exact
     * moment would be indistinguishable from a real transport bug. Writing into the buffer that is
     * NOT currently published and then publishing it through the existing volatile store narrows
     * that window for a fixed extra ~5.3 MB.
     *
     * Narrows, not closes: two slots survive exactly ONE push. onDraw snapshots sPixels and nothing
     * records which Bitmap it is still reading, so the SECOND subsequent push wraps back onto that
     * slot. It needs one onDraw to outlast two publishes - far longer than drawing a 1240x1080 bitmap
     * takes - and the cost is a torn frame, so it is left as is rather than adding a third slot or
     * taking sPixelsLock on the UI thread's draw path.
     */
    static void pushPixels(ByteBuffer buffer, int w, int h) {
        synchronized (sPixelsLock) {
            // The panel went away between the engine deciding to push and this call arriving.
            // nativeAuxEnable(false) is a relaxed store the game thread may not have observed yet, so
            // this is the only test that actually orders against releasePresentation - without it the
            // store below republishes a torn-down panel's last frame and the NEXT presentation opens on
            // it instead of on the splash.
            if (sView == null) {
                return;
            }

            // Flip to the other slot before writing, so this write can never land on the Bitmap sPixels
            // currently points at - that Bitmap may be mid-read on the UI thread right now.
            final int back = sPixelsIndex ^ 1;
            Bitmap bmp = sPixelsBuf[back];
            if (bmp == null || bmp.getWidth() != w || bmp.getHeight() != h) {
                bmp = Bitmap.createBitmap(w, h, Bitmap.Config.ARGB_8888);
                sPixelsBuf[back] = bmp;
            }
            // rewind() is mandatory, not defensive. copyPixelsFromBuffer reads from the buffer's current
            // position and advances it by the bytes consumed, and the native side hands us the SAME
            // cached direct buffer every push - so without this the second push finds position at
            // capacity, sees zero remaining, and throws "Buffer not large enough for pixels".
            buffer.rewind();
            bmp.copyPixelsFromBuffer(buffer);

            sPixelsIndex = back;
            sPixels = bmp;
        }

        final AuxView view = sView;
        if (view != null) {
            view.postInvalidate();
        }
    }

    /**
     * native -> Java, called once on level exit, best-effort. Not the AuxLastMode/AuxWasLive
     * machinery the rest of this file uses for edge detection - the caller just notices gamestate
     * leaving GS_LEVEL with a local flag, and a missed or doubled call here is a cosmetic frame or
     * two, not a correctness bug worth more machinery.
     *
     * Nulling sPixels is the entire effect: onDraw already prefers sStartupImage and then black once
     * there is no codex frame to show, so clearing the stale one is all that is needed to fall
     * through to whichever of those applies.
     */
    static void clearPixels() {
        sPixels = null;
        // Snapshotted: sView is volatile and the UI thread nulls it in releasePresentation, so
        // testing the field and then dereferencing it can see two different values.
        final AuxView view = sView;
        if (view != null) {
            view.postInvalidate();
        }
    }

    /**
     * native -> Java: the aux_panel cvar changed. Unlike the two above, this cannot do its work
     * where it is received. A cvar callback runs on the GAME thread, and a Presentation may only be
     * created or dismissed on the UI thread, so all this does is hand the decision to the main
     * Looper - the same Looper updateDisplay() already runs on for display hotplug, which is what
     * keeps a user toggle and a hotplug from racing each other without a lock of their own.
     *
     * Stopping the pushes is not enough on its own: the Presentation would stay on screen frozen on
     * the last frame the engine gave it. The panel has to actually go away and come back.
     */
    static void setPanelEnabled(boolean enabled) {
        sMainHandler.post(() -> {
            sPanelEnabled = enabled;
            // Null before start() or after stop(): nothing to do beyond remembering the state, which
            // the next start() will honour. The engine can set the cvar from its config before this
            // process has an AuxPanel, and can set it again after the activity is destroyed.
            final AuxPanel panel = sInstance;
            if (panel != null) {
                panel.updateDisplay();
            }
        });
    }

    // Selaco's own startup splash, decoded once from the player's ipk3 and reused for the life of the
    // process - see loadStartupImage. Shown by AuxView whenever there is no codex frame; never shipped
    // in the APK, since Selaco is a commercial asset and this repo is public.
    private static volatile Bitmap sStartupImage;
    private static boolean sStartupLoadStarted = false;

    // Two Bitmaps, alternated by pushPixels: one may be published (referenced by sPixels and
    // possibly mid-read by onDraw) while the other is being written.
    //
    // Guarded by sPixelsLock rather than left to the game thread, because releasePresentation()
    // clears them from the UI THREAD. These are plain fields, so without the lock the game thread
    // has no happens-before against that teardown: it could miss the nulls entirely (retaining
    // ~10 MB of Bitmaps) and, worse, finish a push that was already in flight and republish a
    // dead panel's last frame into the next presentation. The lock is uncontended on every normal
    // push and releasePresentation is rare, so the cost is nil.
    private static final Object sPixelsLock = new Object();
    private static final Bitmap[] sPixelsBuf = new Bitmap[2];
    private static int sPixelsIndex = 0;
    private static volatile Bitmap sPixels;

    // Set by the currently-live AuxView so pushPixels has something to invalidate; cleared in
    // releasePresentation so a push after teardown cannot touch a dismissed view.
    //
    // Safe against a stale clobber because AuxPanel's own bookkeeping (start/stop, and updateDisplay
    // via the DisplayListener callbacks) all run on one Looper: registerDisplayListener(this, null)
    // binds the callbacks to the calling thread's Looper, which is the same UI thread start()/stop()
    // are called from (SelacoActivity.onCreate/onDestroy). Within updateDisplay(), releasePresentation()
    // (which nulls sView) always runs to completion BEFORE the replacement AuxPresentation is
    // constructed and shown - Presentation.show() calls onCreate(), and therefore the new AuxView's
    // constructor, synchronously on the calling thread, not via a posted message. So there is no
    // ordering in which a new AuxView's "sView = this" could run before the old one's "sView = null",
    // and the null can never clobber a live reference.
    private static volatile AuxView sView;

    // The thread hop for setPanelEnabled, and the live panel it has to reach.
    //
    // sInstance is the static handle a static JNI entry point needs to get at the per-instance
    // Presentation bookkeeping; it is set in start() and cleared in stop(), both on the UI thread,
    // and volatile because the game thread does not read it but the posted Runnable does - which is
    // the UI thread again, so the volatile is belt and braces rather than load-bearing.
    //
    // sPanelEnabled mirrors the aux_panel cvar, whose default is likewise true. It exists because
    // releasing the Presentation is not a stable state on its own: any later display hotplug calls
    // updateDisplay(), which would happily put back a panel the player turned off. Only ever touched
    // on the UI thread - written in the posted Runnable, read in updateDisplay() - so it needs no
    // synchronisation and deliberately is not volatile.
    private static final Handler sMainHandler = new Handler(Looper.getMainLooper());
    private static volatile AuxPanel sInstance;
    private static boolean sPanelEnabled = true;

    private final Context mContext;
    private final DisplayManager mDisplayManager;
    private AuxPresentation mPresentation;
    private int mDisplayId = -1;

    // Whether the "no second screen" line has already been said for the current dry spell. UI thread
    // only, like the rest of this bookkeeping, so it needs no synchronisation.
    private boolean mLoggedNoDisplay;

    AuxPanel(Context context) {
        mContext = context;
        mDisplayManager = (DisplayManager) context.getSystemService(Context.DISPLAY_SERVICE);
    }

    void start() {
        if (mDisplayManager == null) {
            Log.i(TAG, "AuxPanel: no DisplayManager, second screen unavailable");
            return;
        }
        // Before the first updateDisplay(), so a cvar toggle that arrives while we are still in here
        // finds the instance it needs - the post cannot run until this returns, since we are on the
        // Looper it posts to.
        sInstance = this;
        mDisplayManager.registerDisplayListener(this, null);
        updateDisplay();
        loadStartupImageAsync();
    }

    /**
     * Kick off the STARTUP.png decode on a background thread. onCreate is on the critical path to
     * the game window appearing, so this must never block it - the zip open and decode of a ~94 KB
     * PNG only costs a few ms, but a few ms here is a few ms of a first frame that has nothing else
     * to wait on.
     */
    private void loadStartupImageAsync() {
        if (sStartupLoadStarted) {
            return;     // decode once and reuse, even across a second AuxPanel on the same process
        }
        sStartupLoadStarted = true;

        final Context context = mContext;
        new Thread(() -> {
            Bitmap bmp = loadStartupImage(context);
            if (bmp != null) {
                sStartupImage = bmp;
                // Snapshotted: sView is volatile and the UI thread nulls it in releasePresentation.
                final AuxView view = sView;
                if (view != null) {
                    view.postInvalidate();
                }
            }
        }, "AuxStartupLoad").start();
    }

    /**
     * Read STARTUP.png out of the player's own Selaco.ipk3. Never bundled in the APK - it is a
     * commercial game asset and this repo is public - so if neither copy of the ipk3 exists yet, the
     * panel simply has no splash to show, which is correct behaviour, not a failure to log.
     */
    private static Bitmap loadStartupImage(Context context) {
        File extDir = context.getExternalFilesDir(null);
        // The adb route (pushed into our own external files dir) and the public folder the README
        // tells users to use - check both, prefer whichever exists.
        File candidate1 = (extDir != null) ? new File(extDir, "Selaco.ipk3") : null;
        File candidate2 = new File("/sdcard/Selaco/Selaco.ipk3");
        File ipk3 = (candidate1 != null && candidate1.isFile()) ? candidate1
                : (candidate2.isFile() ? candidate2 : null);
        if (ipk3 == null) {
            return null;
        }

        try (ZipFile zip = new ZipFile(ipk3)) {
            // Plain "STARTUP.png" - no directory, and unlike most entries in this archive, no
            // backslash separators to worry about.
            ZipEntry entry = zip.getEntry("STARTUP.png");
            if (entry == null) {
                return null;
            }
            try (InputStream in = zip.getInputStream(entry)) {
                return BitmapFactory.decodeStream(in);
            }
        } catch (IOException e) {
            Log.w(TAG, "AuxPanel: could not read STARTUP.png from " + ipk3, e);
            return null;
        }
    }

    void stop() {
        if (mDisplayManager != null) {
            mDisplayManager.unregisterDisplayListener(this);
        }
        // Cleared before the release, so a setPanelEnabled already queued behind us cannot resurrect
        // a panel on an activity that is going away.
        if (sInstance == this) {
            sInstance = null;
        }
        releasePresentation();
    }

    // All three hotplug callbacks funnel into one idempotent update, so there is no ordering
    // subtlety between them and no state machine to get wrong.
    @Override public void onDisplayAdded(int displayId) { updateDisplay(); }
    @Override public void onDisplayRemoved(int displayId) { updateDisplay(); }
    @Override public void onDisplayChanged(int displayId) { updateDisplay(); }

    /**
     * Pick a target display and make the panel match reality.
     *
     * There is deliberately NO hidden VirtualDisplay fallback. Azahar keeps one alive so its native
     * side can assume a surface always exists, which lets it block waiting for one - but the price
     * is a full extra render and present every frame even for users with one screen, which a 30 fps
     * handheld shooter cannot afford. We take the real absence path instead: when nothing qualifies,
     * the panel goes away and native is told to stop.
     */
    private void updateDisplay() {
        // aux_panel off is handled as "no display qualifies", so the player's choice and a real
        // absence take the same path rather than being two states that can disagree.
        Display target = sPanelEnabled ? pickDisplay() : null;

        if (target == null) {
            if (mPresentation != null) {
                Log.i(TAG, sPanelEnabled ? "AuxPanel: target display gone, releasing panel"
                        : "AuxPanel: aux_panel turned off, releasing panel");
            }
            releasePresentation();
            return;
        }

        if (mPresentation != null && mDisplayId == target.getDisplayId()) {
            return;     // already up on the right display
        }

        releasePresentation();

        AuxPresentation pres = new AuxPresentation(mContext, target);
        try {
            pres.show();
        } catch (WindowManager.InvalidDisplayException e) {
            // The display vanished between the pick and the show. Not an error - wait for the next
            // listener callback rather than retrying in a loop.
            Log.i(TAG, "AuxPanel: display " + target.getDisplayId() + " invalid at show(): " + e);
            nativeAuxEnable(false);
            return;
        } catch (WindowManager.BadTokenException e) {
            Log.w(TAG, "AuxPanel: bad token showing on display " + target.getDisplayId() + ": " + e);
            nativeAuxEnable(false);
            return;
        }

        mPresentation = pres;
        mDisplayId = target.getDisplayId();
        // Armed again, so a later disconnect gets its own line rather than being swallowed.
        mLoggedNoDisplay = false;
        Log.i(TAG, "AuxPanel: panel up on display " + mDisplayId
                + " (" + target.getName() + ")");

        // Enable native pushes only once the window is actually up, never before.
        nativeAuxEnable(true);
    }

    /**
     * The first display that is not ours, not the default, and actually on.
     *
     * DISPLAY_CATEGORY_PRESENTATION is the framework's own answer to "where may I put secondary
     * content", and it is what makes this generic rather than Thor-specific. The STATE_OFF test
     * matters on this hardware: a panel that is asleep still enumerates, and presenting to it
     * produces a black window that reads as a renderer bug rather than a sleeping screen.
     */
    private Display pickDisplay() {
        Display[] candidates = mDisplayManager.getDisplays(DisplayManager.DISPLAY_CATEGORY_PRESENTATION);
        if (candidates == null) {
            logNoDisplay("getDisplays returned null");
            return null;
        }
        // Built only on the failing path, so the normal case does no string work at all. Without this
        // a missing panel is silent and indistinguishable from a single-screen device behaving
        // correctly - which is the one thing you most want to know when the panel does not appear.
        StringBuilder rejected = null;
        for (Display d : candidates) {
            final String why;
            if (d.getDisplayId() == Display.DEFAULT_DISPLAY) why = "is the default display";
            else if (!d.isValid()) why = "is not valid";
            else if (d.getState() == Display.STATE_OFF) why = "is off";
            else return d;

            if (rejected == null) rejected = new StringBuilder();
            else rejected.append(", ");
            rejected.append("id ").append(d.getDisplayId()).append(' ').append(why);
        }
        logNoDisplay(candidates.length + " presentation display(s), none usable"
                + (rejected == null ? "" : ": " + rejected));
        return null;
    }

    /**
     * Said once per dry spell rather than on every call, because updateDisplay() runs on every hotplug
     * event and a single-screen device would otherwise repeat this forever. Reset as soon as a panel
     * comes up, so a later disconnect is reported again rather than swallowed as a duplicate.
     */
    private void logNoDisplay(String reason) {
        if (mLoggedNoDisplay) {
            return;
        }
        mLoggedNoDisplay = true;
        Log.i(TAG, "AuxPanel: no second screen - " + reason);
    }

    private void releasePresentation() {
        // Tell native to stop first, so it stops asking. This is a hint rather than a barrier -
        // AuxLive is a relaxed store the game thread may not see for a frame - so the actual
        // ordering against an in-flight push is the sPixelsLock section below.
        nativeAuxEnable(false);

        if (mPresentation != null) {
            mPresentation.dismiss();
            mPresentation = null;
        }
        mDisplayId = -1;

        // The Bitmaps and view reference must not outlive this Presentation: the next one starts
        // from the startup image (or black) rather than one frame of a torn-down window's last pixels.
        //
        // Under sPixelsLock because pushPixels runs on the GAME thread and may be part way through a
        // copy right now; nulling sView inside the same section is what makes its early return the
        // authority on whether a panel is up.
        synchronized (sPixelsLock) {
            sPixelsBuf[0] = null;
            sPixelsBuf[1] = null;
            sPixelsIndex = 0;
            sPixels = null;
            sView = null;
        }
    }

    /** The second-screen window. */
    private static final class AuxPresentation extends Presentation {

        AuxPresentation(Context outerContext, Display display) {
            super(outerContext, display);
        }

        @Override
        protected void onCreate(Bundle savedInstanceState) {
            super.onCreate(savedInstanceState);

            // FLAG_NOT_FOCUSABLE is not optional, and it is load-bearing for a reason specific to
            // this engine: if this window takes focus, the game window gets
            // SDL_WINDOWEVENT_FOCUS_LOST, which sets AppActive = false, and D_Display then returns
            // early - so BOTH screens freeze while the process runs perfectly. Azahar and dusklight
            // independently set this flag; we need it more than either of them.
            //
            // FLAG_NOT_TOUCH_MODAL keeps the main screen's input working when this panel is up.
            //
            // FLAG_KEEP_SCREEN_ON is dusklight's requirement, which azahar omits: the second panel
            // gets no input of its own during play, so without this it sleeps - and a sleeping panel
            // reads as a broken renderer, exactly the trap recorded in CLAUDE.md about screencaps
            // returning pure black.
            getWindow().addFlags(WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE
                    | WindowManager.LayoutParams.FLAG_NOT_TOUCH_MODAL
                    | WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);

            setContentView(new AuxView(getContext()));
        }
    }

    /**
     * The codex readback if one has arrived, else Selaco's own startup splash if it has loaded,
     * else black. No text path any more - black is the universal floor when neither has anything to
     * show, rather than a placeholder or a table-of-contents overlay.
     *
     * Pushes drive invalidation now (pushPixels calls postInvalidate() directly), not a self-arming
     * redraw clock - the old ~15 Hz unconditional repaint was already pointless and would be
     * actively harmful against a multi-megabyte Bitmap. onAttachedToWindow still schedules exactly
     * one delayed redraw so the startup image or black floor appears before any push has happened.
     */
    private static final class AuxView extends View {

        private static final long REDRAW_MS = 66;

        private static final int BLACK = 0xFF000000;

        private final Rect mSrc = new Rect();
        private final Rect mDst = new Rect();

        AuxView(Context context) {
            super(context);

            // Only one panel is ever live at a time, so a plain static back-reference is enough for
            // pushPixels to reach the view it needs to invalidate.
            sView = this;
        }

        @Override
        protected void onDraw(Canvas canvas) {
            super.onDraw(canvas);
            canvas.drawColor(BLACK);

            // Snapshot once: the game thread can replace either reference mid-draw, and a half-old,
            // half-new frame would be worse than a frame that is one push behind.
            final Bitmap pixels = sPixels;
            if (pixels != null) {
                mDst.set(0, 0, getWidth(), getHeight());
                // The engine stores render targets bottom-up, so the rows arrive in reverse order -
                // the same reason CopyScreenToBuffer carries a vertical flip in its own conversion
                // loop. Flipping here rather than in the native copy keeps it free: the compositor
                // applies the scale, where a row-reversing memcpy would cost a ~5.3 MB pass on the
                // game thread every readback.
                canvas.save();
                canvas.scale(1f, -1f, 0f, getHeight() / 2f);
                canvas.drawBitmap(pixels, null, mDst, null);
                canvas.restore();
                return;
            }

            final Bitmap startup = sStartupImage;
            if (startup != null) {
                drawStartupImage(canvas, startup);
            }
            // else: nothing loaded yet, or no ipk3 found - the black floor drawn above stands as-is.
        }

        /**
         * Center-crop the startup splash to fill the panel: scale up by whichever axis needs more
         * to cover it, then take a same-aspect slice out of the middle of the source rather than
         * distorting or letterboxing. An ordinary Android bitmap, so unlike the codex path above it
         * is NOT flipped.
         */
        private void drawStartupImage(Canvas canvas, Bitmap startup) {
            final int viewW = getWidth();
            final int viewH = getHeight();
            final int imgW = startup.getWidth();
            final int imgH = startup.getHeight();
            if (viewW <= 0 || viewH <= 0 || imgW <= 0 || imgH <= 0) {
                return;
            }

            // Derived from the actual view size, never a hardcoded 1240x1080 - the framework does
            // not guarantee this view gets exactly the panel's full reported resolution.
            final float scale = Math.max((float) viewW / imgW, (float) viewH / imgH);
            final int srcW = Math.min(imgW, Math.round(viewW / scale));
            final int srcH = Math.min(imgH, Math.round(viewH / scale));
            final int srcX = (imgW - srcW) / 2;
            final int srcY = (imgH - srcH) / 2;

            mSrc.set(srcX, srcY, srcX + srcW, srcY + srcH);
            mDst.set(0, 0, viewW, viewH);
            canvas.drawBitmap(startup, mSrc, mDst, null);
        }

        @Override
        protected void onAttachedToWindow() {
            super.onAttachedToWindow();
            postInvalidateDelayed(REDRAW_MS);
        }
    }
}
