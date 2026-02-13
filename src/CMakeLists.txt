cmake_minimum_required(VERSION 3.16)

project(MedorCoin VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(SOURCES
    main.cpp
    blockchain.cpp
    transaction.cpp
    utxo.cpp
    crypto/keystore.cpp
    crypto/evm_sign.cpp
    crypto/rlp.cpp
    crypto/signature.cpp
    net/net_manager.cpp
    net/sync_manager.cpp
    net/serialization.cpp
    blockchain_fork.cpp
)

add_executable(MedorCoinNode ${SOURCES})

target_include_directories(MedorCoinNode
    PUBLIC
        ${PROJECT_SOURCE_DIR}
        ${PROJECT_SOURCE_DIR}/net
        ${PROJECT_SOURCE_DIR}/crypto
        ${PROJECT_SOURCE_DIR}/../include
)

# ---- LevelDB integration via pkg-config ----
find_package(PkgConfig REQUIRED)
pkg_check_modules(LEVELDB REQUIRED leveldb)

target_link_libraries(MedorCoinNode
    PRIVATE
        ${LEVELDB_LIBRARIES}
)

target_include_directories(MedorCoinNode
    PRIVATE
        ${LEVELDB_INCLUDE_DIRS}
)

# ---- Optional crypto libs ----
find_package(OpenSSL REQUIRED)
target_link_libraries(MedorCoinNode PRIVATE OpenSSL::SSL OpenSSL::Crypto)

find_package(PkgConfig REQUIRED)
pkg_check_modules(SECP256K1 REQUIRED libsecp256k1)

target_link_libraries(MedorCoinNode PRIVATE ${SECP256K1_LIBRARIES})
target_include_directories(MedorCoinNode PRIVATE ${SECP256K1_INCLUDE_DIRS})
