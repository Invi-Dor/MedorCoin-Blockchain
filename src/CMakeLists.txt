cmake_minimum_required(VERSION 3.19)

# ── RocksDB embedded submodule — built once, shared by all targets ────────────
set(ROCKSDB_ROOT "${CMAKE_SOURCE_DIR}/src/rocksdb")

if(NOT EXISTS "${ROCKSDB_ROOT}/CMakeLists.txt")
    message(FATAL_ERROR
        "RocksDB source not found at ${ROCKSDB_ROOT}.\n"
        "Initialise the submodule with:\n"
        "  git submodule update --init --recursive")
endif()

set(WITH_TESTS               OFF CACHE BOOL "" FORCE)
set(WITH_TOOLS               OFF CACHE BOOL "" FORCE)
set(WITH_BENCHMARK_TOOLS     OFF CACHE BOOL "" FORCE)
set(ROCKSDB_BUILD_SHARED     OFF CACHE BOOL "" FORCE)
set(FAIL_ON_WARNINGS         OFF CACHE BOOL "" FORCE)

add_subdirectory(${ROCKSDB_ROOT} rocksdb_build EXCLUDE_FROM_ALL)

# ═════════════════════════════════════════════════════════════════════════════
# medor_link_common — applied to every target in this file
#
# Centralising flags, include paths, and library linkage in one macro ensures
# MedorCoin, MedorAPI, and MedorCore are always built with identical settings.
# ═════════════════════════════════════════════════════════════════════════════
macro(medor_link_common TARGET)

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

    target_include_directories(${TARGET} PRIVATE
        # Public API headers
        ${CMAKE_SOURCE_DIR}/include

        # All source subdirectories so every translation unit can reach
        # its siblings without relative-path gymnastics
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/abi
        ${CMAKE_SOURCE_DIR}/src/consensus
        ${CMAKE_SOURCE_DIR}/src/crypto
        ${CMAKE_SOURCE_DIR}/src/db
        ${CMAKE_SOURCE_DIR}/src/evm
        ${CMAKE_SOURCE_DIR}/src/fees
        ${CMAKE_SOURCE_DIR}/src/net
        ${CMAKE_SOURCE_DIR}/src/node

        # Third-party headers
        ${OPENSSL_INCLUDE_DIR}
        ${ROCKSDB_ROOT}/include
        ${LEVELDB_INCLUDE_DIRS}
        ${SECP256K1_INCLUDE_DIRS}
    )

    target_link_libraries(${TARGET} PRIVATE
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

    target_compile_definitions(${TARGET} PRIVATE
        HAVE_ROCKSDB=1
        HAVE_SECP256K1=1
    )

    # ── Optional compression backends ────────────────────────────────────────
    find_package(ZLIB QUIET)
    if(ZLIB_FOUND)
        target_link_libraries(${TARGET} PRIVATE ZLIB::ZLIB)
        target_compile_definitions(${TARGET} PRIVATE HAVE_LIB_Z=1)
    endif()

    find_package(BZip2 QUIET)
    if(BZIP2_FOUND)
        target_link_libraries(${TARGET} PRIVATE BZip2::BZip2)
        target_compile_definitions(${TARGET} PRIVATE HAVE_LIB_BZ2=1)
    endif()

    find_package(PkgConfig REQUIRED)

    pkg_check_modules(LZ4 QUIET liblz4)
    if(LZ4_FOUND)
        target_include_directories(${TARGET} PRIVATE ${LZ4_INCLUDE_DIRS})
        target_link_libraries(${TARGET} PRIVATE ${LZ4_LIBRARIES})
        target_compile_definitions(${TARGET} PRIVATE HAVE_LIB_LZ4=1)
    endif()

    find_package(Snappy QUIET)
    if(Snappy_FOUND)
        target_link_libraries(${TARGET} PRIVATE Snappy::Snappy)
        target_compile_definitions(${TARGET} PRIVATE HAVE_LIB_SNAPPY=1)
    endif()

    pkg_check_modules(ZSTD QUIET libzstd)
    if(ZSTD_FOUND)
        target_include_directories(${TARGET} PRIVATE ${ZSTD_INCLUDE_DIRS})
        target_link_libraries(${TARGET} PRIVATE ${ZSTD_LIBRARIES})
        target_compile_definitions(${TARGET} PRIVATE HAVE_LIB_ZSTD=1)
    endif()

    # ── EVMC (EVM host interface) ─────────────────────────────────────────────
    find_package(evmc QUIET)
    if(evmc_FOUND)
        target_link_libraries(${TARGET} PRIVATE evmc::evmc)
        target_compile_definitions(${TARGET} PRIVATE HAVE_EVMC=1)
    endif()

    # ── Sanitiser / coverage link flags (compile flags set at root) ───────────
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

endmacro()

# ═════════════════════════════════════════════════════════════════════════════
# MedorCore — static library compiled once, linked by every binary
#
# Contains all blockchain, networking, cryptographic, and storage logic.
# wallet.js and bindings.gyp are Node.js/N-API artefacts and are handled
# by the Node build system, not by CMake; they are excluded here.
# ═════════════════════════════════════════════════════════════════════════════
set(MEDOR_CORE_SOURCES

    # ── Top-level source files ────────────────────────────────────────────────
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

    # ── Networking layer ──────────────────────────────────────────────────────
    ${CMAKE_SOURCE_DIR}/src/network.cpp
    ${CMAKE_SOURCE_DIR}/src/connection_pool.cpp
    ${CMAKE_SOURCE_DIR}/src/message_codec.cpp
    ${CMAKE_SOURCE_DIR}/src/async_logger.cpp
    ${CMAKE_SOURCE_DIR}/src/thread_pool.cpp

    # ── Database wrappers ─────────────────────────────────────────────────────
    ${CMAKE_SOURCE_DIR}/src/db/rocksdb_wrapper.cpp
    ${CMAKE_SOURCE_DIR}/src/db/blockdb.cpp
    ${CMAKE_SOURCE_DIR}/src/db/accountdb.cpp

    # ── Storage layer ─────────────────────────────────────────────────────────
    # EVMStorage.cpp lives under src/storage/ if present; if the file was
    # merged into src/blockchain.cpp during refactoring, remove this line.
    $<$<BOOL:${EXISTS_EVMSTORAGE}>:${CMAKE_SOURCE_DIR}/src/storage/EVMStorage.cpp>

    # ── EVM host ──────────────────────────────────────────────────────────────
    ${CMAKE_SOURCE_DIR}/src/evm/host.cpp

    # ── Cryptography ─────────────────────────────────────────────────────────
    ${CMAKE_SOURCE_DIR}/src/crypto/keccak256.cpp
    ${CMAKE_SOURCE_DIR}/src/crypto/rlp.cpp
    ${CMAKE_SOURCE_DIR}/src/crypto/secp256k1_wrapper.cpp
    ${CMAKE_SOURCE_DIR}/src/crypto/signature.cpp
    ${CMAKE_SOURCE_DIR}/src/crypto/verify_signature.cpp

    # ── Consensus ────────────────────────────────────────────────────────────
    ${CMAKE_SOURCE_DIR}/src/consensus/validator_registry.cpp
    ${CMAKE_SOURCE_DIR}/src/consensus/poa_sign.cpp
)

# Resolve the conditional EVMStorage path at configure time
if(EXISTS "${CMAKE_SOURCE_DIR}/src/storage/EVMStorage.cpp")
    set(EXISTS_EVMSTORAGE TRUE)
    list(APPEND MEDOR_CORE_SOURCES
        ${CMAKE_SOURCE_DIR}/src/storage/EVMStorage.cpp)
endif()

add_library(MedorCore STATIC ${MEDOR_CORE_SOURCES})
medor_link_common(MedorCore)

# ═════════════════════════════════════════════════════════════════════════════
# Target 1 — MedorCoin  (blockchain node binary)
# Entry point: src/main.cpp
# ═════════════════════════════════════════════════════════════════════════════
if(EXISTS "${CMAKE_SOURCE_DIR}/src/main.cpp")
    add_executable(MedorCoin ${CMAKE_SOURCE_DIR}/src/main.cpp)
    medor_link_common(MedorCoin)
    target_link_libraries(MedorCoin PRIVATE MedorCore)

    include(GNUInstallDirs)
    install(TARGETS MedorCoin
            RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
else()
    message(STATUS
        "src/main.cpp not found — MedorCoin executable skipped. "
        "Add src/main.cpp when ready.")
endif()

# ═════════════════════════════════════════════════════════════════════════════
# Target 2 — MedorAPI  (REST / WebSocket API service)
#
# api.cpp       — REST endpoint implementations
# auth.cpp      — authentication middleware
# rpc_handlers.cpp — JSON-RPC method handlers
# server.cpp    — HTTP/WS server entry point
# api_server.cpp and api_swap.cpp — root-level API sources added previously
#
# wallet.js is a Node.js binding file and is excluded from CMake.
# medorcoin_addon.cpp is compiled by node-gyp via bindings.gyp and is
# therefore also excluded here to prevent duplicate symbol errors.
# ═════════════════════════════════════════════════════════════════════════════
set(MEDOR_API_SOURCES
    ${CMAKE_SOURCE_DIR}/src/api.cpp
    ${CMAKE_SOURCE_DIR}/src/auth.cpp
    ${CMAKE_SOURCE_DIR}/src/rpc_handlers.cpp
    ${CMAKE_SOURCE_DIR}/src/server.cpp
    ${CMAKE_SOURCE_DIR}/api_server.cpp
    ${CMAKE_SOURCE_DIR}/api_swap.cpp
)

foreach(_src ${MEDOR_API_SOURCES})
    if(NOT EXISTS "${_src}")
        message(FATAL_ERROR
            "API source file not found: ${_src}\n"
            "Ensure all API source files are present before configuring.")
    endif()
endforeach()

add_executable(MedorAPI ${MEDOR_API_SOURCES})
medor_link_common(MedorAPI)
target_link_libraries(MedorAPI PRIVATE MedorCore)

include(GNUInstallDirs)
install(TARGETS MedorAPI
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# ═════════════════════════════════════════════════════════════════════════════
# Target 3 — systemd service unit
# ═════════════════════════════════════════════════════════════════════════════
set(_SERVICE_FILE "${CMAKE_SOURCE_DIR}/medorcoin-blockchain.service")

if(EXISTS "${_SERVICE_FILE}")
    if(CMAKE_INSTALL_PREFIX STREQUAL "/usr" OR
       CMAKE_INSTALL_PREFIX STREQUAL "/usr/local")
        install(FILES "${_SERVICE_FILE}"
                DESTINATION "${CMAKE_INSTALL_PREFIX}/lib/systemd/system"
                PERMISSIONS
                    OWNER_READ OWNER_WRITE
                    GROUP_READ
                    WORLD_READ)
        message(STATUS
            "systemd unit will install to "
            "${CMAKE_INSTALL_PREFIX}/lib/systemd/system/")
    else()
        message(STATUS
            "CMAKE_INSTALL_PREFIX='${CMAKE_INSTALL_PREFIX}' — "
            "systemd unit will not install automatically. "
            "To deploy manually: "
            "sudo cp ${_SERVICE_FILE} /usr/lib/systemd/system/ "
            "&& sudo systemctl daemon-reload")
    endif()
else()
    message(WARNING
        "medorcoin-blockchain.service not found — "
        "systemd install step skipped.")
endif()

# ═════════════════════════════════════════════════════════════════════════════
# Intentionally excluded files and the reason for each exclusion
#
#   wallet.js          — Node.js module; built by npm/yarn, not CMake.
#   bindings.gyp       — node-gyp configuration; built by node-gyp, not CMake.
#   medorcoin_addon.cpp— N-API addon compiled exclusively by node-gyp via
#                        bindings.gyp. Including it in CMake would produce
#                        duplicate symbols when the addon is also loaded by
#                        Node. Build it with: node-gyp configure && node-gyp build
# ═════════════════════════════════════════════════════════════════════════════
