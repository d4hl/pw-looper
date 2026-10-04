cmake_minimum_required(VERSION 3.19)
project(pw-looper LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets)
find_package(PkgConfig REQUIRED)
pkg_check_modules(PIPEWIRE REQUIRED IMPORTED_TARGET libpipewire-0.3)

qt_standard_project_setup()

add_executable(pw-looper
    src/main.cpp
    src/pipewire_engine.hpp
    src/pipewire_engine.cpp
    src/main_window.hpp
    src/main_window.cpp)

target_link_libraries(pw-looper PRIVATE Qt6::Widgets PkgConfig::PIPEWIRE)

include(GNUInstallDirs)
install(TARGETS pw-looper DESTINATION ${CMAKE_INSTALL_BINDIR})

# ── Packaging (deb/rpm) ───────────────────────────────────────────
# CPACK_* must be set BEFORE include(CPack) — it snapshots them.
set(CPACK_PACKAGE_NAME "pw-looper")
set(CPACK_PACKAGE_VERSION "0.1.0")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "PipeWire loopback utility")
set(CPACK_PACKAGE_VENDOR "pw-looper")
set(CPACK_PACKAGE_CONTACT "pw-looper <pw-looper@localhost>")
set(CPACK_DEBIAN_PACKAGE_MAINTAINER "pw-looper <pw-looper@localhost>")
set(CPACK_DEBIAN_PACKAGE_DEPENDS "pipewire-0.3")
set(CPACK_RPM_PACKAGE_REQUIRES "pipewire")
set(CPACK_RPM_PACKAGE_LICENSE "MIT")

include(CPack)

add_executable(test_loopback_enable tests/test_enable.cpp)
target_sources(test_loopback_enable PRIVATE src/pipewire_engine.cpp)
target_include_directories(test_loopback_enable PRIVATE src)
target_link_libraries(test_loopback_enable PRIVATE Qt6::Core PkgConfig::PIPEWIRE)

add_executable(test_scenario tests/test_scenario.cpp)
target_sources(test_scenario PRIVATE src/pipewire_engine.cpp)
target_include_directories(test_scenario PRIVATE src)
target_link_libraries(test_scenario PRIVATE Qt6::Core PkgConfig::PIPEWIRE)