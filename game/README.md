# Папка игры

Здесь находятся файлы конкретной игры:

- `game.ds` — основной исходник;
- другие `*.ds` — модули игры;
- `AndroidManifest.xml` — package, имя и параметры Android-приложения;
- `assets/` — PNG, звуки и другие ресурсы, которые попадут в APK.

## Подключение модулей

В `game.ds` можно подключить другой файл как в C:

```c
#include "player.ds"
#include "modules/level.ds"
```

Пути относительны текущему `.ds`-файлу, вложенные include поддерживаются, а
повторно один файл не компилируется. При сборке всей папки верхнеуровневые
`.ds`-файлы по-прежнему добавляются автоматически для обратной совместимости.

## PNG

Положите PNG, например, в `game/assets/player.png`, затем используйте:

```text
str PLAYER_PNG = "player.png"

fn init() {
    png_load(PLAYER_PNG)
}

fn draw() {
    tex(100, 100, PLAYER_PNG, 0, 1)
}
```

`png_load` заранее декодирует и кэширует картинку, но вызывать её необязательно:
`tex` сделает это сам. Поддерживаются RGBA-прозрачность, масштаб и поворот в
радианах. Вложенный ресурс `game/assets/sprites/hero.png` называется в скрипте
`"sprites/hero.png"`.

## Генерация C

Из корня репозитория:

```sh
python3 gen.py
```

Для другой папки или одного файла:

```sh
python3 gen.py examples/game_template examples/game_template/game.c
python3 gen.py game/game.ds game/game.c
```

Сгенерированный `game.c` — промежуточный файл, добавлять его в Git не нужно.
