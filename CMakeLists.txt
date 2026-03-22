cmake_minimum_required(VERSION 3.19)

project(MedorCoin
    VERSION 0.1.0
    DESCRIPTION "MedorCoin blockchain node"
    LANGUAGES C CXX
)

# =============================================================================
# AUTO-INSTALL SYSTEM DEPENDENCIES
# =============================================================================
find_program(APT_GET apt-get)
if(APT_GET)
    message(STATUS "Ensuring all system dependencies are installed...")
    execute_process(COMMAND sudo apt-get update -y)
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
            nlohmann-json3-dev
        RESULT_VARIABLE APT_RESULT
    )
    if(NOT APT_RESULT EQUAL 0)
        message(WARNING "One or more packages could not be installed.")
    endif()
    execute_process(COMMAND sudo ldconfig)
    message(STATUS "System dependencies ready.")
endif()

# =============================================================================
# LANGUAGE STANDARDS
# =============================================================================
set(CMAKE_C_STANDARD            11)
set(CMAKE_C_STANDARD_REQUIRED   ON)
set(CMAKE_CXX_STANDARD          20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS        OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
        Debug Release RelWithDebInfo MinSizeRel)
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

# =============================================================================
# BUILD OPTIONS
# =============================================================================
option(ENABLE_WARNINGS  "Enable extra compiler warnings"  ON)
option(ENABLE_ASAN      "Enable AddressSanitizer"         OFF)
option(ENABLE_UBSAN     "Enable UBSanitizer"              OFF)
option(ENABLE_COVERAGE  "Enable coverage instrumentation" OFF)

if(NOT MSVC)
    add_compile_options(-fPIC)
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
        add_link_options(--coverage)
    endif()
else()
    add_compile_options(/bigobj)
endif()

# =============================================================================
# GLOBAL INCLUDE
# =============================================================================
include_directories(${CMAKE_SOURCE_DIR}/include)

# =============================================================================
# HELP CMAKE FIND INSTALLED LIBRARIES
# Codespaces installs to /usr/lib/x86_64-linux-gnu but RocksDB CMake
# modules look in non-standard paths. Point them explicitly.
# =============================================================================
set(CMAKE_PREFIX_PATH
    /usr/lib/x86_64-linux-gnu
    /usr/include
    /usr/local/lib
    /usr/local/include
    ${CMAKE_PREFIX_PATH}
)

# Snappy
find_library(Snappy_LIBRARIES snappy
    PATHS /usr/lib/x86_64-linux-gnu /usr/local/lib)
find_path(Snappy_INCLUDE_DIRS snappy.h
    PATHS /usr/include /usr/local/include)
if(Snappy_LIBRARIES AND Snappy_INCLUDE_DIRS)
    set(Snappy_FOUND TRUE)
    message(STATUS "Snappy found: ${Snappy_LIBRARIES}")
else()
    message(WARNING "Snappy not found -- disabling")
    set(Snappy_FOUND FALSE)
endif()

# LZ4
find_library(LZ4_LIBRARIES lz4
    PATHS /usr/lib/x86_64-linux-gnu /usr/local/lib)
find_path(LZ4_INCLUDE_DIRS lz4.h
    PATHS /usr/include /usr/local/include)
if(LZ4_LIBRARIES AND LZ4_INCLUDE_DIRS)
    set(LZ4_FOUND TRUE)
    message(STATUS "LZ4 found: ${LZ4_LIBRARIES}")
else()
    message(WARNING "LZ4 not found -- disabling")
    set(LZ4_FOUND FALSE)
endif()

# Zstd
find_library(zstd_LIBRARIES zstd
    PATHS /usr/lib/x86_64-linux-gnu /usr/local/lib)
find_path(zstd_INCLUDE_DIRS zstd.h
    PATHS /usr/include /usr/local/include)
if(zstd_LIBRARIES AND zstd_INCLUDE_DIRS)
    set(zstd_FOUND TRUE)
    message(STATUS "Zstd found: ${zstd_LIBRARIES}")
else()
    message(WARNING "Zstd not found -- disabling")
    set(zstd_FOUND FALSE)
endif()

# BZ2
find_library(BZIP2_LIBRARIES bz2
    PATHS /usr/lib/x86_64-linux-gnu /usr/local/lib)
find_path(BZIP2_INCLUDE_DIRS bzlib.h
    PATHS /usr/include /usr/local/include)
if(BZIP2_LIBRARIES AND BZIP2_INCLUDE_DIRS)
    set(BZIP2_FOUND TRUE)
    message(STATUS "BZip2 found: ${BZIP2_LIBRARIES}")
else()
    message(WARNING "BZip2 not found -- disabling")
    set(BZIP2_FOUND FALSE)
endif()

# gflags
find_library(GFLAGS_LIBRARIES gflags
    PATHS /usr/lib/x86_64-linux-gnu /usr/local/lib)
