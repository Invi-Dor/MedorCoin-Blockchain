cmake_minimum_required(VERSION 3.19)

# ── RocksDB embedded submodule ────────────────────────────────────────────────

set(ROCKSDB_ROOT “${CMAKE_SOURCE_DIR}/src/rocksdb”)

if(NOT EXISTS “${ROCKSDB_ROOT}/CMakeLists.txt”)
message(FATAL_ERROR
“RocksDB source not found at ${ROCKSDB_ROOT}.\n”
“Initialise the submodule with:\n”
“  git submodule update –init –recursive”)
endif()

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

add_subdirectory(${ROCKSDB_ROOT} rocksdb_build EXCLUDE_FROM_ALL)

# ═════════════════════════════════════════════════════════════════════════════

# medor_link_common — applied to every target in this file

# ═════════════════════════════════════════════════════════════════════════════

macro(medor_link_common TARGET)

```
target_compile_features(${TARGET} PUBLIC cxx_std_20)

if(ENABLE_WARNINGS)
    if(MSVC)
        target_compile_options(${TARGET} PRIVATE /W4 /permissive-)
    else()
        target_compile_options(${TARGET} PRIVATE
            -Wall -Wextra -Wpedantic
            -Wconversion -Wshadow -Wundef
            -Wno-unused-parameter
        )
    endif()
endif()

target_include_directories(${TARGET} PUBLIC
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/abi
    ${CMAKE_SOURCE_DIR}/src/consensus
    ${CMAKE_SOURCE_DIR}/src/crypto
    ${CMAKE_SOURCE_DIR}/src/db
    ${CMAKE_SOURCE_DIR}/src/evm
    ${CMAKE_SOURCE_DIR}/src/fees
    ${CMAKE_SOURCE_DIR}/src/net
    ${CMAKE_SOURCE_DIR}/src/node
    ${OPENSSL_INCLUDE_DIR}
    ${ROCKSDB_ROOT}/include
    ${LEVELDB_INCLUDE_DIRS}
    ${SECP256K1_INCLUDE_DIRS}
)

target_link_libraries(${TARGET} PUBLIC
    Threads::Threads
    OpenSSL::SSL
    OpenSSL::Crypto
    rocksdb
    ${LEVELDB_LIBRARIES}
    ${SECP256K1_LIBRARIES}
    nlohmann_json::nlohmann_json
)

target_compile_options(${TARGET} PRIVATE
    ${LEVELDB_CFLAGS_OTHER}
    ${SECP256K1_CFLAGS_OTHER}
)

target_compile_definitions(${TARGET} PUBLIC
    HAVE_ROCKSDB=1
    HAVE_SECP256K1=1
)

# ── PkgConfig required for all module lookups ─────────────────────────────
find_package(PkgConfig REQUIRED)

# ── ZLIB ─────────────────────────────────────────────────────────────────
find_package(ZLIB QUIET)
if(ZLIB_FOUND)
    target_link_libraries(${TARGET} PUBLIC ZLIB::ZLIB)
    target_compile_definitions(${TARGET} PUBLIC HAVE_LIB_Z=1)
endif()

# ── BZip2 ────────────────────────────────────────────────────────────────
find_package(BZip2 QUIET)
if(BZIP2_FOUND)
    target_link_libraries(${TARGET} PUBLIC BZip2::BZip2)
    target_compile_definitions(${TARGET} PUBLIC HAVE_LIB_BZ2=1)
endif()

# ── LZ4 ──────────────────────────────────────────────────────────────────
pkg_check_modules(LZ4 QUIET liblz4)
if(LZ4_FOUND)
    target_include_directories(${TARGET} PUBLIC ${LZ4_INCLUDE_DIRS})
    target_link_libraries(${TARGET} PUBLIC ${LZ4_LIBRARIES})
    target_compile_definitions(${TARGET} PUBLIC HAVE_LIB_LZ4=1)
endif()

# ── Snappy (via pkg-config — works without CMake config files) ───────────
pkg_check_modules(SNAPPY QUIET snappy)
if(SNAPPY_FOUND)
    target_include_directories(${TARGET} PUBLIC ${SNAPPY_INCLUDE_DIRS})
    target_link_libraries(${TARGET} PUBLIC ${SNAPPY_LIBRARIES})
    target_compile_definitions(${TARGET} PUBLIC HAVE_LIB_SNAPPY=1)
else()
    # Fallback: locate library and headers directly
    find_library(SNAPPY_LIB snappy)
    find_path(SNAPPY_INCLUDE snappy.h)
    if(SNAPPY_LIB AND SNAPPY_INCLUDE)
        target_include_directories(${TARGET} PUBLIC ${SNAPPY_INCLUDE})
        target_link_libraries(${TARGET} PUBLIC ${SNAPPY_LIB})
        target_compile_definitions(${TARGET} PUBLIC HAVE_LIB_SNAPPY=1)
        message(STATUS "Snappy found via direct path: ${SNAPPY_LIB}")
    else()
        message(WARNING "Snappy not found — RocksDB Snappy compression disabled")
    endif()
endif()

# ── ZSTD ─────────────────────────────────────────────────────────────────
pkg_check_modules(ZSTD QUIET libzstd)
if(ZSTD_FOUND)
    target_include_directories(${TARGET} PUBLIC ${ZSTD_INCLUDE_DIRS})
    target_link_libraries(${TARGET} PUBLIC ${ZSTD_LIBRARIES})
    target_compile_definitions(${TARGET} PUBLIC HAVE_LIB_ZSTD=1)
endif()

# ── EVMC ─────────────────────────────────────────────────────────────────
find_package(evmc QUIET)
if(evmc_FOUND)
    target_link_libraries(${TARGET} PUBLIC evmc::evmc)
    target_compile_definitions(${TARGET} PUBLIC HAVE_EVMC=1)
endif()

# ── jemalloc ─────────────────────────────────────────────────────────────
pkg_check_modules(JEMALLOC QUIET jemalloc)
if(JEMALLOC_FOUND)
    target_include_directories(${TARGET} PUBLIC ${JEMALLOC_INCLUDE_DIRS})
    target_link_libraries(${TARGET} PUBLIC ${JEMALLOC_LIBRARIES})
    target_compile_definitions(${TARGET} PUBLIC HAVE_JEMALLOC=1)
endif()

# ── Sanitiser and coverage link flags ─────────────────────────────────────
if(NOT MSVC)
    if(ENABLE_ASAN)
        target_compile_options(${TARGET} PRIVATE
            -fsanitize=address -fno-omit-frame-pointer)
        target_link_options(${TARGET} PRIVATE -fsanitize=address)
    endif()
    if(ENABLE_UBSAN)
        target_compile_options(${TARGET} PRIVATE
            -fsanitize=undefined -fno-omit-frame-pointer)
        target_link_options(${TARGET} PRIVATE -fsanitize=undefined)
    endif()
    if(ENABLE_COVERAGE AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${TARGET} PRIVATE
            --coverage -fprofile-arcs -ftest-coverage -O0)
        target_link_options(${TARGET} PRIVATE --coverage)
    endif()
endif()
```

