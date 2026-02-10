cmake_minimum_required(VERSION 3.10)

project(MedorCoin VERSION 1.0 LANGUAGES CXX)

# Use C++17
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Add include folder so headers can be found
include_directories(
    ${PROJECT_SOURCE_DIR}/include
)

# Find the installed LevelDB library
find_library(LEVELDB_LIB leveldb)

# List source files
set(SOURCE_FILES
    src/main.cpp
    src/consensus.cpp
    src/crypto.cpp
    src/transaction.cpp
    src/mempool.cpp
    src/blockchain.cpp
    src/db/blockdb.cpp
    src/net/peer_manager.cpp
    src/net/net_manager.cpp
)

# Define the executable output
add_executable(medorcoin ${SOURCE_FILES})

# Link LevelDB
target_link_libraries(medorcoin PRIVATE ${LEVELDB_LIB})

add_subdirectory(third_party/evmone)
