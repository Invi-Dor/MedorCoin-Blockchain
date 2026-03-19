cmake_minimum_required(VERSION 3.19)

set(ROCKSDB_ROOT "${CMAKE_SOURCE_DIR}/src/rocksdb")

if(NOT EXISTS "${ROCKSDB_ROOT}/CMakeLists.txt")
    message(FATAL_ERROR
        "RocksDB source not found at ${ROCKSDB_ROOT}.\n"
        "Run: git submodule update --init --recursive")
endif()

add_subdirectory(${ROCKSDB_ROOT} rocksdb_build EXCLUDE_FROM_ALL)

find_package(OpenSSL   REQUIRED)
find_package(PkgConfig REQUIRED)
find_package(Threads   REQUIRED)

if(NOT DEFINED SECP256K1_LIBRARIES OR NOT DEFINED SECP256K1_INCLUDE_DIRS)
    find_library(SECP256K1_LIB secp256k1
        PATHS /usr/lib/x86_64-linux-gnu /usr/local/lib)
    find_path(SECP256K1_INC secp256k1.h
        PATHS /usr/include /usr/local/include)
    if(NOT SECP256K1_LIB OR NOT SECP256K1_INC)
        message(FATAL_ERROR "secp256k1 not found.")
    endif()
    set(SECP256K1_LIBRARIES    ${SECP256K1_LIB})
    set(SECP256K1_INCLUDE_DIRS ${SECP256K1_INC})
endif()

if(NOT DEFINED LEVELDB_LIBRARIES OR NOT DEFINED LEVELDB_INCLUDE_DIRS)
    find_library(LEVELDB_LIB leveldb
        PATHS /usr/lib/x86_64-linux-gnu /usr/local/lib)
    find_path(LEVELDB_INC leveldb/db.h
        PATHS /usr/include /usr/local/include)
    if(NOT LEVELDB_LIB OR NOT LEVELDB_INC)
        message(FATAL_ERROR "LevelDB not found.")
    endif()
    set(LEVELDB_LIBRARIES    ${LEVELDB_LIB})
    set(LEVELDB_INCLUDE_DIRS ${LEVELDB_INC})
endif()

find_package(ZLIB  QUIET)
find_package(BZip2 QUIET)

pkg_check_modules(LZ4 QUIET liblz4)
if(NOT LZ4_FOUND)
    find_library(LZ4_LIB lz4
        PATHS /usr/lib/x86_64-linux-gnu /usr/local/lib)
    find_path(LZ4_INC lz4.h
        PATHS /usr/include /usr/local/include)
    if(LZ4_LIB AND LZ4_INC)
        set(LZ4_FOUND TRUE)
        set(LZ4_LIBRARIES    ${LZ4_LIB})
        set(LZ4_INCLUDE_DIRS ${LZ4_INC})
    endif()
endif()

pkg_check_modules(SNAPPY QUIET snappy)
if(NOT SNAPPY_FOUND)
    find_library(SNAPPY_LIB snappy
        PATHS /usr/lib/x86_64-linux-gnu /usr/local/lib)
    find_path(SNAPPY_INC snappy.h
        PATHS /usr/include /usr/local/include)
    if(SNAPPY_LIB AND SNAPPY_INC)
        set(SNAPPY_FOUND TRUE)
        set(SNAPPY_LIBRARIES    ${SNAPPY_LIB})
        set(SNAPPY_INCLUDE_DIRS ${SNAPPY_INC})
    endif()
endif()

pkg_check_modules(ZSTD QUIET libzstd)
if(NOT ZSTD_FOUND)
    find_library(ZSTD_LIB zstd
        PATHS /usr/lib/x86_64-linux-gnu /usr/local/lib)
    find_path(ZSTD_INC zstd.h
        PATHS /usr/include /usr/local/include)
    if(ZSTD_LIB AND ZSTD_INC)
        set(ZSTD_FOUND TRUE)
        set(ZSTD_LIBRARIES    ${ZSTD_LIB})
        set(ZSTD_INCLUDE_DIRS ${ZSTD_INC})
    endif()
endif()

pkg_check_modules(JEMALLOC QUIET jemalloc)
if(NOT JEMALLOC_FOUND)
    find_library(JEMALLOC_LIB jemalloc
        PATHS /usr/lib/x86_64-linux-gnu /usr/local/lib)
    find_path(JEMALLOC_INC jemalloc/jemalloc.h
        PATHS /usr/include /usr/local/include)
    if(JEMALLOC_LIB AND JEMALLOC_INC)
        set(JEMALLOC_FOUND TRUE)
        set(JEMALLOC_LIBRARIES    ${JEMALLOC_LIB})
        set(JEMALLOC_INCLUDE_DIRS ${JEMALLOC_INC})
    endif()
endif()

find_package(evmc QUIET)