find_path(GFLAGS_INCLUDE_DIR gflags/gflags.h
    PATHS /usr/include /usr/local/include)
if(GFLAGS_LIBRARIES AND GFLAGS_INCLUDE_DIR)
    set(GFLAGS_FOUND TRUE)
    message(STATUS "gflags found: ${GFLAGS_LIBRARIES}")
else()
    message(WARNING "gflags not found -- disabling")
    set(GFLAGS_FOUND FALSE)
endif()

# jemalloc
find_library(JEMALLOC_LIBRARIES jemalloc
    PATHS /usr/lib/x86_64-linux-gnu /usr/local/lib)
find_path(JEMALLOC_INCLUDE_DIRS jemalloc/jemalloc.h
    PATHS /usr/include /usr/local/include)
if(JEMALLOC_LIBRARIES AND JEMALLOC_INCLUDE_DIRS)
    set(JEMALLOC_FOUND TRUE)
    message(STATUS "jemalloc found: ${JEMALLOC_LIBRARIES}")
else()
    message(WARNING "jemalloc not found -- disabling")
    set(JEMALLOC_FOUND FALSE)
endif()

# =============================================================================
# ROCKSDB OPTIONS
# Use found status to enable/disable each compression library
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
set(JEMALLOC_FOUND TRUE) ${JEMALLOC_FOUND}CACHE BOOL "" FORCE)
set(WITH_LIBURING        OFF              CACHE BOOL "" FORCE)
set(ROCKSDB_BUILD_SHARED OFF              CACHE BOOL "" FORCE)
set(FAIL_ON_WARNINGS     OFF              CACHE BOOL "" FORCE)
set(USE_RTTI             ON               CACHE BOOL "" FORCE)
set(PORTABLE             ON               CACHE BOOL "" FORCE)

# =============================================================================
# FIND PACKAGES
# =============================================================================
find_package(Threads   REQUIRED)
find_package(OpenSSL   REQUIRED)
find_package(PkgConfig REQUIRED)

find_library(SECP256K1_LIB secp256k1
    PATHS /usr/lib/x86_64-linux-gnu /usr/local/lib)
find_path(SECP256K1_INCLUDE secp256k1.h
    PATHS /usr/include /usr/local/include)
if(NOT SECP256K1_LIB OR NOT SECP256K1_INCLUDE)
    message(FATAL_ERROR
        "secp256k1 not found. Run: sudo apt-get install -y libsecp256k1-dev")
endif()
set(SECP256K1_LIBRARIES    ${SECP256K1_LIB})
set(SECP256K1_INCLUDE_DIRS ${SECP256K1_INCLUDE})

find_library(LEVELDB_LIB leveldb
    PATHS /usr/lib/x86_64-linux-gnu /usr/local/lib)
find_path(LEVELDB_INCLUDE leveldb/db.h
    PATHS /usr/include /usr/local/include)
if(NOT LEVELDB_LIB OR NOT LEVELDB_INCLUDE)
    message(FATAL_ERROR
        "LevelDB not found. Run: sudo apt-get install -y libleveldb-dev")
endif()
set(LEVELDB_LIBRARIES    ${LEVELDB_LIB})
set(LEVELDB_INCLUDE_DIRS ${LEVELDB_INCLUDE})

find_package(nlohmann_json 3.10 QUIET)
if(NOT nlohmann_json_FOUND)
    include(FetchContent)
    FetchContent_Declare(nlohmann_json
        GIT_REPOSITORY https://github.com/nlohmann/json.git
        GIT_TAG        v3.11.3
    )
    FetchContent_MakeAvailable(nlohmann_json)
endif()

# =============================================================================
# SUBDIRECTORY
# =============================================================================
include(GNUInstallDirs)
add_subdirectory(src)

# =============================================================================
# INSTALL HEADERS
# =============================================================================
if(EXISTS "${CMAKE_SOURCE_DIR}/include")
    install(DIRECTORY ${CMAKE_SOURCE_DIR}/include/
            DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
endif()

if(CMAKE_INSTALL_PREFIX STREQUAL "/usr" OR
   CMAKE_INSTALL_PREFIX STREQUAL "/usr/local")
    if(EXISTS "${CMAKE_SOURCE_DIR}/medorcoin-blockchain.service")
        install(FILES ${CMAKE_SOURCE_DIR}/medorcoin-blockchain.service
                DESTINATION ${CMAKE_INSTALL_PREFIX}/lib/systemd/system)
    endif()
endif()

# =============================================================================
# CPACK
# =============================================================================
set(CPACK_PACKAGE_NAME        "MedorCoin")
set(CPACK_PACKAGE_VERSION     ${PROJECT_VERSION})
set(CPACK_PACKAGE_DESCRIPTION "MedorCoin blockchain node")
set(CPACK_PACKAGE_CONTACT     "medorcoin@example.com")
include(CPack)
