# Gig1.0 / DimScript

Минимальный Android-движок и компилятор DimScript в C.

## Структура игры

```
game/
├── game.ds                 # точка входа, переходы между сценами и фон игры
├── lobby.ds                # фон лобби
├── ui.ds                   # весь интерфейс: HUD, кнопки, джойстик и прицел
├── logic.ds                # игровая логика: враг, пули, столкновения и победа
├── cubes.ds                # куб игрока, его текстура и параметры
├── assets/
│   ├── fonts/ChillRoundGothic_Heavy.ttf
│   └── player.png
└── AndroidManifest.xml
```

Генерация `game/game.c`:

```sh
python3 gen.py            # по умолчанию из ./game
python3 gen.py --dump     # показать сгенерированный C-код
```

## Многофайловые игры

Поддержка `#include "file.ds"` (путь относительно файла, где записан include). Include может быть вложенным, каждый файл подключается один раз, цикл — ошибка. Старый режим тоже работает: при передаче папки все верхнеуровневые `*.ds` компилируются вместе.

## Строки, циклы и объекты

`+` компилируется в реальную конкатенацию. Переменные — типизированные C-поля, без хэш-таблицы.

```text
str title = "Score: " + floor(score)
for (num i = 0; i <= 10; i += 1) {
    ds_log("i: " + i)
}

object Player {
    num x = 0
    str name = "player"
    fn init(num start) { self.x = start }
    fn move(num dx) { self.x += dx }
}

Player player = new Player(100)
```

Компилятор генерирует `struct Player`, `ds_new_Player`, `ds_free_Player` и прямые вызовы методов. Поля объекта не хранятся в хэш-таблице.

## Время и размеры экрана

Хост каждый кадр обновляет `screen_w`, `screen_h` и `dt` — длительность прошлого кадра в секундах (ограничена 0.1 с). Анимации и таймеры считайте через `dt`, а не через счётчики кадров: скорость игрового цикла не фиксирована, иначе при высоком FPS эффекты превращаются в мигание.

## Software Renderer и TTF

Окно Android лочится один раз на кадр, скрипт собирает команды, `graphics.c` растеризует их в правильном порядке. `rect`/`circle`/`ring` и полупрозрачная толстая `line` — быстрые span-заливки; непрозрачные PNG в масштабе 1:1 копируются через `memcpy`. Никаких OpenGL/EGL. Для линии используйте `line(x1, y1, x2, y2, thickness, color)`; альфа-канал цвета смешивается с фоном.

Шрифт по умолчанию `assets/fonts/ChillRoundGothic_Heavy.ttf`. TTF разбирается и растеризуется в сглаженный атлас один раз при первом `text`; кадры используют готовые glyph-ы и батч команд.

## PNG

Положите файл в `game/assets`, имя в скрипте — относительно этой папки:

```text
str PLAYER = "player.png"

fn init() { png_load(PLAYER) }  // необязательная предзагрузка
fn draw() { tex(100, 80, PLAYER, 0, 1) }
```

`tex(x, y, name, angle, scale)` — загружает/кэширует, поддерживает прозрачность, масштаб, поворот. Подпапки: `"sprites/enemy.png"`.

Перед `aapt` ресурсы нужно скопировать:

```sh
python3 stage_assets.py game/assets staging/assets
```

Без этого `png_load` и `text` не найдут ресурсы в APK.

## Ошибки и перезапуск

Каждый хук `init`/`update`/`draw`/`touch` запускается через `ds_call_protected`. `ds_runtime_error` сохраняет сообщение и делает контролируемый переход к границе вызова. Хост показывает безопасный экран ошибки и через секунду вызывает `reset`, затем `init`. UI может вызвать `ds_request_script_restart()` для ручного рестарта.

## Сборка

Воркфлоу `.github/workflows/main.yml` перед `aapt` копирует ресурсы в `staging/assets`, NDK собирает `game/game.c`, `runtime.c`, `main.c` (graphics.c встроен в main.c через `#include`). `libEGL` и `libGLESv2` не нужны.
