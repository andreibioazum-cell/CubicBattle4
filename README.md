# Gig1.0 / DimScript

Минимальный Android-движок и компилятор DimScript в C.

## Структура игры

```
game/
├── game.ds                 # весь скрипт: лобби, интерфейс, логика боя
├── assets/
│   ├── fonts/ChillRoundGothic_Heavy.ttf
│   ├── grass.png
│   └── player.png
└── AndroidManifest.xml
```

Генерация `game/game.c`:

```sh
python3 gen.py            # по умолчанию из ./game
python3 gen.py --dump     # показать сгенерированный C-код
```

## Язык

Три типа — `num`, `str`, `col` — и ничего лишнего: вызовы без скобок,
блоки закрываются словом `end`.

```text
str TEX = "player.png"

object Player              // структура с полями
    num x = 0
    num y = 0
    num size = 30
    col color = 0xFF8844
    num angle = 0
end

Player player = new Player()
player.x = 100

function move_player num dx, num dy
    player.x = player.x + dx
    player.y = player.y + dy
end

function draw_player_cube
    circle player.x, player.y, player.size / 2, player.color
    tex player.x - player.size, player.y - player.size, TEX, player.angle, 1
end

move_player 10, 0
draw_player_cube
```

- переменные: `num x = 0`, `str name = "text"`, `col c = 0xFF8844`
- объекты: `object Name` … `end` (только поля), создание `Name v = new Name()`
- функции: `function name тип параметр, …` … `end`; вызов `name a, b`
- ветки и циклы: `if условие` / `else if` / `else` / `loop условие` … `end`
- `return` — выход из функции
- в выражениях вызовы пишутся со скобками: `sqrt(x*x + y*y)`, `atan2(y, x)`,
  `floor(a * 255)`; строки склеиваются через `+`: `"HP: " + enemy.hp`
- комментарии — `//`

Встроенные функции: `rect`, `roundrect`, `circle`, `ring`, `line`, `tex`,
`text`, `text_scaled`, `text_ink_width`, `text_ink_height`, `png_load`,
`sqrt`, `sin`, `cos`, `atan2`, `floor`, `rand`, `init_stars`,
`update_stars`, `draw_stars`.

## Хуки и время

Хост вызывает `init`, `update`, `draw`, `touch` и обновляет `screen_w`,
`screen_h`, `dt` (секунды прошлого кадра, ограничено 0.1 с) и `joy` —
джойстик с полями `x, y, dx, dy, ox, oy, r`. Считайте анимации и таймеры
через `dt`, а не через счётчики кадров: FPS не фиксирован.

`touch num x, num y, num action, num id` поддерживает мультитач: `id` —
стабильный номер пальца внутри жеста, так что джойстик и кнопка атаки
работают одновременно разными пальцами. Действия: `0` — нажатие,
`1` — отпускание, `2` — движение, `3` — системная отмена жеста.

## Рендер и ресурсы

Окно лочится один раз на кадр, скрипт собирает команды, `graphics.c`
растеризует их в правильном порядке. Никаких OpenGL/EGL.

PNG кладите в `game/assets`, имя в скрипте — относительно этой папки:
`tex x, y, "player.png", 0, 1` или через константу `str`. Перед `aapt`
ресурсы копируются в `staging/assets`:

```sh
python3 stage_assets.py game/assets staging/assets
```

## Ошибки и перезапуск

Каждый хук запускается через `ds_call_protected`: при ошибке хост показывает
безопасный экран ошибки и через секунду вызывает `reset`, затем `init`.

## Сборка

Воркфлоу `.github/workflows/main.yml` копирует ресурсы в `staging/assets`,
NDK собирает `game/game.c`, `runtime.c`, `main.c` (graphics.c встроен в
main.c через `#include`). `libEGL` и `libGLESv2` не нужны.
