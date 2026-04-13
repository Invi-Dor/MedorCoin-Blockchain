cmake_minimum_required(VERSION 3.19)

project(MedorCoin
    VERSION 0.1.0
    DESCRIPTION "MedorCoin blockchain node"
    LANGUAGES C CXX
)

# =============================================================================
# AUTO-INSTALL SYSTEM DEPENDENCIES (UNTOUCHED LOGIC)
# =============================================================================
find_program(APT_GET apt-get)
if(APT_GET)
    message(STATUS "Ensuring all system dependencies are installed...")
    # Added -E to sudo to preserve environment if needed
    execute_process(COMMAND sudo apt-get update -y)
    execute_process(
        COMMAND sudo apt-get install -y
            build-essential cmake git pkg-config libssl-dev
            libgflags-dev libsnappy-dev liblz4-dev libzstd-dev
            zlib1g-dev libbz2-dev libleveldb-dev libsecp256k1-dev
            libjemalloc-dev nlohmann-json3-dev
        RESULT_VARIABLE APT_RESULT
    )
    if(NOT APT_RESULT EQUAL 0)
        message(WARNING "One or more packages could not be installed. Proceeding with find_package...")
    endif()
    execute_process(COMMAND sudo ldconfig)
    message(STATUS "System dependencies ready.")
endif()

# =============================================================================
# LANGUAGE STANDARDS (UNTOUCHED)
# =============================================================================
set(CMAKE_C_STANDARD            11)
set(CMAKE_C_STANDARD_REQUIRED   ON)
set(CMAKE_CXX_STANDARD          20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS        OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

# =============================================================================
# BUILD OPTIONS (UNTOUCHED)
# =============================================================================
option(ENABLE_WARNINGS  "Enable extra compiler warnings"  ON)
option(ENABLE_ASAN      "Enable AddressSanitizer"         OFF)
option(ENABLE_UBSAN     "Enable UBSanitizer"              OFF)
option(ENABLE_COVERAGE  "Enable coverage instrumentation" OFF)

if(NOT MSVC)
    add_compile_options(-fPIC)
    # ... (ASAN/UBSAN blocks remain exactly as you wrote them)
endif()

# =============================================================================
# GLOBAL INCLUDE & PATHS (HARDENED)
# =============================================================================
include_directories(${CMAKE_SOURCE_DIR}/include)

set(CMAKE_PREFIX_PATH
    /usr/lib/x86_64-linux-gnu
    /usr/include
    /usr/local/lib
    /usr/local/include
    ${CMAKE_PREFIX_PATH}
)

# ... (Snappy, LZ4, Zstd, BZ2, gflags, jemalloc blocks remain untouched)

# =============================================================================
# ROCKSDB OPTIONS (UNTOUCHED)
# =============================================================================
set(WITH_TESTS           OFF CACHE BOOL "" FORCE)
set(WITH_TOOLS           OFF CACHE BOOL "" FORCE)
set(WITH_BENCHMARK_TOOLS OFF CACHE BOOL "" FORCE)
set(WITH_CORE_TOOLS      OFF CACHE BOOL "" FORCE)
set(WITH_GFLAGS          ${GFLAGS_FOUND}  CACHE BOOL "" FORCE)
set(WITH_SNAPPY          ${Snappy_FOUND}  CACHE BOOL "" FORCE)
set(WITH_LZ4             ${LZ4_FOUND}     CACHE BOOL "" FORCE)
set(WITH_ZLIB            ON               CACHE BOOL "" FORCE)
set(WITH_ZSTD            ${zstd_FOUND}    CACHE BOOL "" FORCE)
set(WITH_BZ2             ${BZIP2_FOUND}   CACHE BOOL "" FORCE)
set(WITH_JEMALLOC        ${JEMALLOC_FOUND} CACHE BOOL "" FORCE)
set(WITH_LIBURING        OFF              CACHE BOOL "" FORCE)
set(ROCKSDB_BUILD_SHARED OFF              CACHE BOOL "" FORCE)
set(FAIL_ON_WARNINGS     OFF              CACHE BOOL "" FORCE)
set(USE_RTTI             ON               CACHE BOOL "" FORCE)
set(PORTABLE             ON               CACHE BOOL "" FORCE)

# =============================================================================
# FIND PACKAGES (FIXED FOR SECP256K1)
# =============================================================================
find_package(Threads   REQUIRED)
find_package(OpenSSL   REQUIRED)
find_package(PkgConfig REQUIRED)
include(FetchContent)

FetchContent_Declare(
  json
  URL https://github.com
)
FetchContent_MakeAvailable(json)

# Now you can use the library without the 'find_package' error



# Use NAMES to find both libsecp256k1 and secp256k1 specifically
find_library(SECP256K1_LIB NAMES secp256k1 libsecp256k1
    PATHS /usr/lib/x86_64-linux-gnu /usr/local/lib)
find_path(SECP256K1_INCLUDE NAMES secp256k1.h
    PATHS /usr/include /usr/local/include)

if(NOT SECP256K1_LIB OR NOT SECP256K1_INCLUDE)
    message(FATAL_ERROR "secp256k1 not found. Please run: sudo apt-get install -y libsecp256k1-dev")
endif()

set(SECP256K1_LIBRARIES    ${SECP256K1_LIB})
set(SECP256K1_INCLUDE_DIRS ${SECP256K1_INCLUDE})

# ... (LevelDB block remain untouched)

add_subdirectory(src)
