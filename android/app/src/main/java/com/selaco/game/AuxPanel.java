package com.selaco.game;

import android.app.Presentation;
import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.hardware.display.DisplayManager;
import android.os.Bundle;
import android.util.Log;
import android.view.Display;
import android.view.View;
import android.view.WindowManager;

/**
 * Second-screen panel for dual-screen Android handhelds (the AYN Thor is a 3DS-style clamshell, so
 * both panels face the player).
 *
 * The engine knows nothing about this. No Vulkan surface, no swapchain, no renderer participation -
 * the panel is an ordinary Presentation drawing an ordinary View, and native only ever pushes a few
 * primitives into it. That is deliberate: an optional second screen must not be able to take the
 * main one down, and the cheapest guarantee is to give it no graphics resources to share.
 *
 * MILESTONE SCOPE: grey rectangle plus one number. It exists to falsify four structural
 * assumptions before any content is built - see i_auxpanel.cpp for the list.
 */
public final class AuxPanel implements DisplayManager.DisplayListener {

    private static final String TAG = "selaco-ea";

    /** Java -> native, the only direction. Tells the engine whether a panel is actually up. */
    private static native void nativeAuxEnable(boolean on);

    /**
     * native -> Java, called from the GAME thread at frame rate.
     *
     * It must never block, allocate or lock: a wait here would stall the render loop and freeze
     * BOTH screens, which is the single failure route this whole design exists to avoid. A plain
     * volatile store is all it is allowed to do. The panel picks the value up on its own clock.
     */
    static void pushState(int gametic) {
        sState = gametic;
    }

    /**
     * native -> Java: the visible codex table of contents, as "depth:title" lines.
     *
     * An EMPTY string means "show nothing" and is how the panel is cleared - when the player is at
     * the menu, or the bridge cannot confirm unlock state. Java deliberately keeps no codex state of
     * its own beyond this one reference, so the last push is always the authority and nothing here
     * can outlive the unlock state that justified it.
     *
     * Called only when the visible set changes, never per frame.
     */
    static void pushCodex(String toc) {
        sCodex = (toc == null) ? "" : toc;
    }

    private static volatile int sState = 0;
    private static volatile String sCodex = "";

    private final Context mContext;
    private final DisplayManager mDisplayManager;
    private AuxPresentation mPresentation;
    private int mDisplayId = -1;

    AuxPanel(Context context) {
        mContext = context;
        mDisplayManager = (DisplayManager) context.getSystemService(Context.DISPLAY_SERVICE);
    }

    void start() {
        if (mDisplayManager == null) {
            Log.i(TAG, "AuxPanel: no DisplayManager, second screen unavailable");
            return;
        }
        mDisplayManager.registerDisplayListener(this, null);
        updateDisplay();
    }

    void stop() {
        if (mDisplayManager != null) {
            mDisplayManager.unregisterDisplayListener(this);
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
        Display target = pickDisplay();

        if (target == null) {
            if (mPresentation != null) {
                Log.i(TAG, "AuxPanel: target display gone, releasing panel");
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
            return null;
        }
        for (Display d : candidates) {
            if (d.getDisplayId() == Display.DEFAULT_DISPLAY) continue;
            if (!d.isValid()) continue;
            if (d.getState() == Display.STATE_OFF) continue;
            return d;
        }
        return null;
    }

    private void releasePresentation() {
        // Tell native to stop BEFORE the window goes away, so no push can be in flight against a
        // dismissed presentation.
        nativeAuxEnable(false);

        if (mPresentation != null) {
            mPresentation.dismiss();
            mPresentation = null;
        }
        mDisplayId = -1;
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
     * Grey fill plus the pushed number.
     *
     * It redraws on its own ~15 Hz clock rather than being driven by the push, so the game thread
     * never waits on view invalidation. A CHANGING number is the point: a static one cannot
     * distinguish "presenting correctly" from "frozen on frame one".
     */
    private static final class AuxView extends View {

        private static final long REDRAW_MS = 66;

        private static final int BG = 0xFF12161C;

        private final Paint mTitle = new Paint();
        private final Paint mBody = new Paint();

        AuxView(Context context) {
            super(context);
            mTitle.setTextSize(52f);
            mTitle.setFakeBoldText(true);
            mTitle.setAntiAlias(true);
            mBody.setTextSize(34f);
            mBody.setAntiAlias(true);
        }

        @Override
        protected void onDraw(Canvas canvas) {
            super.onDraw(canvas);
            canvas.drawColor(BG);

            // Snapshot once: the game thread can replace it mid-draw, and a half-old, half-new
            // frame would be worse than a frame that is one push behind.
            final String toc = sCodex;

            if (toc.isEmpty()) {
                drawPlaceholder(canvas);
            } else {
                drawCodex(canvas, toc);
            }
            postInvalidateDelayed(REDRAW_MS);
        }

        /** Shown whenever there is no codex to show - the menu, or an unconfirmed bridge. */
        private void drawPlaceholder(Canvas canvas) {
            mTitle.setColor(0xFF6E7A8A);
            canvas.drawText("SELACO", 48f, 110f, mTitle);
            mBody.setColor(0xFF54606E);
            canvas.drawText("codex available in-game", 48f, 168f, mBody);
        }

        private void drawCodex(Canvas canvas, String toc) {
            mTitle.setColor(0xFFDCE3EE);
            canvas.drawText("CODEX", 48f, 86f, mTitle);

            float y = 150f;
            final float lineH = 46f;
            final int h = getHeight();

            int from = 0;
            while (from < toc.length() && y < h - 12f) {
                int nl = toc.indexOf('\n', from);
                if (nl < 0) nl = toc.length();
                final String rec = toc.substring(from, nl);
                from = nl + 1;

                // "depth:title" - malformed records are skipped rather than drawn raw, so a protocol
                // mistake shows up as missing text instead of garbage on screen.
                final int colon = rec.indexOf(':');
                if (colon <= 0) continue;
                int depth;
                try {
                    depth = Integer.parseInt(rec.substring(0, colon));
                } catch (NumberFormatException e) {
                    continue;
                }
                final String title = rec.substring(colon + 1);

                mBody.setColor(depth <= 1 ? 0xFFBFC9D8 : 0xFF8A94A4);
                canvas.drawText(title, 48f + depth * 34f, y, mBody);
                y += lineH;
            }
        }

        @Override
        protected void onAttachedToWindow() {
            super.onAttachedToWindow();
            postInvalidateDelayed(REDRAW_MS);
        }
    }
}
