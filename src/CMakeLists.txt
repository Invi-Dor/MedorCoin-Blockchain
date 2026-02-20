cmake_minimum_required(VERSION 3.19)

project(MedorcoinSrc
  VERSION 0.1.0
  DESCRIPTION "Medorcoin core sources"
  LANGUAGES C CXX
)

---------------- C++ standard and warnings ----------------
target_compile_features(medorcoin PRIVATE cxx_std_20) # will be applied after target is created
option(ENABLE_WARNINGS "Enable extra compiler warnings" ON)

---------------- Sources ----------------
file(GLOB_RECURSE SOURCE_FILES
  "${CMAKE_CURRENT_SOURCE_DIR}/.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/.cpp"
)

if(NOT SOURCE_FILES)
  message(FATAL_ERROR "No source files found in ${CMAKE_CURRENT_SOURCE_DIR}")
endif()

---------------- Target ----------------
add_executable(medorcoin ${SOURCE_FILES})

---------------- Include paths ----------------
target_include_directories(medorcoin
  PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/net
    ${CMAKE_SOURCE_DIR}/src/crypto
    ${CMAKE_SOURCE_DIR}/src/db
    ${CMAKE_SOURCE_DIR}/src/evm
)

---------------- C++ standard and warnings (apply now) ----------------
if(ENABLE_WARNINGS)
  if(MSVC)
    target_compile_options(medorcoin PRIVATE /W4 /permissive-)
  else()
    target_compile_options(medorcoin PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wundef)
  endif()
endif()

---------------- PIC ----------------
set_property(TARGET medorcoin PROPERTY POSITION_INDEPENDENT_CODE ON)

---------------- Threads ----------------
find_package(Threads REQUIRED)
target_link_libraries(medorcoin PRIVATE Threads::Threads)

---------------- OpenSSL ----------------
find_package(OpenSSL REQUIRED)
target_link_libraries(medorcoin PRIVATE OpenSSL::SSL OpenSSL::Crypto)

---------------- RocksDB (try CMake, then pkg-config, then -lrocksdb) ----------------
set(_rocksdb_found FALSE)
find_package(RocksDB QUIET)
if(RocksDB_FOUND)
  target_link_libraries(medorcoin PRIVATE RocksDB::rocksdb)
  set(_rocksdb_found TRUE)
endif()

if(NOT _rocksdb_found)
  find_package(PkgConfig QUIET)
  if(PKG_CONFIG_FOUND)
    pkg_check_modules(ROCKSDB QUIET rocksdb)
    if(ROCKSDB_FOUND)
      target_include_directories(medorcoin PRIVATE ${ROCKSDB_INCLUDE_DIRS})
      target_link_libraries(medorcoin PRIVATE ${ROCKSDB_LIBRARIES})
      target_compile_options(medorcoin PRIVATE ${ROCKSDB_CFLAGS_OTHER})
      set(_rocksdb_found TRUE)
    endif()
  endif()
endif()

if(NOT _rocksdb_found)
  include(CheckLibraryExists)
  check_library_exists(rocksdb rocksdb_open "" HAVE_LIB_ROCKSDB)
  if(HAVE_LIB_ROCKSDB)
    target_link_libraries(medorcoin PRIVATE rocksdb)
    set(_rocksdb_found TRUE)
  endif()
endif()

if(NOT _rocksdb_found)
  message(FATAL_ERROR
    "RocksDB not found.\n"
    "Install librocksdb-dev (Debian/Ubuntu), rocksdb-devel (Fedora), or brew install rocksdb.\n"
    "Or set CMAKE_PREFIX_PATH/ROCKSDB_ROOT or PKG_CONFIG_PATH."
  )
endif()

---------------- secp256k1 via pkg-config ----------------
find_package(PkgConfig REQUIRED)
pkg_check_modules(SECP256K1 REQUIRED libsecp256k1)
target_include_directories(medorcoin PRIVATE ${SECP256K1_INCLUDE_DIRS})
target_link_libraries(medorcoin PRIVATE ${SECP256K1_LIBRARIES})
target_compile_options(medorcoin PRIVATE ${SECP256K1_CFLAGS_OTHER})

---------------- Optional compression backends ----------------
include(CheckLibraryExists)
set(_maybe_libs z bz2 lz4 zstd snappy)
foreach(lib IN LISTS _maybe_libs)
  string(TOUPPER "${lib}" LIB_UP)
  set(HAVE_VAR "HAVE_LIB_${LIB_UP}")
  if(lib STREQUAL "z")
    check_library_exists(${lib} inflate "" ${HAVE_VAR})
  elseif(lib STREQUAL "bz2")
    check_library_exists(${lib} BZ2_bzCompress "" ${HAVE_VAR})
  elseif(lib STREQUAL "lz4")
    check_library_exists(${lib} LZ4_createStream "" ${HAVE_VAR})
  elseif(lib STREQUAL "zstd")
    check_library_exists(${lib} ZSTD_createCCtx "" ${HAVE_VAR})
  elseif(lib STREQUAL "snappy")
    check_library_exists(${lib} snappy_compress "" ${HAVE_VAR})
  else()
    check_library_exists(${lib} ${lib}_version "" ${HAVE_VAR})
  endif()
  if(${HAVE_VAR})
    target_link_libraries(medorcoin PRIVATE ${lib})
    target_compile_definitions(medorcoin PRIVATE "HAVE_LIB_${LIB_UP}=1")
  endif()
