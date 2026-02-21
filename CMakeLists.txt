cmake_minimum_required(VERSION 3.19)

# Auto-find all common blockchain dependencies
find_package(PkgConfig REQUIRED)
pkg_check_modules(SECP256K1 REQUIRED libsecp256k1)
pkg_check_modules(ROCKSDB REQUIRED rocksdb)
find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)

project(Medorcoin
  VERSION 0.1.0
  DESCRIPTION "Medorcoin node and tools"
  LANGUAGES C CXX
)

# ---------------- Global options ----------------
option(ENABLE_WARNINGS "Enable extra compiler warnings" ON)
option(ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(ENABLE_UBSAN "Enable UBSanitizer" OFF)
option(ENABLE_COVERAGE "Enable coverage instrumentation (GCC/Clang)" OFF)

# Default build type
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
  set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release RelWithDebInfo MinSizeRel)
endif()

# Useful generator output
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

# Language standards
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Platform tweaks
if(MSVC)
  add_compile_options(/bigobj)
else()
  add_compile_options(-fPIC)
endif()

# Sanitizers / coverage (non-MSVC)
if(NOT MSVC)
  if(ENABLE_ASAN)
    add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address)
  endif()
  if(ENABLE_UBSAN)
    add_compile_options(-fsanitize=undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=undefined)
  endif()
  if(ENABLE_COVERAGE AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(--coverage -fprofile-arcs -ftest-coverage -O0)
    add_link_options(--coverage -fprofile-arcs -ftest-coverage)
  endif()
endif()

# Public headers path
include_directories(${CMAKE_SOURCE_DIR}/include)

# Dependencies discovered at root (linking done in subdirs)
find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(PkgConfig QUIET)

include(GNUInstallDirs)

# ---------------- Subdirectories ----------------
add_subdirectory(src)

# ---------------- Optional tests ----------------
if(EXISTS "${CMAKE_SOURCE_DIR}/tests/CMakeLists.txt")
  enable_testing()
  add_subdirectory(tests)
endif()

# ---------------- Install headers ----------------
if(EXISTS "${CMAKE_SOURCE_DIR}/include")
  install(DIRECTORY ${CMAKE_SOURCE_DIR}/include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
endif()

# ---------------- Self-update FOREVER ----------------
# WARNING: This runs an endless loop that periodically fetches, reconfigures, and rebuilds.
# Invoke explicitly: cmake --build build --target self_update_forever
# Stop with Ctrl+C or by killing the process in your supervisor.
find_program(GIT_EXECUTABLE git)
if(GIT_EXECUTABLE)
  set(SELF_UPDATE_BRANCH "main" CACHE STRING "Git branch to pull during self_update")
  set(SELF_UPDATE_INTERVAL_SECONDS "300" CACHE STRING "Seconds to wait between update cycles")

  # Script that loops forever
  set(SELF_UPDATE_SCRIPT "${CMAKE_BINARY_DIR}/self_update_forever.sh")
  file(WRITE "${SELF_UPDATE_SCRIPT}" "#!/usr/bin/env bash
set -euo pipefail
BRANCH=\"${SELF_UPDATE_BRANCH}\"
INTERVAL=\"${SELF_UPDATE_INTERVAL_SECONDS}\"
REPO_DIR=\"${CMAKE_SOURCE_DIR}\"
BUILD_DIR=\"${CMAKE_BINARY_DIR}\"

echo \"[self-update] Starting infinite updater on branch $BRANCH; interval ${SELF_UPDATE_INTERVAL_SECONDS}s\"
while true; do
  echo \"[self-update] Fetching updates...\"
  cd \"$REPO_DIR\"
  ${GIT_EXECUTABLE} fetch --tags origin || true
  ${GIT_EXECUTABLE} checkout \"$BRANCH\" || true
  ${GIT_EXECUTABLE} pull --ff-only origin \"$BRANCH\" || true

  echo \"[self-update] Re-configuring...\"
  cmake -S \"$REPO_DIR\" -B \"$BUILD_DIR\" -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}

  echo \"[self-update] Re-building...\"
  cmake --build \"$BUILD_DIR\" -j

  echo \"[self-update] Cycle complete. Sleeping ${SELF_UPDATE_INTERVAL_SECONDS}s...\"
  sleep \"$INTERVAL\"
done
")

  # Ensure script is executable
  file(COPY "${SELF_UPDATE_SCRIPT}" DESTINATION "${CMAKE_BINARY_DIR}"
       FILE_PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ WORLD_READ)

  add_custom_target(self_update_forever
    COMMAND ${CMAKE_COMMAND} -E echo
            "Launching self-update loop (branch=${SELF_UPDATE_BRANCH}, interval=${SELF_UPDATE_INTERVAL_SECONDS}s)..."
    COMMAND "${SELF_UPDATE_SCRIPT}"
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    USES_TERMINAL
    COMMENT "Run an infinite self-update loop: fetch -> reconfigure -> rebuild -> sleep -> repeat"
  )
else()
  message(STATUS "git not found; self_update_forever target will not be available.")
endif()

# ---------------- Packaging (basic) ----------------
set(CPACK_PACKAGE_NAME "medorcoin")
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
include(CPack)
