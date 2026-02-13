cmake_minimum_required(VERSION 3.16)

project(MedorCoin VERSION 0.1.0 LANGUAGES CXX)

# C++ standard
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Enable verbose output (optional)
set(CMAKE_VERBOSE_MAKEFILE ON)

# Include header search paths
# Allows #include "blockchain.h" and others from include/
include_directories(
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/src
    ${PROJECT_SOURCE_DIR}/src/net
)

# Add subdirectory where source files live
add_subdirectory(src)
