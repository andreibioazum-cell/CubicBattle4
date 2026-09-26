/*
 * The package is com.cb4, in AndroidManifest.xml and in the JNI names of
 * runtime.c (Java_com_cb4_GameActivity_*). All three must agree: if one of them
 * differs, System.loadLibrary still works, but the native editor methods
 * (nativeReplaceText, nativeSubmitText, nativeKeyboardHidden) are not found,
 * nativeReady stays false, and the keyboard opens while nothing reaches the input
 * field.
 *
 * The file deliberately sits in the old com/dimscript/gamedemo directory, whose
 * path is baked into the build step of .github/workflows/main.yml. javac does not
 * care: the class is compiled by its declared package, into
 * classes/com/cb4/GameActivity.class.
 */
package com.cb4;

import android.app.AlertDialog;
import android.app.NativeActivity;
import android.content.DialogInterface;
import android.content.Context;
import android.graphics.Color;
import android.graphics.Rect;
import android.os.Bundle;
import android.text.Editable;
import android.text.InputFilter;
import android.text.InputType;
import android.text.TextWatcher;
import android.view.Gravity;
import android.view.KeyEvent;
import android.view.View;
import android.view.ViewTreeObserver;
import android.view.WindowManager;
import android.view.inputmethod.EditorInfo;
import android.view.inputmethod.InputMethodManager;
import android.widget.EditText;
import android.widget.TextView;
import android.widget.FrameLayout;

/**
 * NativeActivity with a real, focusable Android text editor used only as the
 * input connection for in-game fields. The game continues to draw the field
 * itself; this 1-pixel editor makes every soft IME deliver commitText events.
 *
 * NativeActivity's surface steals view-focus after IME-driven resizes. If the
 * editor loses focus, the keyboard stays on screen but typed characters go
 * nowhere. wantKeyboard stays true until the game hides the IME, and we
 * reclaim focus whenever the native surface takes it away.
 */
public final class GameActivity extends NativeActivity {
    /*
     * NativeActivity loads the game .so with dlopen(), which does not register
     * it with the Java runtime: without an explicit System.loadLibrary the
     * first call to any native method below threw UnsatisfiedLinkError and
     * crashed the app the moment the keyboard was opened.
     */
    private static boolean nativeReady;
    static {
        try {
            System.loadLibrary("ds_game");
            nativeReady = true;
        } catch (UnsatisfiedLinkError error) {
            nativeReady = false;
        }
    }

    /* The alpha notice is shown once per launch, not after every rotation or
     * return from the recents screen. */
    private boolean alphaNoticeShown;

    private EditText chatEditor;
    private boolean syncingFromNative;
    private boolean keyboardWasVisible;
    /* Game asked for the IME. Stays true across transient focus losses. */
    private volatile boolean wantKeyboard;
    /* Read from the game thread: while true this editor owns the whole text. */
    private volatile boolean editorActive;
    /* Whether the IME is on screen, judged by the real screen size in onGlobalLayout. */
    private volatile boolean imeLooksVisible;
    private int showAttempts;
    /* A held Backspace gives a run of quick deletions from key repeat, and after
     * several in a row the whole text is cleared at once. */
    private long lastDeleteAt;
    private int deleteStreak;
    private boolean pendingDelete;

    private native void nativeReplaceText(String text);
    private native void nativeSubmitText();
    private native void nativeKeyboardHidden();

    private void replaceTextNative(String text) {
        if (nativeReady) try { nativeReplaceText(text); } catch (UnsatisfiedLinkError ignored) { }
    }
    private void submitTextNative() {
        if (nativeReady) try { nativeSubmitText(); } catch (UnsatisfiedLinkError ignored) { }
    }
    private void keyboardHiddenNative() {
        if (nativeReady) try { nativeKeyboardHidden(); } catch (UnsatisfiedLinkError ignored) { }
    }

    /**
     * Keeps the game surface fullscreen. The legacy flags are supported by the
     * complete Android 10–14 range targeted by this APK; sticky mode lets a
     * swipe reveal system bars only temporarily.
     */
    @SuppressWarnings("deprecation")
    private void enterImmersiveMode() {
        getWindow().getDecorView().setSystemUiVisibility(
                View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
                        | View.SYSTEM_UI_FLAG_FULLSCREEN
                        | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                        | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                        | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
                        | View.SYSTEM_UI_FLAG_LAYOUT_STABLE);
    }

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        getWindow().setSoftInputMode(WindowManager.LayoutParams.SOFT_INPUT_ADJUST_RESIZE);
        /* The display refresh rate is deliberately left alone: on TECNO it is not
         * locked to 30 Hz, the game really swings between 30 and 40 fps, and asking
         * for exactly 60 only gets in the way of Android picking a panel mode. */
        enterImmersiveMode();

