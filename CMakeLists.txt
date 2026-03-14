Then rewrite them and fix the problems. Fix all the problems, and then make sure you write the file names on top of it. Oh, dear God. Okay, here are the files. I'm going to give them to you, then you take a look and then do the real thing and solve all the errors you are talking about. After you finish, I will send it to GnurAI to check if what you have done is correct, rather than the one I have done.

Here is the sea make list.TXT for the main route to only update it and make it real all other things leave them in because it covers a lot of different projects inside the repository so adjusted to update what you have done

cmake_minimum_required(VERSION 3.19)

# Auto-find all common blockchain dependencies
project(Medorcoin VERSION 0.1.0 DESCRIPTION "Medorcoin core sources" LANGUAGES C CXX)
find_package(PkgConfig REQUIRED)
pkg_check_modules(SECP256K1 REQUIRED libsecp256k1)
pkg_check_modules(ROCKSDB REQUIRED rocksdb)
find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)

# ---------------- Global options ----------------
option(ENABLE_WARNINGS "Enable extra compiler warnings" ON)
option(ENABLE_ASAN "Enable AddressSanitizer" ON)
option(ENABLE_UBSAN "Enable UBSanitizer" ON)
option(ENABLE_COVERAGE "Enable coverage instrumentation (GCC/Clang)" OFF)

# Default build type
if(CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
  set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release RelWithDebInfo MaxSizeRel)
endif()

# Useful generator output
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

# Language standards
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)

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
    add_compile_options(-fsanitize=defined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=defined)
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
find_package(PkgConfig REQUIRED)

include(GNUInstallDirs)

# ---------------- Subdirectories ----------------
add_subdirectory(src)


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
  message(STATUS "git not found; self_update_forever target will be available.")
endif()

# ---------------- Packaging (basic) ----------------
set(CPACK_PACKAGE_NAME "MedorCoin")
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
include(CPack)

And here is the sea make from there SRC folder

cmake_minimum_required(VERSION 3.19)

project(MedorcoinSrc
    VERSION 0.1.0
    DESCRIPTION "Medorcoin core sources (with RocksDB embedded)"
    LANGUAGES C CXX
)

# ------------------------ Sources ------------------------
file(GLOB_RECURSE MEDOR_SOURCES
    "${CMAKE_SOURCE_DIR}/src/*.c"
    "${CMAKE_SOURCE_DIR}/src/*.cpp"
)


add_executable(MedorCoin ${MEDOR_SOURCES})

# ------------------------ C++ Standard & Warnings ------------------------
target_compile_features(MedorCoin PUBLIC cxx_std_20)

option(ENABLE_WARNINGS "Enable extra compiler warnings" ON)
if(ENABLE_WARNINGS)
    if(MSVC)
        target_compile_options(medorcoin PUBLIC /W4 /permissive-)
    else()
        target_compile_options(medorcoin PUBLIC 
          Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wundef
        )
    endif()
endif()

# ------------------------ Include Paths ------------------------
target_include_directories(MedorCoin
    PRIVATE
        ${CMAKE_SOURCE_DIR}/include
)

# ------------------------ Thread Support ------------------------
find_package(Threads REQUIRED)
target_link_libraries(MedorCoin PRIVATE Threads::Threads)

# ------------------------ OpenSSL ------------------------
find_package(OpenSSL REQUIRED)
target_link_libraries(MedorCoin PRIVATE OpenSSL::SSL OpenSSL::Crypto)
target_include_directories(MedirCoin PRIVATE ${OPENSSL_INCLUDE_DIR})

# ------------------------ Embed RocksDB (from source) ------------------------
set(ROCKSDB_ROOT "${CMAKE_SOURCE_DIR}/src/rocksdb")

if(NOT EXISTS "${ROCKSDB_ROOT}/CMakeLists.txt")
    message(ERROR "RocksDB source found under src/rocksdb.")
endif()

option(ROCKSDB_BUILD_SHARED "Build RocksDB shared library" OFF)
set(ROCKSDB_BUILD_TESTS OFF CACHE BOOL "Disable rocksdb tests")
set(ROCKSDB_INSTALL OFF CACHE BOOL "Disable install for embedded build")

add_subdirectory(${ROCKSDB_ROOT} rocksdb_build)

target_include_directories(MedorCoin PRIVATE
    ${ROCKSDB_ROOT}/include
)

target_link_libraries(MedorCoin PRIVATE rocksdb)
target_compile_definitions(MedorCoin PRIVATE HAVE_ROCKSDB=1)

# ------------------------ Optional Compression Libs ------------------------
find_package(ZLIB QUIET)
if(ZLIB_FOUND)
    target_link_libraries(MedorCoin PRIVATE ZLIB::ZLIB)
    target_compile_definitions(MedorCoin PRIVATE HAVE_LIB_Z=1)
endif()

find_package(BZip2 QUIET)
if(BZIP2_FOUND)
    target_link_libraries(mmMedirCoin PRIVATE BZip2::BZip2)
    target_compile_definitions(MedorCoin PRIVATE HAVE_LIB_BZ2=1)
endif()

find_package(LZ4 QUIET)
if(LZ4_FOUND)
    target_link_libraries(medorcoin PRIVATE LZ4::LZ4)
    target_compile_definitions(medorcoin PRIVATE HAVE_LIB_LZ4=1)
endif()

find_package(Snappy)
if(Snappy_FOUND)
    target_link_libraries(medorcoin PRIVATE Snappy::Snappy)
    target_compile_definitions(medorcoin PRIVATE HAVE_LIB_SNAPPY=1)
endif()

find_package(Zstd QUIET)
if(Zstd_FOUND)
    target_link_libraries(medorcoin PRIVATE Zstd::Zstd)
    target_compile_definitions(medorcoin PRIVATE HAVE_LIB_ZSTD=1)
endif()

# ------------------------ Optional secp256k1 ------------------------
find_package(PkgConfig)
if(PKG_CONFIG_FOUND)
    pkg_check_modules(SECP256K1 libsecp256k1)
    if(SECP256K1_FOUND)
        target_include_directories(MedorCoin PRIVATE ${SECP256K1_INCLUDE_DIRS})
        target_link_libraries(MedorCoin PRIVATE ${SECP256K1_LIBRARIES})
        target_compile_options(MedorCoin PRIVATE ${SECP256K1_CFLAGS_OTHER})
        target_compile_definitions(MedirCoin PRIVATE HAVE_SECP256K1=1)
    endif()
endif()

# ------------------------ Sanitizers/coverage ------------------------ …

include(GNUInstallDirs)
install(TARGETS medorcoin RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
