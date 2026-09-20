# The Qt frontend: a cover-art library grid (LibraryWindow) with the older
# table view (MainWindow) kept as `mira-gui --classic`. See
# docs/architecture.md for what it still grows into: a tray-capable process
# that can supervise mirad itself when systemd isn't managing it.
# It is a separate binary and a separate CMake project scope
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

find_package(Qt6 6.5 QUIET COMPONENTS Widgets DBus)

if(NOT Qt6_FOUND)
  message(STATUS "Qt6 (>= 6.5, Widgets) not found — skipping mira-gui. "
                 "The backend (mirad, mira) does not depend on it.")
  return()
endif()

message(STATUS "Qt6 ${Qt6_VERSION} found — building mira-gui")

qt_standard_project_setup()

qt_add_executable(mira-gui
  main.cpp

  # client/ — everything that talks to mirad, and nothing that draws.
  client/Async.h
  client/EventStream.cpp
  client/EventStream.h
  client/JsonMapping.cpp
  client/JsonMapping.h
  client/MiradClient.cpp
  client/MiradClient.h
  client/Transport.cpp
  client/Transport.h
  client/Types.h

  # ui/ — presentation shared by more than one view or dialog.
  ui/AboutPanel.cpp
  ui/AboutPanel.h
  ui/ArtworkStore.cpp
  ui/ArtworkStore.h
  ui/CoverArt.cpp
  ui/CoverArt.h
  ui/DaemonSupervisor.cpp
  ui/DaemonSupervisor.h
  ui/GameActions.cpp
  ui/GameActions.h
  ui/GameDetailsPanel.cpp
  ui/GameDetailsPanel.h
  ui/HeroArtWidget.cpp
  ui/HeroArtWidget.h
  ui/GamePresentation.h
  ui/LibrarySort.cpp
  ui/LibrarySort.h
  ui/GameTileDelegate.cpp
  ui/GameTileDelegate.h
  ui/Icons.cpp
  ui/Icons.h
  ui/KeyBindings.cpp
  ui/KeyBindings.h
  ui/Notify.cpp
  ui/Notify.h
  ui/PopupDialog.cpp
  ui/PopupDialog.h
  ui/SystemNotifier.cpp
  ui/SystemNotifier.h
  ui/Shortcuts.cpp
  ui/Shortcuts.h
  ui/Theme.cpp
  ui/Theme.h
  ui/Tray.cpp
  ui/Tray.h
  ui/SettingsNav.cpp
  ui/SettingsNav.h
  ui/SettingsPanel.cpp
  ui/SettingsPanel.h
  ui/GameEditForm.cpp
  ui/GameEditForm.h

  # views/ — the two top-level library windows.
  views/LibraryWindow.cpp
  views/LibraryWindow.h
  views/MainWindow.cpp
  views/MainWindow.h

  # dialogs/
  dialogs/ArtworkPickerDialog.cpp
  dialogs/ArtworkPickerDialog.h
  dialogs/DeleteGameDialog.cpp
  dialogs/DeleteGameDialog.h
  dialogs/GameDetailDialog.cpp
  dialogs/GameDetailDialog.h
  dialogs/RunInPrefixDialog.cpp
  dialogs/RunInPrefixDialog.h
  dialogs/RunnerDialog.cpp
  dialogs/RunnerDialog.h
  dialogs/OverridesEditor.cpp
  dialogs/OverridesEditor.h
  dialogs/SettingsDialog.cpp
  dialogs/SettingsCategories.cpp
  dialogs/SettingsCategories.h
  dialogs/SettingsDialog.h
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

# The stylesheet and the themes that fill it in (ui/Theme.cpp). Embedded so a
# build tree, an AppImage and an installed copy all theme themselves the same
# way; a user's own themes are read from ~/.config/mira/themes at run time.
qt_add_resources(mira-gui "themes"
  PREFIX "/themes"
  BASE "${CMAKE_CURRENT_SOURCE_DIR}/themes"
  FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/themes/base.qss"
    "${CMAKE_CURRENT_SOURCE_DIR}/themes/mira-dark.toml"
    "${CMAKE_CURRENT_SOURCE_DIR}/themes/mira-light.toml"
)

# mira_third_party is header-only (httplib.h, json.hpp) and carries no
# backend code, so linking it does not violate the "never link mira_core"
# rule in docs/architecture.md — it's the same REST-client toolkit `mira`
# uses, not a shortcut past the API.
target_link_libraries(mira-gui PRIVATE Qt6::Widgets Qt6::DBus mira_third_party Threads::Threads)
target_compile_features(mira-gui PRIVATE cxx_std_20)

# The version the About panel shows. CMAKE_PROJECT_VERSION is the root
# project's, which a standalone frontend configure does not have.
set(MIRA_GUI_VERSION "${CMAKE_PROJECT_VERSION}")
if(NOT MIRA_GUI_VERSION)
  set(MIRA_GUI_VERSION "dev")
endif()
target_compile_definitions(mira-gui PRIVATE MIRA_VERSION="${MIRA_GUI_VERSION}")

set_target_properties(mira-gui PROPERTIES
  WIN32_EXECUTABLE OFF
  MACOSX_BUNDLE OFF
)

# Frontend unit tests. Deliberately its own executable rather than more
# files in the root's tests/: that target links mira_core, and a frontend
# test that could reach backend code would be able to "pass" against an
# implementation the real mira-gui never talks to. This one links only the
# header-only third-party deps the client itself uses.
#
# Only the layers with no Qt event loop and no socket in them are covered
# here — client/JsonMapping (JSON <-> Types.h) and the parts of the client
# that are pure functions. Anything needing a live mirad is exercised
# against the real daemon instead; see docs/architecture.md.
file(GLOB MIRA_GUI_TEST_SOURCES CONFIGURE_DEPENDS tests/*.cpp)
add_executable(mira_gui_tests
  ${MIRA_GUI_TEST_SOURCES}
  client/JsonMapping.cpp
  client/MiradClient.cpp
  ui/LibrarySort.cpp
  ui/DaemonSupervisor.cpp
  client/Transport.cpp
)
target_include_directories(mira_gui_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
# Qt6::Core, not Widgets: the covered code needs QPointer and the queued-call
# machinery to compile, but never a widget, a window or an event loop — a
# test that needed one of those would be testing the wrong layer.
target_link_libraries(mira_gui_tests PRIVATE mira_third_party Qt6::Core)
target_compile_features(mira_gui_tests PRIVATE cxx_std_20)
target_compile_options(mira_gui_tests PRIVATE -Wall -Wextra)
add_test(NAME frontend_unit COMMAND mira_gui_tests)

include(GNUInstallDirs)
install(TARGETS mira-gui RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
