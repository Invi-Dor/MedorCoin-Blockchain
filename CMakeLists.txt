cmake_minimum_required(VERSION 3.10)

project(MedorCoin VERSION 1.0 LANGUAGES C CXX)

# Use C++17
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Add include folders so headers can be found
include_directories(
    ${PROJECT_SOURCE_DIR}/src
    ${PROJECT_SOURCE_DIR}/src/abi
    ${PROJECT_SOURCE_DIR}/src/db
    ${PROJECT_SOURCE_DIR}/src/crypto/keccak
)

# ---- External Libraries ----

# LevelDB
find_library(LEVELDB_LIB leveldb REQUIRED)

# nlohmann/json (header‑only)
# You can install it via package manager or place it under third_party/json
find_package(nlohmann_json 3.2.0 REQUIRED)

# ---- EVM Engine ----
add_subdirectory(third_party/evmone)

# ---- Project Source Files ----

set(SOURCE_FILES
    src/main.cpp
    src/consensus.cpp
    src/crypto.cpp
    src/transaction.cpp
    src/mempool.cpp
    src/blockchain.cpp

    # RocksDB / LevelDB wrappers
    src/db/blockdb.cpp
    src/db/accountdb.cpp

    # ABI parser + encoder
    src/abi/abi.cpp
    src/abi/encode.cpp

    # Keccak implementation
    src/crypto/keccak/keccak.c

    # Network
    src/net/peer_manager.cpp
    src/net/net_manager.cpp

    # UTXO
    src/utxo.cpp

    # Wallet
    src/wallet.cpp
)

# ---- Build Executable ----

add_executable(medorcoin ${SOURCE_FILES})

# ---- Link Libraries ----

target_link_libraries(medorcoin
    PRIVATE
        ${LEVELDB_LIB}
        evmone
        nlohmann_json::nlohmann_json
)

# ---- Include Directories ----

target_include_directories(medorcoin PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/db
    ${CMAKE_SOURCE_DIR}/src/abi
    ${CMAKE_SOURCE_DIR}/src/crypto/keccak
    ${CMAKE_SOURCE_DIR}/src/net
)

# ---- Compiler Definitions (if any) ----

# Example: enable debug symbols
if (CMAKE_BUILD_TYPE STREQUAL "Debug")
    message(STATUS "Build type: Debug")
    target_compile_definitions(medorcoin PRIVATE DEBUG)
endif()

# ---- Custom Build Options ----

option(ENABLE_VERBOSE_LOGS "Enable verbose debug logs" OFF)

if (ENABLE_VERBOSE_LOGS)
    target_compile_definitions(medorcoin PRIVATE VERBOSE_LOGS)
endif()
