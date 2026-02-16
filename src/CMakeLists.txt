cmake_minimum_required(VERSION 3.16)

# Ensure we’re in the MedorCoin project context
project(MedorCoinNode LANGUAGES CXX)

# Enforce C++ standard locally as well (inherits from top-level but explicit here)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Collect sources in src/ only. Adjust patterns as needed.
# Add additional subfolders if you have more directories.
file(GLOB_RECURSE SOURCE_FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
)

# Define the executable
add_executable(medorcoin ${SOURCE_FILES})

# Include directories
target_include_directories(medorcoin
    PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/net
        ${CMAKE_SOURCE_DIR}/src/crypto
)

# Threads
find_package(Threads REQUIRED)
target_link_libraries(medorcoin PRIVATE Threads::Threads)

# OpenSSL
find_package(OpenSSL REQUIRED)
target_link_libraries(medorcoin PRIVATE OpenSSL::SSL OpenSSL::Crypto)

# RocksDB:
# Prefer a CMake package if available; otherwise try pkg-config; otherwise try a plain library fallback.
# 1) Try CMake package
set(_rocksdb_found FALSE)
find_package(RocksDB QUIET)
if(RocksDB_FOUND)
    target_link_libraries(medorcoin PRIVATE RocksDB::rocksdb)
    set(_rocksdb_found TRUE)
endif()

# 2) Try pkg-config if CMake package not found
if(NOT _rocksdb_found)
    find_package(PkgConfig QUIET)
    if(PKG_CONFIG_FOUND)
        pkg_check_modules(ROCKSDB QUIET rocksdb)
        if(ROCKSDB_FOUND)
            target_include_directories(medorcoin PRIVATE ${ROCKSDB_INCLUDE_DIRS})
            target_link_libraries(medorcoin PRIVATE ${ROCKSDB_LIBRARIES})
            set(_rocksdb_found TRUE)
        endif()
    endif()
endif()

# 3) Fallback: try to link with -lrocksdb directly (common on many distros)
if(NOT _rocksdb_found)
    include(CheckLibraryExists)
    check_library_exists(rocksdb rocksdb_open "" HAVE_LIB_ROCKSDB)
    if(HAVE_LIB_ROCKSDB)
        target_link_libraries(medorcoin PRIVATE rocksdb)
        set(_rocksdb_found TRUE)
    endif()
endif()

# If still not found, emit a clear error with guidance.
if(NOT _rocksdb_found)
    message(FATAL_ERROR
        "RocksDB not found.\n"
        "Tried: CMake package (RocksDBConfig.cmake), pkg-config (rocksdb), and -lrocksdb.\n"
        "Hints:\n"
        "  - Install rocksdb dev package (e.g., Ubuntu: sudo apt-get install -y librocksdb-dev)\n"
        "  - Or build from source and set CMAKE_PREFIX_PATH or ROCKSDB_ROOT to the install prefix.\n"
        "  - Or ensure pkg-config can find 'rocksdb' (PKG_CONFIG_PATH)."
    )
endif()

# secp256k1 via pkg-config
if(NOT PKG_CONFIG_FOUND)
    find_package(PkgConfig REQUIRED)
endif()
pkg_check_modules(SECP256K1 REQUIRED libsecp256k1)
target_include_directories(medorcoin PRIVATE ${SECP256K1_INCLUDE_DIRS})
target_link_libraries(medorcoin PRIVATE ${SECP256K1_LIBRARIES})

# Platform libraries commonly needed with RocksDB
# Many distros require these when linking RocksDB statically; harmless when dynamic.
include(CheckLibraryExists)
set(_maybe_libs z bz2 lz4 zstd snappy)
foreach(lib ${_maybe_libs})
    string(TOUPPER ${lib} LIB_UP)
    set(HAVE_VAR "HAVE_LIB_${LIB_UP}")
    check_library_exists(${lib} ${lib}_version "" ${HAVE_VAR})
    if(${HAVE_VAR})
        target_link_libraries(medorcoin PRIVATE ${lib})
    endif()
endforeach()
