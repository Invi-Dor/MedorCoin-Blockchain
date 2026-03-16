cmake_minimum_required(VERSION 3.19)

project(MedorCoin
    VERSION 0.1.0
    DESCRIPTION "MedorCoin blockchain node"
    LANGUAGES C CXX
)

# ── Language standards ────────────────────────────────────────────────────────
set(CMAKE_C_STANDARD            11)
set(CMAKE_C_STANDARD_REQUIRED   ON)
set(CMAKE_CXX_STANDARD          20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS        OFF)

# ── Build type default ────────────────────────────────────────────────────────
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
        Debug Release RelWithDebInfo MinSizeRel)
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

# ── Build options ─────────────────────────────────────────────────────────────
option(ENABLE_WARNINGS  "Enable extra compiler warnings"  ON)
option(ENABLE_ASAN      "Enable AddressSanitizer"         OFF)
option(ENABLE_UBSAN     "Enable UBSanitizer"              OFF)
option(ENABLE_COVERAGE  "Enable coverage instrumentation" OFF)

# ── Platform flags ────────────────────────────────────────────────────────────
if(NOT MSVC)
    add_compile_options(-fPIC)

    if(ENABLE_ASAN)
        add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
        add_link_options   (-fsanitize=address)
    endif()

    if(ENABLE_UBSAN)
        add_compile_options(-fsanitize=undefined -fno-omit-frame-pointer)
        add_link_options   (-fsanitize=undefined)
    endif()

    if(ENABLE_COVERAGE AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        add_compile_options(--coverage -fprofile-arcs -ftest-coverage -O0)
        add_link_options   (--coverage)
    endif()
else()
    add_compile_options(/bigobj)
endif()

# ── Global include path ───────────────────────────────────────────────────────
include_directories(${CMAKE_SOURCE_DIR}/include)

# ── Core dependencies resolved once at root; consumed in src/ ─────────────────
find_package(Threads   REQUIRED)
find_package(OpenSSL   REQUIRED)
find_package(PkgConfig REQUIRED)

pkg_check_modules(SECP256K1 REQUIRED libsecp256k1)
pkg_check_modules(LEVELDB   REQUIRED libleveldb)

# nlohmann/json — system package preferred; FetchContent fallback
find_package(nlohmann_json 3.10 QUIET)
if(NOT nlohmann_json_FOUND)
    include(FetchContent)
    FetchContent_Declare(nlohmann_json
        GIT_REPOSITORY https://github.com/nlohmann/json.git
        GIT_TAG        v3.11.3
    )
    FetchContent_MakeAvailable(nlohmann_json)
endif()

include(GNUInstallDirs)

# ── Subdirectories ────────────────────────────────────────────────────────────
add_subdirectory(src)

# ── Install public headers ────────────────────────────────────────────────────
if(EXISTS "${CMAKE_SOURCE_DIR}/include")
    install(DIRECTORY ${CMAKE_SOURCE_DIR}/include/
            DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
endif()

# ── Install systemd service unit ──────────────────────────────────────────────
# Installed only when the prefix is a real system path so that developer
# builds under ~/build do not pollute /lib/systemd.
if(CMAKE_INSTALL_PREFIX STREQUAL "/usr" OR
   CMAKE_INSTALL_PREFIX STREQUAL "/usr/local")
    install(FILES ${CMAKE_SOURCE_DIR}/medorcoin-blockchain.service
            DESTINATION ${CMAKE_INSTALL_PREFIX}/lib/systemd/system)
endif()

# ── Self-update target (explicit invocation only) ─────────────────────────────
find_program(GIT_EXECUTABLE git)
if(GIT_EXECUTABLE)
    set(SELF_UPDATE_BRANCH           "main" CACHE STRING
        "Git branch to pull during self_update")
    set(SELF_UPDATE_INTERVAL_SECONDS "300"  CACHE STRING
        "Seconds between update cycles")

    set(_SU_SCRIPT "${CMAKE_BINARY_DIR}/self_update_forever.sh")
    file(WRITE "${_SU_SCRIPT}" "#!/usr/bin/env bash
set -euo pipefail
BRANCH=\"${SELF_UPDATE_BRANCH}\"
INTERVAL=\"${SELF_UPDATE_INTERVAL_SECONDS}\"
REPO=\"${CMAKE_SOURCE_DIR}\"
BUILD=\"${CMAKE_BINARY_DIR}\"

echo \"[self-update] branch=\$BRANCH interval=\${INTERVAL}s\"
while true; do
  cd \"\$REPO\"
  ${GIT_EXECUTABLE} fetch --tags origin               || true
  ${GIT_EXECUTABLE} checkout \"\$BRANCH\"             || true
  ${GIT_EXECUTABLE} pull --ff-only origin \"\$BRANCH\" || true
  cmake -S \"\$REPO\" -B \"\$BUILD\" -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}
  cmake --build \"\$BUILD\" -j
  echo \"[self-update] cycle complete; sleeping \${INTERVAL}s\"
  sleep \"\$INTERVAL\"
done
")
    execute_process(COMMAND chmod +x "${_SU_SCRIPT}")

    add_custom_target(self_update_forever
        COMMAND "${_SU_SCRIPT}"
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        USES_TERMINAL
        COMMENT "Infinite self-update: fetch → reconfigure → rebuild → sleep"
    )
else()
    message(STATUS "git not found — self_update_forever target disabled")
endif()

# ── Packaging ─────────────────────────────────────────────────────────────────
set(CPACK_PACKAGE_NAME    "MedorCoin")
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
include(CPack)
