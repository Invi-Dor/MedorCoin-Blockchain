project(Medorcoin
  VERSION 0.1.0
  DESCRIPTION "Medorcoin node and tools"
  LANGUAGES C CXX
)

# ------------------------------------------------------------------
# Global options
# ------------------------------------------------------------------
option(ENABLE_WARNINGS "Enable extra compiler warnings" ON)
option(ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(ENABLE_UBSAN "Enable UBSanitizer" OFF)
option(ENABLE_COVERAGE "Enable coverage instrumentation (GCC/Clang)" OFF)

# Default build type
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
  set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release RelWithDebInfo MinSizeRel)
endif()

# Export compile_commands.json for tooling
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Standards (global defaults)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Platform tweaks
if(MSVC)
  add_compile_options(/bigobj)
else()
  add_compile_options(-fPIC)
endif()

# ------------------------------------------------------------------
# Global sanitizer/coverage flags (non-MSVC)
# ------------------------------------------------------------------
if(NOT MSVC)
  if(ENABLE_ASAN)
    add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address)
  endif()
  if(ENABLE_UBSAN)
    add_c
ompile_options(-fsanitize=undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=undefined)
  endif()
  if(ENABLE_COVERAGE AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(--coverage -fprofile-arcs -ftest-coverage -O0)
    add_link_options(--coverage -fprofile-arcs -ftest-coverage)
  endif()
endif()

# ------------------------------------------------------------------
# Project-wide include paths (public headers)
# ------------------------------------------------------------------
include_directories(${CMAKE_SOURCE_DIR}/include)

# ------------------------------------------------------------------
# Dependencies at root so IDEs detect them (actual linking in src/)
# ------------------------------------------------------------------
find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(PkgConfig QUIET) # used by subdirs if needed

include(GNUInstallDirs)

# ------------------------------------------------------------------
# Subdirectories
# ------------------------------------------------------------------
add_subdirectory(src)

# Optional tests
if(EXISTS "${CMAKE_SOURCE_DIR}/tests/CMakeLists.txt")
  enable_testing()
  add_subdirectory(tests)
endif()

# ------------------------------------------------------------------
# Install public headers (optional)
# ------------------------------------------------------------------
if(EXISTS "${CMAKE_SOURCE_DIR}/include")
  install(DIRECTORY ${CMAKE_SOURCE_DIR}/include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
endif()

# ------------------------------------------------------------------
# Packaging (basic)
# ------------------------------------------------------------------
set(CPACK_PACKAGE_NAME "medorcoin")
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
include(CPack)
