cmake_minimum_required(VERSION 3.16)

project(MedorCoinNode LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Collect sources
file(GLOB_RECURSE SOURCE_FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
)

add_executable(medorcoin ${SOURCE_FILES})

# Include directories
target_include_directories(medorcoin
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}            # src
        ${CMAKE_CURRENT_SOURCE_DIR}/net        # networking
        ${CMAKE_CURRENT_SOURCE_DIR}/crypto     # crypto
        ${PROJECT_SOURCE_DIR}/../include       # public headers
)

# ------------------------
# RocksDB (block storage)
# ------------------------
find_package(RocksDB REQUIRED)
target_link_libraries(medorcoin PRIVATE RocksDB::rocksdb)

# ------------------------
# OpenSSL (crypto)
# ------------------------
find_package(OpenSSL REQUIRED)
target_link_libraries(medorcoin
    PRIVATE
        OpenSSL::SSL
        OpenSSL::Crypto
)

# ------------------------
# secp256k1 (signatures)
# ------------------------
find_package(PkgConfig REQUIRED)
pkg_check_modules(SECP256K1 REQUIRED libsecp256k1)

target_include_directories(medorcoin
    PRIVATE
        ${SECP256K1_INCLUDE_DIRS}
)

target_link_libraries(medorcoin
    PRIVATE
        ${SECP256K1_LIBRARIES}
)

# ------------------------
# Threads (P2P networking)
# ------------------------
find_package(Threads REQUIRED)
target_link_libraries(medorcoin PRIVATE Threads::Threads)
