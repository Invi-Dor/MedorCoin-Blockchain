cmake_minimum_required(VERSION 3.16)

project(MedorCoin VERSION 0.1.0 LANGUAGES CXX)

Global C++ standard
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

Options
option(BUILD_TESTING "Build tests" ON)
option(ENABLE_WARNINGS "Enable strict warnings" ON)

Where to put binaries
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

Subdirectories
add_subdirectory(src)

Tests (optional, only if you add a tests dir later)
if(BUILD_TESTING)
  enable_testing()
  if(EXISTS "${CMAKE_SOURCE_DIR}/tests/CMakeLists.txt")
    add_subdirectory(tests)
  endif()
endif()