endforeach()

---------------- Feature macros ----------------
if(RocksDB_FOUND OR HAVE_LIB_ROCKSDB)
  target_compile_definitions(medorcoin PRIVATE HAVE_ROCKSDB=1)
endif()
if(SECP256K1_FOUND)
  target_compile_definitions(medorcoin PRIVATE HAVE_SECP256K1=1)
endif()

---------------- Sanitizers (optional) ----------------
option(ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(ENABLE_UBSAN "Enable UBSanitizer" OFF)
if(NOT MSVC)
  if(ENABLE_ASAN)
    target_compile_options(medorcoin PRIVATE -fsanitize=address -fno-omit-frame-pointer)
    target_link_options(medorcoin PRIVATE -fsanitize=address)
  endif()
  if(ENABLE_UBSAN)
    target_compile_options(medorcoin PRIVATE -fsanitize=undefined -fno-omit-frame-pointer)
    target_link_options(medorcoin PRIVATE -fsanitize=undefined)
  endif()
endif()

---------------- Coverage (GCC/Clang) ----------------
option(ENABLE_COVERAGE "Enable coverage flags" OFF)
if(ENABLE_COVERAGE AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(medorcoin PRIVATE -O0 --coverage -fprofile-arcs -ftest-coverage)
  target_link_options(medorcoin PRIVATE --coverage -fprofile-arcs -ftest-coverage)
endif()

---------------- Install ----------------
include(GNUInstallDirs)
install(TARGETS medorcoin RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

---------------- Self-update FOREVER (src-level) ----------------
Infinite loop that fetches root repo, reconfigures at root, and rebuilds.
Invoke: cmake --build build --target src_self_update_forever
find_program(GIT_EXECUTABLE git)
if(GIT_EXECUTABLE)
  set(SELF_UPDATE_BRANCH "main" CACHE STRING "Git branch to pull during self_update (src)")
  set(SELF_UPDATE_INTERVAL_SECONDS "300" CACHE STRING "Seconds to wait between update cycles (src)")

  set(SRC_SELF_UPDATE_SCRIPT "${CMAKE_BINARY_DIR}/src_self_update_forever.sh")
  file(WRITE "${SRC_SELF_UPDATE_SCRIPT}" "#!/usr/bin/env bash
set -euo pipefail
BRANCH=\"${SELF_UPDATE_BRANCH}\"
INTERVAL=\"${SELF_UPDATE_INTERVAL_SECONDS}\"
REPO_DIR=\"${CMAKE_SOURCE_DIR}\"
BUILD_DIR=\"${CMAKE_BINARY_DIR}\"

echo \"[src self-update] Starting infinite updater on branch $BRANCH; interval ${SELF_UPDATE_INTERVAL_SECONDS}s\"
while true; do
  echo \"[src self-update] Fetching updates...\"
  cd \"$REPO_DIR\"
  ${GIT_EXECUTABLE} fetch --tags origin || true
  ${GIT_EXECUTABLE} checkout \"$BRANCH\" || true
  ${GIT_EXECUTABLE} pull --ff-only origin \"$BRANCH\" || true

  echo \"[src self-update] Re-configuring (root)...\"
  cmake -S \"$REPO_DIR\" -B \"$BUILD_DIR\" -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}

  echo \"[src self-update] Re-building (root)...\"
  cmake --build \"$BUILD_DIR\" -j

  echo \"[src self-update] Cycle complete. Sleeping ${SELF_UPDATE_INTERVAL_SECONDS}s...\"
  sleep \"$INTERVAL\"
done
")

  file(COPY "${SRC_SELF_UPDATE_SCRIPT}" DESTINATION "${CMAKE_BINARY_DIR}" FILE_PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ WORLD_READ)

  add_custom_target(src_self_update_forever
    COMMAND ${CMAKE_COMMAND} -E echo "Launching src self-update loop (branch=${SELF_UPDATE_BRANCH}, interval=${SELF_UPDATE_INTERVAL_SECONDS}s)..."
    COMMAND "${SRC_SELF_UPDATE_SCRIPT}"
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    USES_TERMINAL
    COMMENT "Infinite self-update loop from src: fetch -> reconfigure (root) -> rebuild -> sleep -> repeat"
  )
else()
  message(STATUS "git not found; src_self_update_forever target will not be available.")
endif()
