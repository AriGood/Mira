# The Qt frontend. Deliberately minimal for now — a blank window is the whole
# scope of this pass (see docs/architecture.md for what it grows into: a
# tray-capable process that can supervise mirad itself when systemd isn't
# managing it). It is a separate binary and a separate CMake project scope
# from mira_core: the frontend talks to the daemon exclusively over the REST
# API in docs/api.md, never by linking backend code directly.
#
# cmake_minimum_required + project() here are what let this directory also
# be configured standalone (cmake -S frontend -B frontend/build), for
# frontend-only iteration without touching the backend build at all — not
# required when pulled in via the root's add_subdirectory(frontend), but
# harmless there too (a nested project() just declares a sub-project).
cmake_minimum_required(VERSION 3.20)
project(mira-gui LANGUAGES CXX)

find_package(Qt6 6.5 QUIET COMPONENTS Widgets)

if(NOT Qt6_FOUND)
  message(STATUS "Qt6 (>= 6.5, Widgets) not found — skipping mira-gui. "
                 "The backend (mirad, mira) does not depend on it.")
  return()
endif()

message(STATUS "Qt6 ${Qt6_VERSION} found — building mira-gui")

qt_standard_project_setup()

qt_add_executable(mira-gui
  main.cpp
  MainWindow.cpp
  MainWindow.h
  MiradClient.cpp
  MiradClient.h
  GameColors.h
  GameDetailDialog.cpp
  GameDetailDialog.h
  SettingsDialog.cpp
  SettingsDialog.h
)

# The placeholder app icon, embedded so mira-gui has a window/taskbar icon
# regardless of whether it's installed (packaging/mira.desktop's Icon=mira
# is the separate, install-time path via the hicolor theme — see the root
# CMakeLists.txt's install() rules for that copy).
set(MIRA_ICON_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../packaging/icons/hicolor")
qt_add_resources(mira-gui "icons"
  PREFIX "/icons"
  BASE "${MIRA_ICON_DIR}"
  FILES
    "${MIRA_ICON_DIR}/16x16/apps/mira.png"
    "${MIRA_ICON_DIR}/32x32/apps/mira.png"
    "${MIRA_ICON_DIR}/48x48/apps/mira.png"
    "${MIRA_ICON_DIR}/64x64/apps/mira.png"
    "${MIRA_ICON_DIR}/128x128/apps/mira.png"
    "${MIRA_ICON_DIR}/256x256/apps/mira.png"
)

# mira_third_party is header-only (httplib.h, json.hpp) and carries no
# backend code, so linking it does not violate the "never link mira_core"
# rule in docs/architecture.md — it's the same REST-client toolkit `mira`
# uses, not a shortcut past the API.
target_link_libraries(mira-gui PRIVATE Qt6::Widgets mira_third_party Threads::Threads)
target_compile_features(mira-gui PRIVATE cxx_std_20)

set_target_properties(mira-gui PROPERTIES
  WIN32_EXECUTABLE OFF
  MACOSX_BUNDLE OFF
)

include(GNUInstallDirs)
install(TARGETS mira-gui RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