        if (state != null) alphaNoticeShown = state.getBoolean("alphaNoticeShown", false);

        chatEditor = new EditText(this);
        chatEditor.setSingleLine(true);
        // Transparent text with alpha=1: at alpha=0 Gboard and the system IME
        // treat the field as dead and hand over no characters at all.
        chatEditor.setTextColor(Color.TRANSPARENT);
        chatEditor.setHintTextColor(Color.TRANSPARENT);
        chatEditor.setBackgroundColor(Color.TRANSPARENT);
        chatEditor.setCursorVisible(false);
        chatEditor.setAlpha(1f);
        chatEditor.setGravity(Gravity.TOP | Gravity.START);
        chatEditor.setFocusable(true);
        chatEditor.setFocusableInTouchMode(true);
        chatEditor.setClickable(false);
        chatEditor.setLongClickable(false);
        /* While the editor is visible it covers the thin strip of the native
         * surface, so a tap there counts as a tap outside the game field and closes
         * the IME instead of returning focus to it. */
        chatEditor.setOnTouchListener(new View.OnTouchListener() {
            @Override
            public boolean onTouch(View view, android.view.MotionEvent event) {
                if (event.getAction() == android.view.MotionEvent.ACTION_DOWN) {
                    hideGameKeyboard();
                }
                return true;
            }
        });
        // VISIBLE_PASSWORD used to sit here: it disables composing in Gboard, so
        // every Latin letter is committed at once, but it also switches on the
        // password layout with a number row the user does not get elsewhere. The
        // text filter gives the same direct committing without composing and keeps
        // the ordinary layout.
        chatEditor.setInputType(InputType.TYPE_CLASS_TEXT
                | InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS
                | InputType.TYPE_TEXT_VARIATION_FILTER);
        chatEditor.setImeOptions(EditorInfo.IME_ACTION_DONE
                | EditorInfo.IME_FLAG_NO_EXTRACT_UI
                | EditorInfo.IME_FLAG_NO_FULLSCREEN);
        chatEditor.setFilters(new InputFilter[] { new InputFilter.LengthFilter(95) });
        chatEditor.setVisibility(View.INVISIBLE);

