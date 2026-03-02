cmake_minimum_required(VERSION 3.19)

project(MedorcoinSrc
    VERSION 0.1.0
    DESCRIPTION "Medorcoin core sources (with RocksDB embedded)"
    LANGUAGES C CXX
)

# ------------------------ Sources ------------------------
set(SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src")
file(GLOB_RECURSE MEDOR_SOURCES
    "${SRC_DIR}/*.c"
    "${SRC_DIR}/*.cpp"
)

if(MEDOR_SOURCES STREQUAL "")
    message(FATAL_ERROR "No Medorcoin source files found.")
endif()

add_executable(medorcoin ${MEDOR_SOURCES})

# ------------------------ C++ Standard & Warnings ------------------------
target_compile_features(medorcoin PRIVATE cxx_std_20)

option(ENABLE_WARNINGS "Enable extra compiler warnings" ON)
if(ENABLE_WARNINGS)
    if(MSVC)
        target_compile_options(medorcoin PRIVATE /W4 /permissive-)
    else()
        target_compile_options(medorcoin PRIVATE 
            -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wundef
        )
    endif()
endif()

# ------------------------ Include Paths ------------------------
target_include_directories(medorcoin
    PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/src
)

# ------------------------ Thread Support ------------------------
find_package(Threads REQUIRED)
target_link_libraries(medorcoin PRIVATE Threads::Threads)

# ------------------------ OpenSSL ------------------------
find_package(OpenSSL REQUIRED)
target_link_libraries(medorcoin PRIVATE OpenSSL::SSL OpenSSL::Crypto)
target_include_directories(medorcoin PRIVATE ${OPENSSL_INCLUDE_DIR})

# ------------------------ Embed RocksDB (from source) ------------------------
# Assume rocksdb source is in "src/rocksdb" subdirectory
set(ROCKSDB_ROOT "${CMAKE_SOURCE_DIR}/src/rocksdb")

if(NOT EXISTS "${ROCKSDB_ROOT}/CMakeLists.txt")
    message(FATAL_ERROR "RocksDB source not found under src/rocksdb.")
endif()

# Build RocksDB as part of this project
option(ROCKSDB_BUILD_SHARED "Build RocksDB shared library" OFF)
set(ROCKSDB_BUILD_TESTS OFF CACHE BOOL "Disable rocksdb tests")
set(ROCKSDB_INSTALL OFF CACHE BOOL "Disable install for embedded build")

add_subdirectory(${ROCKSDB_ROOT} rocksdb_build)

# Expose RocksDB include directory
target_include_directories(medorcoin PRIVATE
    ${ROCKSDB_ROOT}/include
)

# Link RocksDB static library
target_link_libraries(medorcoin PRIVATE rocksdb)

target_compile_definitions(medorcoin PRIVATE HAVE_ROCKSDB=1)

# ------------------------ Optional Compression Libs ------------------------
find_package(ZLIB QUIET)
if(ZLIB_FOUND)
    target_link_libraries(medorcoin PRIVATE ZLIB::ZLIB)
    target_compile_definitions(medorcoin PRIVATE HAVE_LIB_Z=1)
endif()

find_package(BZip2 QUIET)
if(BZIP2_FOUND)
    target_link_libraries(medorcoin PRIVATE BZip2::BZip2)
    target_compile_definitions(medorcoin PRIVATE HAVE_LIB_BZ2=1)
endif()

find_package(LZ4 QUIET)
if(LZ4_FOUND)
    target_link_libraries(medorcoin PRIVATE LZ4::LZ4)
    target_compile_definitions(medorcoin PRIVATE HAVE_LIB_LZ4=1)
endif()

find_package(Snappy QUIET)
if(Snappy_FOUND)
    target_link_libraries(medorcoin PRIVATE Snappy::Snappy)
    target_compile_definitions(medorcoin PRIVATE HAVE_LIB_SNAPPY=1)
endif()

find_package(Zstd QUIET)
if(Zstd_FOUND)
    target_link_libraries(medorcoin PRIVATE Zstd::Zstd)
    target_compile_definitions(medorcoin PRIVATE HAVE_LIB_ZSTD=1)
endif()

# ------------------------ Optional secp256k1 ------------------------
find_package(PkgConfig QUIET)
if(PKG_CONFIG_FOUND)
    pkg_check_modules(SECP256K1 QUIET libsecp256k1)
    if(SECP256K1_FOUND)
        target_include_directories(medorcoin PRIVATE ${SECP256K1_INCLUDE_DIRS})
        target_link_libraries(medorcoin PRIVATE ${SECP256K1_LIBRARIES})
        target_compile_options(medorcoin PRIVATE ${SECP256K1_CFLAGS_OTHER})
        target_compile_definitions(medorcoin PRIVATE HAVE_SECP256K1=1)
    endif()
endif()

# ------------------------ Sanitizers (Optional) ------------------------
option(ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(ENABLE_UBSAN "Enable UBSanitizer" OFF)
if(NOT MSVC)
    if(ENABLE_ASAN)
        target_compile_options(medorcoin PRIVATE -fsanitize=address -fno-omit-frame-pointer)
        target_link_options(medorcoin PRIVATE -fsanitize=address)
    endif()
    if(ENABLE_UBSAN)
        target_compile_options(medorcoin PRIVATE -fsanitize=undefined -fno-omit-frame-pointer)
        target_link_options(medorcoin PRIVATE -fsanitize=undefined)
    endif()
endif()

# ------------------------ Coverage ------------------------
option(ENABLE_COVERAGE "Enable coverage" OFF)
if(ENABLE_COVERAGE AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(medorcoin PRIVATE -O0 --coverage -fprofile-arcs -ftest-coverage)
    target_link_options(medorcoin PRIVATE --coverage -fprofile-arcs -ftest-coverage)
endif()

# ------------------------ Install ------------------------
include(GNUInstallDirs)
install(TARGETS medorcoin RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