endmacro()

# ═════════════════════════════════════════════════════════════════════════════

# MedorCore — static library compiled once, linked by every binary

# ═════════════════════════════════════════════════════════════════════════════

set(MEDOR_CORE_SOURCES
${CMAKE_SOURCE_DIR}/src/bip39.cpp
${CMAKE_SOURCE_DIR}/src/block.cpp
${CMAKE_SOURCE_DIR}/src/blockchain.cpp
${CMAKE_SOURCE_DIR}/src/blockchain_fork.cpp
${CMAKE_SOURCE_DIR}/src/blockchain_receipts.cpp
${CMAKE_SOURCE_DIR}/src/bloom_utils.cpp
${CMAKE_SOURCE_DIR}/src/consensus.cpp
${CMAKE_SOURCE_DIR}/src/convert_medor_to_btc.cpp
${CMAKE_SOURCE_DIR}/src/convert_medor_to_eth.cpp
${CMAKE_SOURCE_DIR}/src/convert_medor_to_usdt.cpp
${CMAKE_SOURCE_DIR}/src/crypto.cpp
${CMAKE_SOURCE_DIR}/src/mempool.cpp
${CMAKE_SOURCE_DIR}/src/merkle.cpp
${CMAKE_SOURCE_DIR}/src/miner.cpp
${CMAKE_SOURCE_DIR}/src/receipt.cpp
${CMAKE_SOURCE_DIR}/src/rpc_server.cpp
${CMAKE_SOURCE_DIR}/src/several.cpp
${CMAKE_SOURCE_DIR}/src/transaction.cpp
${CMAKE_SOURCE_DIR}/src/utxo.cpp
${CMAKE_SOURCE_DIR}/src/wallet.cpp
${CMAKE_SOURCE_DIR}/src/network.cpp
${CMAKE_SOURCE_DIR}/src/connection_pool.cpp
${CMAKE_SOURCE_DIR}/src/message_codec.cpp
${CMAKE_SOURCE_DIR}/src/async_logger.cpp
${CMAKE_SOURCE_DIR}/src/thread_pool.cpp
${CMAKE_SOURCE_DIR}/src/db/rocksdb_wrapper.cpp
${CMAKE_SOURCE_DIR}/src/db/blockdb.cpp
${CMAKE_SOURCE_DIR}/src/db/accountdb.cpp
${CMAKE_SOURCE_DIR}/src/evm/host.cpp
${CMAKE_SOURCE_DIR}/src/crypto/keccak256.cpp
${CMAKE_SOURCE_DIR}/src/crypto/rlp.cpp
${CMAKE_SOURCE_DIR}/src/crypto/secp256k1_wrapper.cpp
${CMAKE_SOURCE_DIR}/src/crypto/signature.cpp
${CMAKE_SOURCE_DIR}/src/crypto/verify_signature.cpp
${CMAKE_SOURCE_DIR}/src/consensus/validator_registry.cpp
${CMAKE_SOURCE_DIR}/src/consensus/poa_sign.cpp
)

if(EXISTS “${CMAKE_SOURCE_DIR}/src/storage/EVMStorage.cpp”)
list(APPEND MEDOR_CORE_SOURCES
${CMAKE_SOURCE_DIR}/src/storage/EVMStorage.cpp)
endif()

add_library(MedorCore STATIC ${MEDOR_CORE_SOURCES})
medor_link_common(MedorCore)