        /* A full-width strip at the top: a tiny 1x1 field or one in a corner reads
         * as dead and receives nothing. Touches still reach the native InputQueue. */
        float density = getResources().getDisplayMetrics().density;
        int editorH = Math.max(48, (int) (48f * density));
        FrameLayout.LayoutParams params = new FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.MATCH_PARENT, editorH, Gravity.TOP);
        addContentView(chatEditor, params);

        chatEditor.addTextChangedListener(new TextWatcher() {
            @Override public void beforeTextChanged(CharSequence s, int start, int count, int after) {
                // A real deletion from the keyboard: the text got shorter rather
                // than replaced wholesale by a sync from the native side.
                pendingDelete = !syncingFromNative && count > 0 && after == 0;
            }
            @Override public void onTextChanged(CharSequence s, int start, int before, int count) { }
            @Override public void afterTextChanged(Editable value) {
                if (syncingFromNative) return;
                replaceTextNative(value.toString());
                if (pendingDelete) {
                    long now = android.os.SystemClock.elapsedRealtime();
                    deleteStreak = (now - lastDeleteAt <= 200) ? deleteStreak + 1 : 1;
                    lastDeleteAt = now;
                    pendingDelete = false;
                    /* A held Backspace: after six deletions in a row, no slower
                     * than 200 ms apart, the rest is cleared at once. Ordinary quick
                     * taps remove one character and never reach the threshold. */
                    if (deleteStreak >= 6 && chatEditor.length() > 0) {
                        deleteStreak = 0;
                        chatEditor.post(new Runnable() {
                            @Override public void run() {
                                if (chatEditor.length() > 0) chatEditor.setText("");
                            }
                        });
                    }
                } else {
                    deleteStreak = 0;
                }
            }
        });
        chatEditor.setOnFocusChangeListener(new View.OnFocusChangeListener() {
            @Override
            public void onFocusChange(View view, boolean focused) {
                if (focused) {
                    editorActive = nativeReady && wantKeyboard
                            && chatEditor.getVisibility() == View.VISIBLE;
                    return;
                }
                /* Native surface often steals focus after adjustResize.
                 * If the game still wants the keyboard, take focus back
                 * instead of marking the editor dead — otherwise the IME
                 * stays up and typed characters never reach the field. */
                if (wantKeyboard && chatEditor.getVisibility() == View.VISIBLE) {
                    chatEditor.post(new Runnable() {
                        @Override public void run() { claimEditorFocus(); }
                    });
                } else {
                    editorActive = false;
                }
            }
        });
        chatEditor.setOnEditorActionListener(new TextView.OnEditorActionListener() {
            @Override
            public boolean onEditorAction(TextView view, int actionId, KeyEvent event) {
                boolean enter = actionId == EditorInfo.IME_ACTION_SEND
                        || actionId == EditorInfo.IME_ACTION_DONE
                        || (event != null && event.getKeyCode() == KeyEvent.KEYCODE_ENTER
                            && event.getAction() == KeyEvent.ACTION_DOWN);
                if (enter) {
                    submitTextNative();
                    return true;
                }
                return false;
            }
        });

        // Android does not send a direct callback when the user dismisses an IME
        // with the system Back gesture. Track an actual visible->hidden transition;
        // importantly, do not report "hidden" during the short show request delay.
        // When the keyboard is swiped away the game is told ALWAYS, even if the
        // field still wants input, because otherwise the native visibility flag got
        // stuck on "open": a second tap on the field thought the IME was already up,
        // never reopened it, and input went nowhere, which is what broke the nick
        // field.
        chatEditor.getRootView().getViewTreeObserver().addOnGlobalLayoutListener(
                new ViewTreeObserver.OnGlobalLayoutListener() {
                    @Override
                    public void onGlobalLayout() {
                        View root = chatEditor.getRootView();
                        Rect visible = new Rect();
                        root.getWindowVisibleDisplayFrame(visible);
                        boolean keyboardVisible = root.getHeight() - visible.bottom
                                > root.getHeight() * 0.15f;
                        imeLooksVisible = keyboardVisible;
                        if (wantKeyboard) claimEditorFocus();
                        if (keyboardVisible) {
                            keyboardWasVisible = true;
                        } else if (keyboardWasVisible) {
                            keyboardWasVisible = false;
                            keyboardHiddenNative();
                        }
                    }
                });
    }

    /**
     * The "the game is in alpha" window. Called from native code (the game asks
     * for it a moment after the epilepsy warning is accepted, not at startup,
     * so the dialog does not cover the warning). A plain AlertDialog: one OK
     * button, no cancel outside it, and the immersive flags are set again when
     * it closes, because the system bars come back with the dialog.
     */
    public void showAlphaNotice() {
        runOnUiThread(new Runnable() {
            @Override public void run() { openAlphaNotice(); }
        });
    }

    private void openAlphaNotice() {
        if (alphaNoticeShown || isFinishing()) return;
        alphaNoticeShown = true;
        new AlertDialog.Builder(this)
                .setTitle("Cubic Battle 4 — альфа")
                .setMessage("Игра находится в альфа-версии.\n\n"
                        + "Здесь есть баги, часть контента ещё не готова, а баланс и "
                        + "прогресс могут меняться между обновлениями. Спасибо, что "
                        + "играете и помогаете её делать лучше!")
                .setCancelable(false)
                .setPositiveButton("ОК", new DialogInterface.OnClickListener() {
                    @Override
                    public void onClick(DialogInterface dialog, int which) {
                        enterImmersiveMode();
                    }
                })
                .show();
    }

    private void claimEditorFocus() {
        if (chatEditor == null || !wantKeyboard) return;
        if (chatEditor.getVisibility() != View.VISIBLE) chatEditor.setVisibility(View.VISIBLE);
        if (!chatEditor.hasFocus()) chatEditor.requestFocus();
        editorActive = nativeReady;
    }

    /** Called from native code. It is safe to call from the native game thread. */
    public void showGameKeyboard(final String currentText) {
        runOnUiThread(new Runnable() {
            @Override
            public void run() {
                if (chatEditor == null) return;
                wantKeyboard = true;
                chatEditor.setVisibility(View.VISIBLE);
                chatEditor.bringToFront();
                replaceEditorText(currentText);
                claimEditorFocus();
                getWindow().setSoftInputMode(
                        WindowManager.LayoutParams.SOFT_INPUT_STATE_ALWAYS_VISIBLE
                                | WindowManager.LayoutParams.SOFT_INPUT_ADJUST_RESIZE);
                if (imeLooksVisible) {
                    /* The IME is already on screen: keep the focus, restart nothing. */
                    claimEditorFocus();
                    return;
                }
                showAttempts = 0;
                requestShowWhenReady();
            }
        });
    }

    /* showSoftInput quietly returns false until the editor becomes the target of
     * the IME, since focus and the input connection settle only on the next layout
     * pass after setVisibility(VISIBLE) and requestFocus. The first request is
     * therefore delayed, and while the keyboard has not really appeared, judged by
     * the shrunken screen in onGlobalLayout, the request repeats. */
    private void requestShowWhenReady() {
        if (chatEditor == null) return;
        final int attempt = showAttempts++;
        if (attempt >= 10) return;
        chatEditor.postDelayed(new Runnable() {
            @Override
            public void run() {
                if (chatEditor == null || !wantKeyboard || imeLooksVisible) return;
                claimEditorFocus();
                InputMethodManager input = (InputMethodManager)
                        getSystemService(Context.INPUT_METHOD_SERVICE);
                if (input != null) {
                    if (!input.isActive(chatEditor)) chatEditor.requestFocus();
                    input.showSoftInput(chatEditor, InputMethodManager.SHOW_FORCED);
                }
                requestShowWhenReady();
            }
        }, attempt == 0 ? 60 : 120);
    }

    /** Keeps the hidden editor in sync after the native Send button clears it. */
    public void setGameKeyboardText(final String text) {
        runOnUiThread(new Runnable() {
            @Override
            public void run() {
                replaceEditorText(text);
                if (wantKeyboard) claimEditorFocus();
            }
        });
    }

    /**
     * Called from the native game thread: true when this editor owns the text,
     * so the native side must not append key events into its own buffer.
     */
    public boolean gameKeyboardActive() {
        return wantKeyboard;
    }

    /** Called from native code when chat is closed or the online game is left. */
    public void hideGameKeyboard() {
        runOnUiThread(new Runnable() {
            @Override
            public void run() {
                if (chatEditor == null) return;
                wantKeyboard = false;
                InputMethodManager input = (InputMethodManager)
                        getSystemService(Context.INPUT_METHOD_SERVICE);
                if (input != null) {
                    input.hideSoftInputFromWindow(chatEditor.getWindowToken(), 0);
                }
                editorActive = false;
                imeLooksVisible = false;
                showAttempts = 10; /* stop the pending show retries */
                getWindow().setSoftInputMode(
                        WindowManager.LayoutParams.SOFT_INPUT_ADJUST_RESIZE);
                chatEditor.clearFocus();
                chatEditor.setVisibility(View.INVISIBLE);
                /* The text lives in the native buffer, which closing the IME must
                 * not wipe: the game clears it itself after sending or leaving the
                 * screen. */
                keyboardHiddenNative();
            }
        });
    }

    private void replaceEditorText(String text) {
        if (chatEditor == null) return;
        String safe = text == null ? "" : text;
        if (safe.contentEquals(chatEditor.getText())) return;
        boolean shrinking = safe.length() < chatEditor.length();
        syncingFromNative = true;
        chatEditor.setText(safe);
        chatEditor.setSelection(chatEditor.length());
        syncingFromNative = false;
        // restartInput only on a deletion or a clear: otherwise the IME drops the
        // Latin letter just typed and the nick field stays empty.
        if (shrinking) {
            InputMethodManager input = (InputMethodManager)
                    getSystemService(Context.INPUT_METHOD_SERVICE);
            if (input != null) input.restartInput(chatEditor);
        }
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        /* Back from the launcher or the recents screen the system bars are shown
         * again; without hiding them the window shrinks and resizes once more
         * right after the return. */
        if (hasFocus) enterImmersiveMode();
        if (hasFocus && wantKeyboard && chatEditor != null) {
            claimEditorFocus();
            if (!imeLooksVisible) {
                showAttempts = 0;
                requestShowWhenReady();
            }
        }
    }

    @Override
    protected void onSaveInstanceState(Bundle out) {
        super.onSaveInstanceState(out);
        out.putBoolean("alphaNoticeShown", alphaNoticeShown);
    }

    @Override
    protected void onResume() {
        super.onResume();
        enterImmersiveMode();
    }

    @Override
    protected void onPause() {
        hideGameKeyboard();
        super.onPause();
    }
}
