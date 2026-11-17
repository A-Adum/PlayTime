cmake_minimum_required(VERSION 3.25)
project(PlayTime VERSION 1.0.0 LANGUAGES C CXX RC)

if(NOT WIN32)
  message(FATAL_ERROR "PlayTime is a Windows application.")
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(PLAYTIME_BUILD_TESTS "Build the automated test suite" ON)

set(PLAYTIME_DEFS UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00 WINVER=0x0A00
    PLAYTIME_VERSION="${PROJECT_VERSION}")

if(MSVC)
  set(PLAYTIME_WARN /W4 /utf-8 /permissive-)
  set(PLAYTIME_LINK_STATIC "")
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
else()
  set(PLAYTIME_WARN -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -Wno-cast-function-type)
  # Fully static: no libstdc++/libgcc/winpthread DLLs shipped next to the exe.
  set(PLAYTIME_LINK_STATIC -static -static-libgcc -static-libstdc++)
  add_compile_options($<$<CONFIG:Release,MinSizeRel>:-ffunction-sections> $<$<CONFIG:Release,MinSizeRel>:-fdata-sections>)
  add_link_options($<$<CONFIG:Release,MinSizeRel>:-Wl,--gc-sections> $<$<CONFIG:Release,MinSizeRel>:-s>)
endif()

# ---- SQLite (vendored amalgamation, single-threaded, trimmed) ----
add_library(sqlite3 STATIC third_party/sqlite/sqlite3.c)
target_include_directories(sqlite3 PUBLIC third_party/sqlite)
target_compile_definitions(sqlite3 PRIVATE
  SQLITE_THREADSAFE=0 SQLITE_DEFAULT_MEMSTATUS=0 SQLITE_DQS=0 SQLITE_OMIT_LOAD_EXTENSION
  SQLITE_OMIT_DEPRECATED SQLITE_OMIT_SHARED_CACHE SQLITE_DEFAULT_FOREIGN_KEYS=1
  SQLITE_DEFAULT_CACHE_SIZE=-512 SQLITE_LIKE_DOESNT_MATCH_BLOBS SQLITE_MAX_EXPR_DEPTH=0
  SQLITE_DEFAULT_WAL_SYNCHRONOUS=1 SQLITE_WIN32_MALLOC)
if(NOT MSVC)
  target_compile_options(sqlite3 PRIVATE -w)
endif()

# ---- Core engine (no UI) ----
add_library(playtime_core STATIC
  src/util/Str.cpp
  src/util/Json.cpp
  src/util/Vdf.cpp
  src/util/TimeUtil.cpp
  src/core/Database.cpp
  src/core/SettingsManager.cpp
  src/core/GameLibrary.cpp
  src/core/AccountManager.cpp
  src/core/SessionStore.cpp
  src/core/PlaytimeCalculator.cpp
  src/core/StatisticsEngine.cpp
  src/core/GameDetector.cpp
  src/core/SessionTracker.cpp
  src/core/BackupManager.cpp
  src/platform/ProcessMonitor.cpp
  src/platform/LauncherDiscovery.cpp
  src/platform/SteamImport.cpp
  src/platform/WinUtil.cpp
)
target_include_directories(playtime_core PUBLIC src)
target_compile_definitions(playtime_core PUBLIC ${PLAYTIME_DEFS})
target_compile_options(playtime_core PRIVATE ${PLAYTIME_WARN})
target_link_libraries(playtime_core PUBLIC sqlite3 advapi32 shell32 ole32 uuid winhttp)

# ---- Desktop application ----
add_executable(PlayTime WIN32
  src/main.cpp
  src/app/Application.cpp
  src/ui/Gfx.cpp
  src/ui/MainWindow.cpp
  src/ui/Views.cpp
  src/ui/Dialogs.cpp
  src/ui/TrayManager.cpp
  res/playtime.rc
)
target_compile_options(PlayTime PRIVATE ${PLAYTIME_WARN})
target_link_libraries(PlayTime PRIVATE playtime_core user32 gdi32 comctl32 comdlg32 uxtheme dwmapi
  wtsapi32 psapi windowscodecs uuid ${PLAYTIME_LINK_STATIC})
if(MSVC)
  target_link_options(PlayTime PRIVATE /MANIFEST:NO)
else()
  target_link_options(PlayTime PRIVATE -municode)
endif()

# ---- Tests ----
if(PLAYTIME_BUILD_TESTS)
  enable_testing()
  add_executable(playtime_tests tests/test_main.cpp tests/test_core.cpp)
  target_compile_options(playtime_tests PRIVATE ${PLAYTIME_WARN})
  target_link_libraries(playtime_tests PRIVATE playtime_core ${PLAYTIME_LINK_STATIC})
  add_test(NAME playtime_tests COMMAND playtime_tests)
endif()

install(TARGETS PlayTime RUNTIME DESTINATION .)
install(FILES README.md DESTINATION .)
