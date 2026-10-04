cmake_minimum_required(VERSION 3.19)
# version: -DPW_LOOPER_VERSION= override (nix), then git tag, else 0.0.0
set(PW_LOOPER_VERSION "" CACHE STRING "pw-looper version")
if(NOT PW_LOOPER_VERSION)
    execute_process(
        COMMAND git describe --tags --match "v*"
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        OUTPUT_VARIABLE PW_LOOPER_VERSION
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
endif()
if(NOT PW_LOOPER_VERSION)
    set(PW_LOOPER_VERSION "0.0.0") # ponytail: no .git and no -D override (plain tarball build)
endif()
string(REGEX REPLACE "^v" "" PW_LOOPER_VERSION "${PW_LOOPER_VERSION}")
string(REGEX REPLACE "-.*$" "" PW_LOOPER_VERSION "${PW_LOOPER_VERSION}") # strip -dirty/-g<hash>
project(pw-looper VERSION ${PW_LOOPER_VERSION} LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets Network)
find_package(PkgConfig REQUIRED)
pkg_check_modules(PIPEWIRE REQUIRED IMPORTED_TARGET libpipewire-0.3)

qt_standard_project_setup()

add_executable(pw-looper
    src/main.cpp
    src/pipewire_engine.hpp
    src/pipewire_engine.cpp
    src/main_window.hpp
    src/main_window.cpp)

target_link_libraries(pw-looper PRIVATE Qt6::Widgets Qt6::Network PkgConfig::PIPEWIRE)

include(GNUInstallDirs)
install(TARGETS pw-looper DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES assets/pw-looper.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)

# ── Packaging (deb/rpm) ───────────────────────────────────────────
# CPACK_* must be set BEFORE include(CPack) — it snapshots them.
set(CPACK_PACKAGE_NAME "pw-looper")
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