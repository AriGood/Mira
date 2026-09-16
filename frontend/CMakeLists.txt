# The Qt frontend. Deliberately minimal for now — a blank window is the whole
# scope of this pass (see docs/architecture.md for what it grows into: a
# tray-capable process that can supervise mirad itself when systemd isn't
# managing it). It is a separate binary and a separate CMake project scope
# from mira_core: the frontend talks to the daemon exclusively over the REST
# API in docs/api.md, never by linking backend code directly.
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
)

target_link_libraries(mira-gui PRIVATE Qt6::Widgets)
target_compile_features(mira-gui PRIVATE cxx_std_20)

set_target_properties(mira-gui PROPERTIES
  WIN32_EXECUTABLE OFF
  MACOSX_BUNDLE OFF
)

include(GNUInstallDirs)
install(TARGETS mira-gui RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
