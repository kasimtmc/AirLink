# AirLink app: Kompoz (C++20) UI over the Rust core (core/airlink-capi).
#   Desktop:  cmake -S app -B app/build && cmake --build app/build -j && ./app/build/airlink
#   Android:  app/android (Gradle builds this file as libmain.so)
cmake_minimum_required(VERSION 3.24)
project(airlink_app LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(AIRLINK_ROOT "${CMAKE_CURRENT_LIST_DIR}/..")
set(KOMPOZ_DIR "${AIRLINK_ROOT}/../kompoz" CACHE PATH "Kompoz source tree (a clone or a submodule)")
if(NOT EXISTS "${KOMPOZ_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "Kompoz not found at ${KOMPOZ_DIR}; set -DKOMPOZ_DIR=/path/to/kompoz")
endif()
set(KOMPOZ_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(KOMPOZ_BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory("${KOMPOZ_DIR}" kompoz)

# ---- The Rust core, built by cargo as a static library -------------------------------------
find_program(CARGO cargo HINTS "$ENV{HOME}/.cargo/bin" REQUIRED)
get_filename_component(CARGO_BIN_DIR "${CARGO}" DIRECTORY)
set(CARGO_TARGET_DIR "${CMAKE_BINARY_DIR}/cargo")

if(ANDROID)
    if(ANDROID_ABI STREQUAL "arm64-v8a")
        set(RUST_TRIPLE aarch64-linux-android)
    elseif(ANDROID_ABI STREQUAL "x86_64")
        set(RUST_TRIPLE x86_64-linux-android)
    elseif(ANDROID_ABI STREQUAL "armeabi-v7a")
        set(RUST_TRIPLE armv7-linux-androideabi)
    else()
        message(FATAL_ERROR "Unsupported ABI ${ANDROID_ABI}")
    endif()
    set(AIRLINK_RUST_LIB "${CARGO_TARGET_DIR}/${RUST_TRIPLE}/release/libairlink.a")
    # cargo-ndk runs as cargo's subcommand (`cargo ndk`), found on PATH.
    find_program(CARGO_NDK cargo-ndk HINTS "$ENV{HOME}/.cargo/bin" REQUIRED)
    get_filename_component(CARGO_NDK_DIR "${CARGO_NDK}" DIRECTORY)
    set(CARGO_COMMAND ${CMAKE_COMMAND} -E env
        "PATH=${CARGO_NDK_DIR}:${CARGO_BIN_DIR}:$ENV{PATH}" "ANDROID_NDK_HOME=${CMAKE_ANDROID_NDK}" "CARGO_TARGET_DIR=${CARGO_TARGET_DIR}"
        "${CARGO}" ndk -t ${ANDROID_ABI} --platform ${ANDROID_NATIVE_API_LEVEL}
        build --release -p airlink-capi --lib)
elseif(WIN32 AND MINGW)
    set(AIRLINK_RUST_LIB "${CARGO_TARGET_DIR}/x86_64-pc-windows-gnu/release/libairlink.a")
    set(CARGO_COMMAND ${CMAKE_COMMAND} -E env "CARGO_TARGET_DIR=${CARGO_TARGET_DIR}"
        "${CARGO}" build --release -p airlink-capi --lib --target x86_64-pc-windows-gnu)
else()
    set(AIRLINK_RUST_LIB "${CARGO_TARGET_DIR}/release/libairlink.a")
    set(CARGO_COMMAND ${CMAKE_COMMAND} -E env "CARGO_TARGET_DIR=${CARGO_TARGET_DIR}"
        "${CARGO}" build --release -p airlink-capi --lib)
endif()

# Always run cargo: it is incremental and knows the Rust sources' dependencies.
add_custom_target(airlink_core_cargo
    COMMAND ${CARGO_COMMAND}
    WORKING_DIRECTORY "${AIRLINK_ROOT}"
    BYPRODUCTS "${AIRLINK_RUST_LIB}"
    COMMENT "Building the Rust core (airlink-capi)"
    USES_TERMINAL)

add_library(airlink_core STATIC IMPORTED GLOBAL)
set_target_properties(airlink_core PROPERTIES
    IMPORTED_LOCATION "${AIRLINK_RUST_LIB}"
    INTERFACE_INCLUDE_DIRECTORIES "${AIRLINK_ROOT}/core/airlink-capi/include")
add_dependencies(airlink_core airlink_core_cargo)
find_package(Threads REQUIRED)
if(WIN32)
    set(AIRLINK_RUST_SYSLIBS ws2_32 userenv bcrypt ntdll advapi32 kernel32 dbghelp iphlpapi
        ole32 uuid shell32)  # the last three: the file dialog, the Send To shortcut
elseif(ANDROID)
    set(AIRLINK_RUST_SYSLIBS dl m log)
else()
    # airlink-desktop (sharing this computer's screen): PipeWire for the portal's screen cast.
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(PIPEWIRE REQUIRED IMPORTED_TARGET libpipewire-0.3)
    pkg_check_modules(PULSE_SIMPLE REQUIRED IMPORTED_TARGET libpulse-simple)
    set(AIRLINK_RUST_SYSLIBS dl m PkgConfig::PIPEWIRE)
endif()
target_link_libraries(airlink_core INTERFACE Threads::Threads ${AIRLINK_RUST_SYSLIBS})

# ---- The app ---------------------------------------------------------------------------------
set(AIRLINK_SOURCES src/app_state.cpp src/ui.cpp)
if(ANDROID)
    add_library(main SHARED ${AIRLINK_SOURCES} src/platform_android.cpp)
    set(AIRLINK_TARGET main)
else()
    add_executable(airlink ${AIRLINK_SOURCES} src/platform_desktop.cpp)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        target_sources(airlink PRIVATE src/call_audio_linux.cpp src/desktop_audio_capture.cpp)
        target_link_libraries(airlink PRIVATE PkgConfig::PULSE_SIMPLE)
    elseif(WIN32)
        target_sources(airlink PRIVATE src/media_audio_windows.cpp src/call_audio_windows.cpp src/desktop_audio_capture.cpp)
        target_link_libraries(airlink PRIVATE winmm)
    endif()
    set(AIRLINK_TARGET airlink)
endif()
target_include_directories(${AIRLINK_TARGET} PRIVATE src)
# kompoz::app (an OBJECT library with the entry points: main, the activity's JNI methods) is
# linked straight into the final binary, so the linker keeps them.
target_link_libraries(${AIRLINK_TARGET} PRIVATE kompoz::app airlink_core)
# The UI's @Kompozable functions are Kotlin; kompozc rewrites them and passes the plain C++
# (app_state.cpp, the platform files) through as it is.
kompozable(${AIRLINK_TARGET})
if(NOT ANDROID)
    kompoz_copy_assets(${AIRLINK_TARGET})
endif()
if(MINGW)
    # One .exe that needs only Windows' own DLLs (no libstdc++/libgcc/winpthread next to it),
    # and no console window behind the app.
    target_link_options(${AIRLINK_TARGET} PRIVATE -static -mwindows)
endif()
