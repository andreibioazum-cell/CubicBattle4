# Gig1.0 / DimScript (Android 10+ / Windows PC)

## Сборка

### Генерация C-кода:
```sh
python3 gen.py
```

### Windows PC (.exe):
```cmd
build_pc.bat
```
или через CMake:
```cmd
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

### Android 10 NDK (APK):
```sh
aarch64-linux-android29-clang -O3 -shared game/game.c runtime.c main.c $GLUE/android_native_app_glue.c -I. -I$GLUE -landroid -llog -lm -o staging/lib/arm64-v8a/libds_game.so
armv7a-linux-androideabi29-clang -O3 -shared game/game.c runtime.c main.c $GLUE/android_native_app_glue.c -I. -I$GLUE -landroid -llog -lm -o staging/lib/armeabi-v7a/libds_game.so
aapt package -f -M game/AndroidManifest.xml -I android-34/android.jar -F unsigned.apk ./staging/
```

## Управление на ПК:
- `W`, `A`, `S`, `D` / Стрелки — движение
- `Space`, `J`, `F` — прицел (удар при отпускании)
- Мышь (ЛКМ) — взаимодействие с UI и джойстиком
- `Enter` — отправка в чате
- `F11` — полный экран
- `Alt + F4` — подтверждение выхода
