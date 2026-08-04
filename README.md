# Gig1.0 / DimScript

Шаблон Android-игры на DimScript. Все файлы конкретной игры находятся в
`game/`:

```text
game/
├── AndroidManifest.xml  # имя приложения и package
├── *.ds                 # код игры, можно разделить на несколько файлов
├── README.md            # инструкция для игры
└── assets/              # ресурсы игры
```

## Своя игра

1. Измените `game/*.ds` или замените файлы своим исходным кодом.
2. Разделяйте большую игру на несколько `.ds`-файлов: они компилируются вместе
   и видят функции и глобальные значения друг друга. Простые команды можно
   записывать компактно через `;` на одной строке.
3. Положите ресурсы в `game/assets/`.
4. В `game/AndroidManifest.xml` измените `android:label`, например на
   `Game DS`, и задайте уникальный `package`.
5. Сгенерируйте C-код:

   ```sh
   ./gen.sh
   ```

   По умолчанию все `.ds`-файлы из `game/` будут собраны в `game/game.c`.

## Пример многофайлового проекта

Готовый шаблон находится в `examples/game_template/`. В нём `game.ds`
содержит точки входа, а `player.ds` — код игрока и joystick:

```sh
./gen.sh examples/game_template examples/game_template/game.c
```

Манифест шаблона также лежит рядом с исходниками и содержит отдельное имя
`DimScript Example` и package.

APK собирается GitHub Actions из `game/` и использует манифест
`game/AndroidManifest.xml`.
