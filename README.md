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

### 5. Онлайн: меньше лага и ближний бой вместо пуль
- **Меньше лага:** `net.c` больше не делает последовательные HTTP-запросы на одном
  потоке. Запись (`PATCH` своего состояния) и чтение (`GET` комнаты) идут на двух
  параллельных потоках с интервалом 50 мс — обновления соперников приходят часто
  и не блокируют друг друга.
- **Удар вместо пули:** пуля не вылетает. Пока игрок держит кнопку атаки —
  показывается тёмно-серый прицел-хитбокс, а сам удар происходит в момент, когда
  игрок отпускает кнопку (перестал целиться). Спрайт игрока на ~0.2 с меняется с
  `ordinary.png` на `ordinary_punch.png`, и весь этот кадр хитбокс ближнего боя
  активен: широкий (`punch_width` = 90px) и недлинный (`punch_reach` = 70px).
  Враг теперь того же размера, что и игрок, с простым квадратным хитбоксом.
  Событие удара публикуется в Firebase как `(x, y, dx, active, shot)` — соперники
  показывают спрайт удара и получают урон, если стоят в хитбоксе.
- **Чат:** сообщения — слева сверху, кнопка чата — чуть ниже их, в том же стиле,
  что и остальные кнопки (`0x5F10A0`, радиус 20). Enter отправляет сообщение.
- **Консоль:** в настройках показывает все логи игры. Фон идеально чёрный,
  обычные логи — зелёным, ошибки (включая «текстура не загрузилась») — красным.

## Структура

```
game/
├── config.ds    — все num/str через запятую (параметры удара: punch_*)
├── entities.ds  — игровые объекты (Player/Enemy/Punch) и сетевые массивы
├── ui.ds        — кнопки, зона «Назад» и полосы HP
├── menu.ds      — лобби, режимы, настройки и консоль (чёрная, зелёный/красный)
├── chat.ds      — онлайн-чат: сообщения слева сверху, кнопка ниже в стиле кнопок
├── battle.ds    — соло + синхронизированный онлайн Firebase (прицел и удар)
├── engine.ds    — переходы без вспышек, игровые хуки и логи загрузки текстур
```

## Сборка

### 1. Генерация C-кода из DimScript
```sh
python3 gen.py
```

### 2. Сборка для ПК Windows (.exe)

**Через bat-скрипт (Windows):**
```cmd
build_pc.bat
```

**Через MinGW GCC (вручную):**
```sh
gcc -O3 -Wall -Wextra -I. -Igame game/game.c runtime.c main_win32.c -lgdi32 -lwininet -luser32 -lkernel32 -lm -o Game.exe -mwindows
```

**Через MSVC (cl.exe):**
```cmd
cl /O2 /W3 /I. /Igame game/game.c runtime.c main_win32.c /Fe:Game.exe /link gdi32.lib wininet.lib user32.lib kernel32.lib shell32.lib /SUBSYSTEM:WINDOWS
```

**Управление на ПК:**
- `W`, `A`, `S`, `D` / Стрелочки — движение персонажа
- `Space` (Пробел), `J`, `F` — прицеливание и удар (удар наносится при отпускании)
- Мышь (ЛКМ) — нажатие на кнопки меню, управление джойстиком
- `Enter` / Клавиатура — набор текста и отправка сообщений в онлайн-чате
- `Esc` — кнопка «Назад»

### 3. Сборка для Android 10 NDK (APK)

```sh
aarch64-linux-android29-clang -O3 -shared game/game.c runtime.c main.c $GLUE/android_native_app_glue.c -I. -I$GLUE -landroid -llog -lm -o staging/lib/arm64-v8a/libds_game.so
armv7a-linux-androideabi29-clang -O3 -shared game/game.c runtime.c main.c $GLUE/android_native_app_glue.c -I. -I$GLUE -landroid -llog -lm -o staging/lib/armeabi-v7a/libds_game.so
aapt package -f -M game/AndroidManifest.xml -I android-34/android.jar -F unsigned.apk ./staging/
```
