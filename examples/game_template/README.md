# Шаблон многофайловой игры DimScript

Скопируйте эту папку для новой игры. В примере:

- `game.ds` подключает `player.ds` через `#include "player.ds"`;
- `player.ds` хранит состояние и функции игрока;
- `assets/player.png` предварительно загружается через `png_load` и рисуется
  через `tex`;
- `AndroidManifest.xml` задаёт название приложения и Android package.

Из корня репозитория:

```sh
python3 gen.py examples/game_template examples/game_template/game.c
```

Можно скомпилировать только точку входа — подключённые файлы будут найдены
рекурсивно:

```sh
python3 gen.py examples/game_template/game.ds examples/game_template/game.c
```

Пути include считаются относительно подключающего файла, поэтому модули можно
раскладывать по подпапкам. Один файл загружается только один раз; циклические
include считаются ошибкой.

PNG кладутся в `assets/`, но в скрипте указывается путь относительно неё:
`"player.png"` или `"sprites/player.png"`. Сгенерированный `game.c` добавлять в
Git не нужно.
