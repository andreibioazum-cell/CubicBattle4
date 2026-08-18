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
    /*
     * NativeActivity loads the game .so with dlopen(), which does NOT register
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

    private EditText chatEditor;
    private boolean syncingFromNative;
    private boolean keyboardWasVisible;
    /* Читается из игрового потока: пока true, весь текст ведёт этот редактор. */
    private volatile boolean editorActive;
    /* Видна ли IME прямо сейчас (по реальному размеру экрана в onGlobalLayout). */
    private volatile boolean imeLooksVisible;
    private int showAttempts;

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

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        getWindow().setSoftInputMode(WindowManager.LayoutParams.SOFT_INPUT_ADJUST_RESIZE);
        getWindow().clearFlags(WindowManager.LayoutParams.FLAG_FULLSCREEN);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_FORCE_NOT_FULLSCREEN);

        chatEditor = new EditText(this);
        chatEditor.setSingleLine(true);
        chatEditor.setTextColor(Color.TRANSPARENT);
        chatEditor.setHintTextColor(Color.TRANSPARENT);
        chatEditor.setBackgroundColor(Color.TRANSPARENT);
        chatEditor.setCursorVisible(false);
        chatEditor.setAlpha(0.02f);
        chatEditor.setGravity(Gravity.BOTTOM | Gravity.START);
        chatEditor.setFocusable(true);
        chatEditor.setFocusableInTouchMode(true);
        // Никакого автодополнения и автозамены: IME больше не восстанавливает
        // только что стёртые символы и не склеивает их с новым вводом.
        chatEditor.setInputType(InputType.TYPE_CLASS_TEXT
                | InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS
                | InputType.TYPE_TEXT_VARIATION_VISIBLE_PASSWORD);
        chatEditor.setImeOptions(EditorInfo.IME_ACTION_DONE
                | EditorInfo.IME_FLAG_NO_EXTRACT_UI
                | EditorInfo.IME_FLAG_NO_FULLSCREEN);
        chatEditor.setFilters(new InputFilter[] { new InputFilter.LengthFilter(95) });
        chatEditor.setVisibility(View.INVISIBLE);

        FrameLayout.LayoutParams params = new FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.MATCH_PARENT, 48,
                Gravity.BOTTOM);
        addContentView(chatEditor, params);

        chatEditor.addTextChangedListener(new TextWatcher() {
            @Override public void beforeTextChanged(CharSequence s, int start, int count, int after) { }
            @Override public void onTextChanged(CharSequence s, int start, int before, int count) { }
            @Override public void afterTextChanged(Editable value) {
                if (!syncingFromNative) replaceTextNative(value.toString());
            }
        });
        chatEditor.setOnFocusChangeListener(new View.OnFocusChangeListener() {
            @Override
            public void onFocusChange(View view, boolean focused) {
                editorActive = nativeReady && focused
                        && chatEditor.getVisibility() == View.VISIBLE;
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
                        if (keyboardVisible) {
                            keyboardWasVisible = true;
                        } else if (keyboardWasVisible) {
                            keyboardWasVisible = false;
                            keyboardHiddenNative();
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
                chatEditor.bringToFront();
                replaceEditorText(currentText);
                chatEditor.requestFocus();
                editorActive = true;
                getWindow().setSoftInputMode(
                        WindowManager.LayoutParams.SOFT_INPUT_STATE_ALWAYS_VISIBLE
                                | WindowManager.LayoutParams.SOFT_INPUT_ADJUST_RESIZE);
                if (imeLooksVisible) {
                    /* IME уже на экране: не перезапускаем её, просто держим фокус. */
                    if (!chatEditor.hasFocus()) chatEditor.requestFocus();
                    return;
                }
                showAttempts = 0;
                requestShowWhenReady();
            }
        });
    }

    /* showSoftInput молча возвращает false, пока редактор не стал целью IME:
     * фокус и input-подключение привязываются только на следующем
     * layout-проходе после setVisibility(VISIBLE)/requestFocus. Поэтому первый
     * запрос отложен, и пока клавиатура реально не появилась (onGlobalLayout
     * видит уменьшившийся экран), запрос повторяется. */
    private void requestShowWhenReady() {
        if (chatEditor == null) return;
        final int attempt = showAttempts++;
        if (attempt >= 10) return;
        chatEditor.postDelayed(new Runnable() {
            @Override
            public void run() {
                if (chatEditor == null || !editorActive || imeLooksVisible) return;
                if (!chatEditor.hasFocus()) chatEditor.requestFocus();
                InputMethodManager input = (InputMethodManager)
                        getSystemService(Context.INPUT_METHOD_SERVICE);
                if (input != null && input.isActive(chatEditor)) {
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
            }
        });
    }

    /**
     * Called from the native game thread: true when this editor owns the text,
     * so the native side must not append key events into its own buffer.
     */
    public boolean gameKeyboardActive() {
        return editorActive;
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
                editorActive = false;
                imeLooksVisible = false;
                showAttempts = 10; /* остановить незавершённые повторы показа */
                getWindow().setSoftInputMode(
                        WindowManager.LayoutParams.SOFT_INPUT_ADJUST_RESIZE);
                chatEditor.clearFocus();
                chatEditor.setVisibility(View.INVISIBLE);
                clearEditorText();
                keyboardHiddenNative();
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
        // Сбрасываем composing-регион IME: без этого клавиатура держит у себя
        // уже удалённые буквы и приклеивает их к следующему слову.
        InputMethodManager input = (InputMethodManager)
                getSystemService(Context.INPUT_METHOD_SERVICE);
        if (input != null) input.restartInput(chatEditor);
    }

    /** Полная очистка поля, чтобы закрытая клавиатура не хранила старый текст. */
    private void clearEditorText() {
        replaceEditorText("");
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus && editorActive && chatEditor != null) {
            chatEditor.requestFocus();
            if (!imeLooksVisible) {
                showAttempts = 0;
                requestShowWhenReady();
            }
        }
    }

    @Override
    protected void onPause() {
        hideGameKeyboard();
        super.onPause();
    }
}
