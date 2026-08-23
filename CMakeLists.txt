cmake_minimum_required(VERSION 3.16)
project(ZeroHabit C)
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
    # Картинки и шрифт зашиваются прямо в Game.exe как RCDATA-ресурсы
    # (embed_assets.py -> embedded_assets.rc), поэтому рядом с exe в
    # релизном пакете никаких файлов assets/ больше не лежит.
    set(EMBED_RC "${CMAKE_CURRENT_BINARY_DIR}/embedded_assets.rc")
    file(GLOB_RECURSE ASSET_FILES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/game/assets/*")
    add_custom_command(
        OUTPUT "${EMBED_RC}"
        COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/embed_assets.py" "${CMAKE_CURRENT_SOURCE_DIR}/game/assets" "${EMBED_RC}"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/embed_assets.py" ${ASSET_FILES}
        COMMENT "Embedding game assets into Game.exe resources"
    )
    # Иконка ПК-версии — icon.ico в корне проекта (для Android рядом лежит
    # icon.png, его забирает CI). Файл ложится в Game.exe ресурсом с id 1 —
    # эту иконку показывает и Проводник, и окно игры.
    set(APP_ICON_RC "")
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/icon.ico")
        set(APP_ICON_RC "${CMAKE_CURRENT_BINARY_DIR}/app_icon.rc")
        file(TO_NATIVE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/icon.ico" APP_ICON_PATH)
        string(REPLACE "\\" "\\\\" APP_ICON_PATH "${APP_ICON_PATH}")
        file(WRITE "${APP_ICON_RC}" "1 ICON \"${APP_ICON_PATH}\"\n")
        message(STATUS "ZeroHabit: иконка icon.ico вшивается в Game.exe")
    else()
        message(STATUS "ZeroHabit: icon.ico в корне не найден, exe останется со стандартной иконкой")
    endif()
    add_executable(Game WIN32 "${GAME_C}" runtime.c main_win32.c "${EMBED_RC}" ${APP_ICON_RC})
    target_include_directories(Game PRIVATE . game)
    target_link_libraries(Game PRIVATE gdi32 wininet user32 kernel32 shell32)
    if(NOT MSVC)
        target_link_libraries(Game PRIVATE m)
    endif()
else()
    add_executable(Game "${GAME_C}" runtime.c main_win32.c)
    target_include_directories(Game PRIVATE . game)
    target_link_libraries(Game PRIVATE m pthread)
    add_custom_command(TARGET Game POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/game/assets" "$<TARGET_FILE_DIR:Game>/assets")
endif()
