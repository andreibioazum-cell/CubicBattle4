# Gig1.0 / DimScript

Шаблон Android-игры на DimScript. Все файлы конкретной игры находятся в
`game/`:

```text
game/
├── AndroidManifest.xml  # имя приложения и package
├── game.ds              # код игры
├── README.md            # инструкция для игры
└── assets/              # ресурсы игры
```

## Своя игра

1. Измените `game/game.ds` или замените его своим исходником.
2. Положите ресурсы в `game/assets/`.
3. В `game/AndroidManifest.xml` измените `android:label`, например на
   `Game DS`, и задайте уникальный `package`.
4. Сгенерируйте C-код:

   ```sh
   ./gen.sh
   ```

   По умолчанию будет создан `game/game.c`.

Можно скомпилировать другой файл без изменения проекта:

```sh
./gen.sh game/my_game.ds game/my_game.c
```

APK собирается GitHub Actions из `game/game.c` и использует манифест из
`game/AndroidManifest.xml`.
