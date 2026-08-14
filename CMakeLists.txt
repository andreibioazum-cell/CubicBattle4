cmake_minimum_required(VERSION 3.16)
project(CubicBattle C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

# 1. Поиск Python3 для генерации C-кода из DimScript (.ds)
find_package(Python3 COMPONENTS Interpreter REQUIRED)

set(GAME_C "${CMAKE_CURRENT_SOURCE_DIR}/game/game.c")
file(GLOB_RECURSE DS_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/game/*.ds")

add_custom_command(
    OUTPUT "${GAME_C}"
    COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/gen.py"
    DEPENDS ${DS_SOURCES} "${CMAKE_CURRENT_SOURCE_DIR}/ds_compiler.py" "${CMAKE_CURRENT_SOURCE_DIR}/gen.py"
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    COMMENT "Compiling DimScript sources to game/game.c"
)

# 2. Определение таргета в зависимости от платформы
if(ANDROID)
    add_library(ds_game SHARED
        "${GAME_C}"
        runtime.c
        main.c
        ${ANDROID_NDK}/sources/android/native_app_glue/android_native_app_glue.c
    )
    target_include_directories(ds_game PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_SOURCE_DIR}/game"
        ${ANDROID_NDK}/sources/android/native_app_glue
    )
    target_link_libraries(ds_game PRIVATE android log m)

elseif(WIN32 OR MINGW)
    # Windows PC (.exe)
    add_executable(Game WIN32
        "${GAME_C}"
        runtime.c
        main_win32.c
    )
    target_include_directories(Game PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_SOURCE_DIR}/game"
    )
    target_link_libraries(Game PRIVATE
        gdi32
        wininet
        user32
        kernel32
        shell32
        m
    )
    
    # Копирование ассетов в директорию сборки рядом с Game.exe
    add_custom_command(TARGET Game POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/game/assets"
            "$<TARGET_FILE_DIR:Game>/assets"
        COMMENT "Copying game assets next to Game.exe"
    )

else()
    # Generic Desktop (Linux)
    add_executable(Game
        "${GAME_C}"
        runtime.c
        main_win32.c
    )
    target_include_directories(Game PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_SOURCE_DIR}/game"
    )
    target_link_libraries(Game PRIVATE m pthread)
endif()