macro(medor_link_common TARGET)

    target_compile_features(${TARGET} PUBLIC cxx_std_20)

    if(ENABLE_WARNINGS)
        if(MSVC)
            target_compile_options(${TARGET} PRIVATE /W4 /permissive-)
        else()
            target_compile_options(${TARGET} PRIVATE
                -Wall -Wextra -Wpedantic -Wconversion
                -Wshadow -Wundef -Wno-unused-parameter)
        endif()
    endif()

    target_include_directories(${TARGET} PUBLIC
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/include/db
        ${CMAKE_SOURCE_DIR}/include/net
        ${CMAKE_SOURCE_DIR}/include/mempool
        ${CMAKE_SOURCE_DIR}/include/crypto
        ${CMAKE_SOURCE_DIR}/include/consensus
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/abi
        ${CMAKE_SOURCE_DIR}/src/consensus
        ${CMAKE_SOURCE_DIR}/src/crypto
        ${CMAKE_SOURCE_DIR}/src/db
        ${CMAKE_SOURCE_DIR}/src/evm
        ${CMAKE_SOURCE_DIR}/src/fees
        ${CMAKE_SOURCE_DIR}/src/net
        ${CMAKE_SOURCE_DIR}/src/node
        ${CMAKE_SOURCE_DIR}/src/mempool
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

    target_compile_definitions(${TARGET} PUBLIC
        HAVE_ROCKSDB=1
        HAVE_SECP256K1=1
    )

    if(ZLIB_FOUND)
        target_link_libraries(${TARGET} PUBLIC ZLIB::ZLIB)
        target_compile_definitions(${TARGET} PUBLIC HAVE_LIB_Z=1)
    endif()
    if(BZIP2_FOUND)
        target_link_libraries(${TARGET} PUBLIC BZip2::BZip2)
        target_compile_definitions(${TARGET} PUBLIC HAVE_LIB_BZ2=1)
    endif()
    if(LZ4_FOUND)
        target_include_directories(${TARGET} PUBLIC ${LZ4_INCLUDE_DIRS})
        target_link_libraries(${TARGET} PUBLIC ${LZ4_LIBRARIES})
        target_compile_definitions(${TARGET} PUBLIC HAVE_LIB_LZ4=1)
    endif()
    if(SNAPPY_FOUND)
        target_include_directories(${TARGET} PUBLIC ${SNAPPY_INCLUDE_DIRS})
        target_link_libraries(${TARGET} PUBLIC ${SNAPPY_LIBRARIES})
        target_compile_definitions(${TARGET} PUBLIC HAVE_LIB_SNAPPY=1)
    endif()
    if(ZSTD_FOUND)
        target_include_directories(${TARGET} PUBLIC ${ZSTD_INCLUDE_DIRS})
        target_link_libraries(${TARGET} PUBLIC ${ZSTD_LIBRARIES})
        target_compile_definitions(${TARGET} PUBLIC HAVE_LIB_ZSTD=1)
    endif()
    if(JEMALLOC_FOUND)
        target_include_directories(${TARGET} PUBLIC ${JEMALLOC_INCLUDE_DIRS})
        target_link_libraries(${TARGET} PUBLIC ${JEMALLOC_LIBRARIES})
        target_compile_definitions(${TARGET} PUBLIC HAVE_JEMALLOC=1)
    endif()
    if(evmc_FOUND)
        target_link_libraries(${TARGET} PUBLIC evmc::evmc)
        target_compile_definitions(${TARGET} PUBLIC HAVE_EVMC=1)
    endif()

endmacro()

set(MEDOR_CORE_SOURCES
    ${CMAKE_SOURCE_DIR}/src/block.cpp
    ${CMAKE_SOURCE_DIR}/src/blockchain.cpp
    ${CMAKE_SOURCE_DIR}/src/blockchain_fork.cpp
    ${CMAKE_SOURCE_DIR}/src/blockchain_receipts.cpp
    ${CMAKE_SOURCE_DIR}/src/transaction.cpp
    ${CMAKE_SOURCE_DIR}/src/receipt.cpp
    ${CMAKE_SOURCE_DIR}/src/merkle.cpp
    ${CMAKE_SOURCE_DIR}/src/miner.cpp
    ${CMAKE_SOURCE_DIR}/src/utxo.cpp
    ${CMAKE_SOURCE_DIR}/src/mempool/mempool.cpp
    ${CMAKE_SOURCE_DIR}/src/net/net_manager.cpp
    ${CMAKE_SOURCE_DIR}/src/net/peer_manager.cpp
    ${CMAKE_SOURCE_DIR}/src/net/message_handler.cpp
    ${CMAKE_SOURCE_DIR}/src/net/p2p_node.cpp
    ${CMAKE_SOURCE_DIR}/src/net/serialization.cpp
    ${CMAKE_SOURCE_DIR}/src/net/sync_manager.cpp
    ${CMAKE_SOURCE_DIR}/src/connection_pool.cpp
    ${CMAKE_SOURCE_DIR}/src/async_logger.cpp
    ${CMAKE_SOURCE_DIR}/src/thread_pool.cpp
    ${CMAKE_SOURCE_DIR}/src/crypto/keccak256.cpp
    ${CMAKE_SOURCE_DIR}/src/crypto/rlp.cpp
    ${CMAKE_SOURCE_DIR}/src/crypto/secp256k1_wrapper.cpp
    ${CMAKE_SOURCE_DIR}/src/crypto/signature.cpp
    ${CMAKE_SOURCE_DIR}/src/crypto/verify_signature.cpp
    ${CMAKE_SOURCE_DIR}/src/bip39.cpp
    ${CMAKE_SOURCE_DIR}/src/wallet.cpp
    ${CMAKE_SOURCE_DIR}/src/consensus/validator_registry.cpp
    ${CMAKE_SOURCE_DIR}/src/consensus/poa_sign.cpp
    ${CMAKE_SOURCE_DIR}/src/consensus.cpp
    ${CMAKE_SOURCE_DIR}/src/db/rocksdb_wrapper.cpp
    ${CMAKE_SOURCE_DIR}/src/db/blockdb.cpp
    ${CMAKE_SOURCE_DIR}/src/db/accountdb.cpp
    ${CMAKE_SOURCE_DIR}/src/rpc_server.cpp
    ${CMAKE_SOURCE_DIR}/src/bloom_utils.cpp
    ${CMAKE_SOURCE_DIR}/src/several.cpp
    ${CMAKE_SOURCE_DIR}/src/crypto.cpp
    ${CMAKE_SOURCE_DIR}/src/convert_medor_to_btc.cpp
    ${CMAKE_SOURCE_DIR}/src/convert_medor_to_eth.cpp
    ${CMAKE_SOURCE_DIR}/src/convert_medor_to_usdt.cpp
)

if(EXISTS "${CMAKE_SOURCE_DIR}/src/evm/host.cpp")
    list(APPEND MEDOR_CORE_SOURCES ${CMAKE_SOURCE_DIR}/src/evm/host.cpp)
endif()
if(EXISTS "${CMAKE_SOURCE_DIR}/src/storage/EVMStorage.cpp")
    list(APPEND MEDOR_CORE_SOURCES ${CMAKE_SOURCE_DIR}/src/storage/EVMStorage.cpp)
endif()
if(EXISTS "${CMAKE_SOURCE_DIR}/src/node/node.cpp")
    list(APPEND MEDOR_CORE_SOURCES ${CMAKE_SOURCE_DIR}/src/node/node.cpp)
endif()

add_library(MedorCore STATIC ${MEDOR_CORE_SOURCES})
medor_link_common(MedorCore)

add_executable(medorcoin-node ${CMAKE_SOURCE_DIR}/src/main.cpp)
medor_link_common(medorcoin-node)
target_link_libraries(medorcoin-node PRIVATE MedorCore)
set_target_properties(medorcoin-node PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
install(TARGETS medorcoin-node
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

if(EXISTS "${CMAKE_SOURCE_DIR}/src/rpc_cli.cpp")
    add_executable(medorcoin-rpc ${CMAKE_SOURCE_DIR}/src/rpc_cli.cpp)
    medor_link_common(medorcoin-rpc)
    target_link_libraries(medorcoin-rpc PRIVATE MedorCore)
    set_target_properties(medorcoin-rpc PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
    install(TARGETS medorcoin-rpc
            RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

if(EXISTS "${CMAKE_SOURCE_DIR}/src/wallet_cli.cpp")
    add_executable(medorcoin-wallet ${CMAKE_SOURCE_DIR}/src/wallet_cli.cpp)
    medor_link_common(medorcoin-wallet)
    target_link_libraries(medorcoin-wallet PRIVATE MedorCore)
    set_target_properties(medorcoin-wallet PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
    install(TARGETS medorcoin-wallet
            RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

message(STATUS "")
message(STATUS "========== MedorCoin Build Configuration ==========")
message(STATUS "  Build type   : ${CMAKE_BUILD_TYPE}")
message(STATUS "  C++ standard : C++20")
message(STATUS "  OpenSSL      : ${OPENSSL_VERSION}")
message(STATUS "  RocksDB      : ${ROCKSDB_ROOT}")
message(STATUS "  LevelDB      : ${LEVELDB_LIBRARIES}")
message(STATUS "  secp256k1    : ${SECP256K1_LIBRARIES}")
message(STATUS "  Output       : ${CMAKE_BINARY_DIR}/bin/")
message(STATUS "====================================================")
message(STATUS "")
