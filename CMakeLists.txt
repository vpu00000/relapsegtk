cmake_minimum_required(VERSION 3.20)
project(relapsegtk LANGUAGES C)

option(RELAPSEGTK_ENABLE_UPDATER "Build GitHub AppImage update support" OFF)

# Avoid embedding the builder's checkout path in diagnostics or debug records.
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options("-ffile-prefix-map=${CMAKE_CURRENT_SOURCE_DIR}=.")
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(GTK4 REQUIRED IMPORTED_TARGET gtk4>=4.16)
pkg_check_modules(GTKSOURCE REQUIRED IMPORTED_TARGET gtksourceview-5>=5.16)
pkg_check_modules(GIO REQUIRED IMPORTED_TARGET gio-unix-2.0)
pkg_check_modules(JSON_GLIB REQUIRED IMPORTED_TARGET json-glib-1.0)

add_library(relapse-settings STATIC src/settings/editor.c src/settings/session.c src/settings/output.c src/settings/store.c src/settings/launcher.c src/settings/scripts.c)
target_compile_features(relapse-settings PUBLIC c_std_11)
target_include_directories(relapse-settings PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(relapse-settings PUBLIC PkgConfig::GIO)

add_library(relapse-state STATIC src/state/session.c)
target_compile_features(relapse-state PUBLIC c_std_11)
target_include_directories(relapse-state PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(relapse-state PUBLIC PkgConfig::GIO PRIVATE PkgConfig::JSON_GLIB)

add_library(relapse-profiles STATIC src/profiles/store.c)
target_compile_features(relapse-profiles PUBLIC c_std_11)
target_include_directories(relapse-profiles PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(relapse-profiles PUBLIC PkgConfig::GIO)

add_library(relapse-socket STATIC src/socket/transport.c src/socket/client.c)
target_compile_features(relapse-socket PUBLIC c_std_11)
target_include_directories(relapse-socket PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(relapse-socket PUBLIC PkgConfig::GIO PRIVATE PkgConfig::JSON_GLIB)

add_library(relapse-bridge STATIC src/socket/bridge.c src/socket/console.c)
target_compile_features(relapse-bridge PUBLIC c_std_11)
target_link_libraries(relapse-bridge PUBLIC relapse-socket PRIVATE PkgConfig::JSON_GLIB)

add_library(relapse-sessions STATIC src/socket/sessions.c)
target_compile_features(relapse-sessions PUBLIC c_std_11)
target_link_libraries(relapse-sessions PUBLIC relapse-bridge)

pkg_check_modules(EPOXY REQUIRED IMPORTED_TARGET epoxy)

add_library(relapse-ui STATIC
    src/ui/window.c src/ui/style.c src/ui/editor.c src/ui/scripts.c src/ui/tabs.c src/ui/settings.c
    src/ui/tab_strip.c src/ui/toolbar.c src/ui/output.c src/ui/search.c src/ui/shader_strip.c src/ui/profiles.c src/ui/console.c
    src/ui/picker.c)
target_compile_features(relapse-ui PUBLIC c_std_11)
target_include_directories(relapse-ui PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(relapse-ui PUBLIC PkgConfig::GTK4 relapse-settings relapse-profiles PRIVATE relapse-state PkgConfig::GTKSOURCE PkgConfig::EPOXY)

find_program(GLIB_COMPILE_RESOURCES glib-compile-resources REQUIRED)
set(APP_RESOURCES "${CMAKE_CURRENT_BINARY_DIR}/relapsegtk-resources.c")
add_custom_command(
    OUTPUT "${APP_RESOURCES}"
    COMMAND "${GLIB_COMPILE_RESOURCES}"
        "${CMAKE_CURRENT_SOURCE_DIR}/resources/relapsegtk.gresource.xml"
        --sourcedir=${CMAKE_CURRENT_SOURCE_DIR}/resources
        --target=${APP_RESOURCES}
        --generate-source --c-name=relapsegtk
    DEPENDS resources/relapsegtk.gresource.xml resources/style.css resources/styles/relapse.xml
        resources/icons/relapse.svg
        resources/icons/chevron-left.svg resources/icons/chevron-right.svg
        resources/icons/caret-down.svg resources/icons/rename.svg resources/icons/profiles.svg
        resources/icons/editor.svg resources/icons/autoexec.svg resources/icons/settings.svg
        resources/icons/folder-open.svg resources/icons/play.svg
        resources/icons/save.svg resources/icons/delete.svg
        resources/scripts/session.luau resources/scripts/console.luau
    VERBATIM)

add_library(relapse-resources OBJECT "${APP_RESOURCES}")
target_link_libraries(relapse-resources PRIVATE PkgConfig::GIO)

set(RELAPSEGTK_SOURCES src/main.c src/app.c)
if(RELAPSEGTK_ENABLE_UPDATER)
    set(RELAPSEGTK_VERSION "" CACHE STRING "RelapseGTK version")
    set(RELAPSEGTK_UPDATE_REPOSITORY "" CACHE STRING "GitHub repository used for updates")
    set(RELAPSEGTK_UPDATE_PUBLIC_KEY "" CACHE FILEPATH "OpenPGP public key embedded for update verification")
    foreach(required RELAPSEGTK_VERSION RELAPSEGTK_UPDATE_REPOSITORY RELAPSEGTK_UPDATE_PUBLIC_KEY)
        if("${${required}}" STREQUAL "")
            message(FATAL_ERROR "${required} is required when RELAPSEGTK_ENABLE_UPDATER=ON")
        endif()
    endforeach()
    if(NOT EXISTS "${RELAPSEGTK_UPDATE_PUBLIC_KEY}")
        message(FATAL_ERROR "Update public key does not exist: ${RELAPSEGTK_UPDATE_PUBLIC_KEY}")
    endif()
    list(APPEND RELAPSEGTK_SOURCES src/update/update.c)
endif()

add_executable(relapsegtk ${RELAPSEGTK_SOURCES})
target_compile_features(relapsegtk PRIVATE c_std_11)
if(RELAPSEGTK_ENABLE_UPDATER)
    target_compile_definitions(relapsegtk PRIVATE
        RELAPSEGTK_UPDATE_ENABLED=1
        RELAPSEGTK_VERSION="${RELAPSEGTK_VERSION}"
        RELAPSEGTK_UPDATE_REPOSITORY="${RELAPSEGTK_UPDATE_REPOSITORY}")
    file(READ "${RELAPSEGTK_UPDATE_PUBLIC_KEY}" RELAPSEGTK_UPDATE_KEY_HEX HEX)
    string(LENGTH "${RELAPSEGTK_UPDATE_KEY_HEX}" RELAPSEGTK_UPDATE_KEY_HEX_LENGTH)
    math(EXPR RELAPSEGTK_UPDATE_KEY_SIZE "${RELAPSEGTK_UPDATE_KEY_HEX_LENGTH} / 2")
    string(REGEX REPLACE "(..)" "0x\\1," RELAPSEGTK_UPDATE_KEY_BYTES "${RELAPSEGTK_UPDATE_KEY_HEX}")
    configure_file(packaging/update-key.h.in "${CMAKE_CURRENT_BINARY_DIR}/relapsegtk-update-key.h" @ONLY)
    target_include_directories(relapsegtk PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
    target_compile_definitions(relapsegtk PRIVATE RELAPSEGTK_UPDATE_KEY_EMBEDDED=1)
endif()
target_link_libraries(relapsegtk PRIVATE relapse-ui relapse-sessions relapse-resources PkgConfig::JSON_GLIB)
