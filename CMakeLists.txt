cmake_minimum_required(VERSION 3.16)
project(CubicBattle C)
set(CMAKE_C_STANDARD 99)

find_package(Python3 COMPONENTS Interpreter REQUIRED)
set(GAME_C "${CMAKE_CURRENT_SOURCE_DIR}/game/game.c")
# The scripts live in modules under game/scripts (core, ui, combat, fx). Their
# compile order comes from gen.py, so finding every .ds is enough here.
file(GLOB_RECURSE DS_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/game/*.ds")

add_custom_command(
    OUTPUT "${GAME_C}"
    COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/gen.py"
    DEPENDS ${DS_SOURCES} "${CMAKE_CURRENT_SOURCE_DIR}/ds_compiler.py" "${CMAKE_CURRENT_SOURCE_DIR}/gen.py"
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    COMMENT "Generating game/game.c from DimScript"
)

# Android is the only target: the libds_game.so shared library that the APK packs
# through game/AndroidManifest.xml. Desktop support, Windows and the Linux test
# harness, was removed from the repository.
if(NOT ANDROID)
    message(FATAL_ERROR
        "Cubic Battle builds for Android only. Run CMake through the NDK toolchain "
        "file, for example: cmake -B build -DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK_ROOT/build/cmake/android.toolchain.cmake "
        "-DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-26 -DANDROID_STL=c++_shared. "
        "The GitHub Actions workflow is the easy way to get an APK.")
endif()

add_library(ds_game SHARED "${GAME_C}" runtime.c main.c ${ANDROID_NDK}/sources/android/native_app_glue/android_native_app_glue.c)
target_include_directories(ds_game PRIVATE . game ${ANDROID_NDK}/sources/android/native_app_glue)
# vulkan is the rendering backend: swapchain, offscreen target, prebaked SPIR-V.
# dl brings dlsym, because ANativeWindow_setFrameRate only arrived in API 30 and
# the pointer to it is looked up at runtime, missing on older systems.
target_link_libraries(ds_game PRIVATE android log dl m vulkan)
