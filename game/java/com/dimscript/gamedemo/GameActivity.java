/*
 * ВАЖНО: пакет — com.cb4, он же в AndroidManifest.xml и в именах JNI-функций
 * runtime.c (Java_com_cb4_GameActivity_*). Все три места обязаны совпадать:
 * если хотя бы одно расходится, System.loadLibrary отработает, но нативные
 * методы редактора (nativeReplaceText/nativeSubmitText/nativeKeyboardHidden)
 * не найдутся, nativeReady станет false — клавиатура откроется, а текст в
 * поле ввода не попадёт.
 *
 * Файл намеренно лежит в старом каталоге com/dimscript/gamedemo: путь зашит
 * в шаг сборки .github/workflows/main.yml. Для javac это неважно — класс
 * компилируется по объявленному пакету (classes/com/cb4/GameActivity.class).
 */
package com.cb4;

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
    /* Game asked for the IME. Stays true across transient focus losses. */
    private volatile boolean wantKeyboard;
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
        enterImmersiveMode();

        chatEditor = new EditText(this);
        chatEditor.setSingleLine(true);
        // Прозрачный текст, но alpha=1: при alpha=0 Gboard/системная IME
        // считают поле мёртвым и не отдают символы (даже латиницу).
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
        /* Пока редактор видим, он лежит поверх маленькой полосы native-surface.
         * Тап по этой полосе тоже считается тапом вне игрового поля и закрывает
         * IME, а не возвращает ей фокус. */
        chatEditor.setOnTouchListener(new View.OnTouchListener() {
            @Override
            public boolean onTouch(View view, android.view.MotionEvent event) {
                if (event.getAction() == android.view.MotionEvent.ACTION_DOWN) {
                    hideGameKeyboard();
                }
                return true;
            }
        });
        // VISIBLE_PASSWORD отключает composing у Gboard: каждая латинская буква
        // сразу коммитится в поле. Без этого IME держит «a» как composing, а
        // restartInput/setText с нативной стороны его съедает — клавиатура
        // открыта, буквы нажимаются, в поле Ник ничего не появляется.
        chatEditor.setInputType(InputType.TYPE_CLASS_TEXT
                | InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS
                | InputType.TYPE_TEXT_VARIATION_VISIBLE_PASSWORD);
        chatEditor.setImeOptions(EditorInfo.IME_ACTION_DONE
                | EditorInfo.IME_FLAG_NO_EXTRACT_UI
                | EditorInfo.IME_FLAG_NO_FULLSCREEN);
        chatEditor.setFilters(new InputFilter[] { new InputFilter.LengthFilter(95) });
        chatEditor.setVisibility(View.INVISIBLE);

        /* Полноширинная полоса сверху: крошечный 1×1/угол IME считает мёртвым
         * и не отдаёт символы. Касания всё равно идут в native InputQueue. */
        float density = getResources().getDisplayMetrics().density;
        int editorH = Math.max(48, (int) (48f * density));
        FrameLayout.LayoutParams params = new FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.MATCH_PARENT, editorH, Gravity.TOP);
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
        // Когда клавиатуру смахнули, сообщаем игре ВСЕГДА, даже если поле ещё
        // «хочет» ввод (wantKeyboard=true). Иначе нативный флаг видимости
        // навсегда застревал в «открыто»: повторный тап по полю считал IME уже
        // показанной и не переоткрывал её, а ввод шёл в никуда — поле выглядело
        // мёртвым (именно это ломало ввод ника).
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
                    /* IME уже на экране: не перезапускаем её, просто держим фокус. */
                    claimEditorFocus();
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
                showAttempts = 10; /* остановить незавершённые повторы показа */
                getWindow().setSoftInputMode(
                        WindowManager.LayoutParams.SOFT_INPUT_ADJUST_RESIZE);
                chatEditor.clearFocus();
                chatEditor.setVisibility(View.INVISIBLE);
                /* Текст хранится в native-буфере: закрытие IME не должно
                 * стирать введённый ник/пароль/сообщение. Игра очищает его
                 * сама после отправки или выхода из экрана. */
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
        // restartInput только при удалении/очистке: иначе IME сбрасывает
        // только что введённую латинскую букву, и поле Ник остаётся пустым.
        if (shrinking) {
            InputMethodManager input = (InputMethodManager)
                    getSystemService(Context.INPUT_METHOD_SERVICE);
            if (input != null) input.restartInput(chatEditor);
        }
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus && wantKeyboard && chatEditor != null) {
            claimEditorFocus();
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
