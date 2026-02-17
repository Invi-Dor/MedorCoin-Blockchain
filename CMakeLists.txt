cat > CMakeLists.txt << 'CMAKE'
cmake_minimum_required(VERSION 3.16)

project(MedorCoin
  VERSION 0.1.0
  DESCRIPTION "MedorCoin node"
  LANGUAGES CXX
)

Options
option(BUILD_TESTING "Build tests" ON)
option(ENABLE_WARNINGS "Enable strict warnings" ON)

C++ standard
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

Output dirs
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

Prefer position independent code for libs (future-proofing if you add libs)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

Policies (safe defaults for newer CMake, but don’t fail on older)
if(POLICY CMP0077)
  cmake_policy(SET CMP0077 NEW)
endif()

Subdirectories
add_subdirectory(src)

Tests (optional)
if(BUILD_TESTING)
  include(CTest)
  if(EXISTS "${CMAKE_SOURCE_DIR}/tests/CMakeLists.txt")
    add_subdirectory(tests)
  endif()
endif()

Packaging hints (optional; helps IDEs and future install packaging)
include(GNUInstallDirs)
message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "Install prefix: ${CMAKE_INSTALL_PREFIX}")
CMAKE
