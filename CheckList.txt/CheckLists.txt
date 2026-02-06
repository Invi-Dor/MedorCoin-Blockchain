# Minimum CMake version required
cmake_minimum_required(VERSION 3.10)

# Define the project name, language, and version
project(MedorCoin VERSION 1.0 LANGUAGES CXX)

# Set the C++ standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED True)

# Include header directory
include_directories(
    ${PROJECT_SOURCE_DIR}/include
)

# Find LevelDB library installed via apt
find_library(LEVELDB_LIB leveldb)

# List all source files for the project
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

# Create executable from source files
add_executable(medorcoin ${SOURCE_FILES})

# Link the LevelDB library
target_link_libraries(medorcoin PRIVATE ${LEVELDB_LIB})
