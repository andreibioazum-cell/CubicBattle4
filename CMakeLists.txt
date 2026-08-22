cmake_minimum_required(VERSION 3.16)
project(CubicBattle C)
set(CMAKE_C_STANDARD 99)

if(MSVC)
    add_compile_options(/utf-8)
endif()

find_package(Python3 COMPONENTS Interpreter REQUIRED)
set(GAME_C "${CMAKE_CURRENT_SOURCE_DIR}/game/game.c")
file(GLOB_RECURSE DS_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/game/*.ds")

add_custom_command(
    OUTPUT "${GAME_C}"
    COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/gen.py"
    DEPENDS ${DS_SOURCES} "${CMAKE_CURRENT_SOURCE_DIR}/ds_compiler.py" "${CMAKE_CURRENT_SOURCE_DIR}/gen.py"
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    COMMENT "Generating game/game.c from DimScript"
)

if(ANDROID)
    add_library(ds_game SHARED "${GAME_C}" runtime.c main.c ${ANDROID_NDK}/sources/android/native_app_glue/android_native_app_glue.c)
    target_include_directories(ds_game PRIVATE . game ${ANDROID_NDK}/sources/android/native_app_glue)
    target_link_libraries(ds_game PRIVATE android log m)
elseif(WIN32 OR MINGW)
    # Картинки, шрифт и звуки зашиваются прямо в Game.exe как RCDATA-ресурсы
    # (embed_assets.py -> embedded_assets.rc; файлы из game/sounds получают
    # имя "sounds/<файл>"), поэтому рядом с exe в релизном пакете никаких
    # файлов assets/ и sounds/ больше не лежит.
    set(EMBED_RC "${CMAKE_CURRENT_BINARY_DIR}/embedded_assets.rc")
    file(GLOB_RECURSE ASSET_FILES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/game/assets/*")
    file(GLOB_RECURSE SOUND_FILES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/game/sounds/*")
    add_custom_command(
        OUTPUT "${EMBED_RC}"
        COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/embed_assets.py" "${CMAKE_CURRENT_SOURCE_DIR}/game/assets" "${CMAKE_CURRENT_SOURCE_DIR}/game/sounds" "${EMBED_RC}"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/embed_assets.py" ${ASSET_FILES} ${SOUND_FILES}
        COMMENT "Embedding game assets and sounds into Game.exe resources"
    )
    add_executable(Game WIN32 "${GAME_C}" runtime.c main_win32.c "${EMBED_RC}")
    target_include_directories(Game PRIVATE . game)
    target_link_libraries(Game PRIVATE gdi32 winmm wininet user32 kernel32 shell32)
    if(NOT MSVC)
        target_link_libraries(Game PRIVATE m)
    endif()
else()
    add_executable(Game "${GAME_C}" runtime.c main_win32.c)
    target_include_directories(Game PRIVATE . game)
    target_link_libraries(Game PRIVATE m pthread)
    add_custom_command(TARGET Game POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/game/assets" "$<TARGET_FILE_DIR:Game>/assets")
    add_custom_command(TARGET Game POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/game/sounds" "$<TARGET_FILE_DIR:Game>/sounds")
endif()
