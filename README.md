# Gig1.0 / DimScript — Android 10 only, без звёзд

## Что исправлено по оценке 6/10

**Слабые стороны были:**
- Ограниченный типаж (нет массивов, словарей, bool, enum)
- Минимальная stdlib (нет файлов, JSON, сети, таймеров)
- Синтаксис без преимуществ
- Привязка к одному движку (и кроссплатформенность была лишней)

**Исправления:**

### 1. Типы
- `bool` с `true`/`false` → `int` 1/0
- `arr` — динамический массив `num`: `arr_new()`, `arr_push()`, `arr_get()`, `arr_set()`, `arr_len()`, `arr_pop()`, `arr_clear()`, `arr_free()`
- `dict` — словарь `str -> num`: `dict_new()`, `dict_set()`, `dict_get()`, `dict_has()`, `dict_del()`, `dict_free()`
- `timer` — таймер: `timer_new()`, `timer_start()`, `timer_elapsed()`, `timer_reset()`, `timer_free()`
- `enum` — `enum State Lobby=0, Game, Menu end` создаёт константы

### 2. Стандартная библиотека
- Файлы: `file_read()`, `file_write()`, `file_exists()`, `file_del()`
- JSON: `json_get_str()`, `json_get_num()`, `json_get_bool()`
- Сеть: `http_get()`, `http_post()` + уже существующие `net_connect()` для Firebase
- Утилиты: `clamp()`, `lerp()`, `dist()`, `now()`, `ds_log()`
- Онлайн — одна общая комната `main` в Firebase Realtime Database, до 4 игроков без экрана ожидания (см. `FIREBASE.md`)

### 3. Синтаксис проще и компактнее
- `;` — несколько операторов в строке: `x=0; y=0; hp=10`
- `,` — несколько переменных: `num x=0, y=0, size=30`
- Объекты в 3 строки: `object Player` / `num x=0, y=0...` / `end`
- `then` / `do` и `:` в конце `if`/`loop` — более читаемо
- `c` — inline C: `c printf("hi\n");` / `c ds_log("fire");`
- Код игры сжат с 853 до 186 строк (-78%), 6 файлов без цифр

### 4. Платформа — теперь только Android 10
По запросу убрана кроссплатформенность:
- Только `arm64-v8a` + `armeabi-v7a`
- Только Android 10 API 29 (`minSdkVersion 29`, `targetSdkVersion 29`)
- `runtime.h`, `graphics.c`, `main.c`, `net.c` — только Android, без desktop fallback и без `#ifdef __ANDROID__` веток
- Звёзды полностью удалены: нет `init_stars`, `update_stars`, `draw_stars` ни в рантайме, ни в компиляторе

## Структура

```
game/
├── config.ds    2 строки — все num/str через запятую
├── entities.ds  14 — игровые объекты и сетевые массивы
├── ui.ds        17 — кнопки, зона «Назад» и полосы HP
├── menu.ds      23 — лобби, режимы и настройки
├── battle.ds    109 — соло + синхронизированный онлайн Firebase
├── engine.ds    21 — переходы без вспышек и игровые хуки
```

## Сборка

```sh
python3 gen.py
```

NDK (Android 10):
```sh
aarch64-linux-android29-clang -O3 -shared game.c runtime.c main.c glue -landroid -llog -lm -o lib/arm64-v8a/libds_game.so
armv7a-linux-androideabi29-clang ... -o lib/armeabi-v7a/libds_game.so
aapt package -M AndroidManifest.xml -I android-29/android.jar -F apk
```

APK только для Android 10 arm64/arm32, без звёзд, без кроссплатформы.
