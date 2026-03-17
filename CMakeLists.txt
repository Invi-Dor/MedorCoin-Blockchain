cmake_minimum_required(VERSION 3.19)

project(MedorCoin
VERSION 0.1.0
DESCRIPTION “MedorCoin blockchain node”
LANGUAGES C CXX
)

# ── Auto-install all required system dependencies ─────────────────────────────

# This block runs automatically during cmake configuration on any Ubuntu/Debian

# system. It ensures all libraries are present before the build begins.

find_program(APT_GET apt-get)
if(APT_GET)
message(STATUS “Ensuring all system dependencies are installed…”)
execute_process(
COMMAND sudo apt-get update -y
RESULT_VARIABLE APT_UPDATE_RESULT
)
execute_process(
COMMAND sudo apt-get install -y
build-essential
cmake
git
pkg-config
libssl-dev
libgflags-dev
libsnappy-dev
liblz4-dev
libzstd-dev
zlib1g-dev
libbz2-dev
libleveldb-dev
libsecp256k1-dev
libjemalloc-dev
liburing-dev
nlohmann-json3-dev
RESULT_VARIABLE APT_RESULT
)
if(NOT APT_RESULT EQUAL 0)
message(WARNING “One or more packages could not be installed via apt-get. “
“Build may fail if required libraries are missing.”)
endif()
execute_process(COMMAND sudo ldconfig)
message(STATUS “System dependencies installed successfully.”)
endif()

# ── Language standards ────────────────────────────────────────────────────────

set(CMAKE_C_STANDARD            11)
set(CMAKE_C_STANDARD_REQUIRED   ON)
set(CMAKE_CXX_STANDARD          20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS        OFF)

# ── Build type default ────────────────────────────────────────────────────────

if(NOT CMAKE_BUILD_TYPE)
set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING “Build type” FORCE)
set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
Debug Release RelWithDebInfo MinSizeRel)
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

# ── Build options ─────────────────────────────────────────────────────────────

option(ENABLE_WARNINGS  “Enable extra compiler warnings”  ON)
option(ENABLE_ASAN      “Enable AddressSanitizer”         OFF)
option(ENABLE_UBSAN     “Enable UBSanitizer”              OFF)
option(ENABLE_COVERAGE  “Enable coverage instrumentation” OFF)

# ── Platform flags ────────────────────────────────────────────────────────────

if(NOT MSVC)
add_compile_options(-fPIC)

```
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
```

else()
add_compile_options(/bigobj)
endif()

# ── Global include path ───────────────────────────────────────────────────────

include_directories(${CMAKE_SOURCE_DIR}/include)

# ── RocksDB configuration ─────────────────────────────────────────────────────

set(WITH_TESTS               OFF CACHE BOOL “” FORCE)
set(WITH_TOOLS               ON  CACHE BOOL “” FORCE)
set(WITH_BENCHMARK_TOOLS     OFF CACHE BOOL “” FORCE)
set(WITH_CORE_TOOLS          ON  CACHE BOOL “” FORCE)
set(WITH_GFLAGS              ON  CACHE BOOL “” FORCE)
set(WITH_SNAPPY              ON  CACHE BOOL “” FORCE)
set(WITH_LZ4                 ON  CACHE BOOL “” FORCE)
set(WITH_ZLIB                ON  CACHE BOOL “” FORCE)
set(WITH_ZSTD                ON  CACHE BOOL “” FORCE)
set(WITH_BZ2                 ON  CACHE BOOL “” FORCE)
set(WITH_JEMALLOC            ON  CACHE BOOL “” FORCE)
set(WITH_LIBURING            ON  CACHE BOOL “” FORCE)
set(ROCKSDB_BUILD_SHARED     OFF CACHE BOOL “” FORCE)
set(FAIL_ON_WARNINGS         OFF CACHE BOOL “” FORCE)
set(USE_RTTI                 ON  CACHE BOOL “” FORCE)
set(PORTABLE                 ON  CACHE BOOL “” FORCE)

# ── Core dependencies ─────────────────────────────────────────────────────────

find_package(Threads   REQUIRED)
find_package(OpenSSL   REQUIRED)
find_package(PkgConfig REQUIRED)

# ── secp256k1 ─────────────────────────────────────────────────────────────────

pkg_check_modules(SECP256K1 QUIET libsecp256k1)
if(NOT SECP256K1_FOUND)
find_library(SECP256K1_LIBRARIES secp256k1
PATHS /usr/lib/x86_64-linux-gnu /usr/local/lib
REQUIRED)
find_path(SECP256K1_INCLUDE_DIRS secp256k1.h
PATHS /usr/include /usr/local/include
REQUIRED)
message(STATUS “secp256k1 found via direct path: ${SECP256K1_LIBRARIES}”)
endif()

# ── LevelDB ───────────────────────────────────────────────────────────────────

# LevelDB on Ubuntu Noble does not ship a .pc file, so we locate it directly.

find_library(LEVELDB_LIBRARIES leveldb
PATHS /usr/lib/x86_64-linux-gnu /usr/local/lib
REQUIRED)
find_path(LEVELDB_INCLUDE_DIRS leveldb/db.h
PATHS /usr/include /usr/local/include
REQUIRED)
message(STATUS “LevelDB found: ${LEVELDB_LIBRARIES}”)

# ── nlohmann/json ─────────────────────────────────────────────────────────────

find_package(nlohmann_json 3.10 QUIET)
if(NOT nlohmann_json_FOUND)
message(STATUS “nlohmann_json not found via find_package, fetching via FetchContent…”)
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

if(EXISTS “${CMAKE_SOURCE_DIR}/include”)
install(DIRECTORY ${CMAKE_SOURCE_DIR}/include/
DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
endif()

# ── Install systemd service unit ──────────────────────────────────────────────

if(CMAKE_INSTALL_PREFIX STREQUAL “/usr” OR
CMAKE_INSTALL_PREFIX STREQUAL “/usr/local”)
if(EXISTS “${CMAKE_SOURCE_DIR}/medorcoin-blockchain.service”)
install(FILES ${CMAKE_SOURCE_DIR}/medorcoin-blockchain.service
DESTINATION ${CMAKE_INSTALL_PREFIX}/lib/systemd/system)
endif()
endif()

# ── Packaging ─────────────────────────────────────────────────────────────────

set(CPACK_PACKAGE_NAME        “MedorCoin”)
set(CPACK_PACKAGE_VERSION     ${PROJECT_VERSION})
set(CPACK_PACKAGE_DESCRIPTION “MedorCoin blockchain node”)
set(CPACK_PACKAGE_CONTACT     “medorcoin@example.com”)
include(CPack)
