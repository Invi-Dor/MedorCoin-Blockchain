cmake_minimum_required(VERSION 3.20)

# MedorCoin sources
set(SRC_FILES
    main.cpp
    blockchain.cpp
    transaction.cpp
    mempool.cpp
    # Add other source files here
)

# Create executable
add_executable(medorcoin ${SRC_FILES})

# Include directories (public headers)
target_include_directories(medorcoin PRIVATE
    ${PROJECT_SOURCE_DIR}/include
)

# Link external dependencies
find_package(LevelDB REQUIRED)
find_package(secp256k1 REQUIRED)
# If using evmc and libp2p, add find_package or FetchContent below

target_link_libraries(medorcoin PRIVATE
    LevelDB::leveldb
    secp256k1::secp256k1
    # Link other libs here (evmc, libp2p, etc.)
)
