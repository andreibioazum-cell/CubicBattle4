package com.dimscript.gamedemo;

import android.app.NativeActivity;
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
 * input connection for the in-game chat. The game continues to draw the field
 * itself; this one-pixel editor makes every soft IME deliver commitText events.
 */
public final class GameActivity extends NativeActivity {
    private EditText chatEditor;
    private boolean syncingFromNative;
    private boolean keyboardWasVisible;

    private native void nativeReplaceText(String text);
    private native void nativeSubmitText();
    private native void nativeKeyboardHidden();

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        getWindow().setSoftInputMode(WindowManager.LayoutParams.SOFT_INPUT_ADJUST_RESIZE);

        chatEditor = new EditText(this);
        chatEditor.setSingleLine(true);
        chatEditor.setTextColor(Color.TRANSPARENT);
        chatEditor.setHintTextColor(Color.TRANSPARENT);
        chatEditor.setBackgroundColor(Color.TRANSPARENT);
        chatEditor.setCursorVisible(false);
        chatEditor.setAlpha(0.01f);
        chatEditor.setGravity(Gravity.BOTTOM | Gravity.START);
        chatEditor.setInputType(InputType.TYPE_CLASS_TEXT
                | InputType.TYPE_TEXT_FLAG_CAP_SENTENCES
                | InputType.TYPE_TEXT_FLAG_AUTO_CORRECT);
        chatEditor.setImeOptions(EditorInfo.IME_ACTION_SEND
                | EditorInfo.IME_FLAG_NO_EXTRACT_UI);
        chatEditor.setFilters(new InputFilter[] { new InputFilter.LengthFilter(95) });
        chatEditor.setVisibility(View.INVISIBLE);

        FrameLayout.LayoutParams params = new FrameLayout.LayoutParams(2, 2,
                Gravity.BOTTOM | Gravity.START);
        addContentView(chatEditor, params);

        chatEditor.addTextChangedListener(new TextWatcher() {
            @Override public void beforeTextChanged(CharSequence s, int start, int count, int after) { }
            @Override public void onTextChanged(CharSequence s, int start, int before, int count) { }
            @Override public void afterTextChanged(Editable value) {
                if (!syncingFromNative) nativeReplaceText(value.toString());
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
                    nativeSubmitText();
                    return true;
                }
                return false;
            }
        });

        // Android does not send a direct callback when the user dismisses an IME
        // with the system Back gesture. Track an actual visible->hidden transition;
        // importantly, do not report "hidden" during the short show request delay.
        chatEditor.getRootView().getViewTreeObserver().addOnGlobalLayoutListener(
                new ViewTreeObserver.OnGlobalLayoutListener() {
                    @Override
                    public void onGlobalLayout() {
                        View root = chatEditor.getRootView();
                        Rect visible = new Rect();
                        root.getWindowVisibleDisplayFrame(visible);
                        boolean keyboardVisible = root.getHeight() - visible.bottom
                                > root.getHeight() * 0.15f;
                        if (keyboardVisible) {
                            keyboardWasVisible = true;
                        } else if (keyboardWasVisible) {
                            keyboardWasVisible = false;
                            nativeKeyboardHidden();
                        }
                    }
                });
    }

    /** Called from native code. It is safe to call from the native game thread. */
    public void showGameKeyboard(final String currentText) {
        runOnUiThread(new Runnable() {
            @Override
            public void run() {
                if (chatEditor == null) return;
                chatEditor.setVisibility(View.VISIBLE);
                replaceEditorText(currentText);
                chatEditor.requestFocus();
                chatEditor.postDelayed(new Runnable() {
                    @Override
                    public void run() {
                        InputMethodManager input = (InputMethodManager)
                                getSystemService(Context.INPUT_METHOD_SERVICE);
                        if (input != null) {
                            input.showSoftInput(chatEditor, InputMethodManager.SHOW_IMPLICIT);
                        }
                    }
                }, 80);
            }
        });
    }

    /** Keeps the hidden editor in sync after the native Send button clears it. */
    public void setGameKeyboardText(final String text) {
        runOnUiThread(new Runnable() {
            @Override
            public void run() {
                replaceEditorText(text);
            }
        });
    }

    /** Called from native code when chat is closed or the online game is left. */
    public void hideGameKeyboard() {
        runOnUiThread(new Runnable() {
            @Override
            public void run() {
                if (chatEditor == null) return;
                InputMethodManager input = (InputMethodManager)
                        getSystemService(Context.INPUT_METHOD_SERVICE);
                if (input != null) {
                    input.hideSoftInputFromWindow(chatEditor.getWindowToken(), 0);
                }
                chatEditor.clearFocus();
                chatEditor.setVisibility(View.INVISIBLE);
                nativeKeyboardHidden();
            }
        });
    }

    private void replaceEditorText(String text) {
        if (chatEditor == null) return;
        String safe = text == null ? "" : text;
        if (safe.contentEquals(chatEditor.getText())) return;
        syncingFromNative = true;
        chatEditor.setText(safe);
        chatEditor.setSelection(chatEditor.length());
        syncingFromNative = false;
    }

    @Override
    protected void onPause() {
        hideGameKeyboard();
        super.onPause();
    }
}
