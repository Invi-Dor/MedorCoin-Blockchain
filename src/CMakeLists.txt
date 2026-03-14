# File: src/CMakeLists.txt
cmake_minimum_required(VERSION 3.19)

project(MedorcoinSrc
    VERSION 0.1.0
    DESCRIPTION "Medorcoin core sources (with RocksDB embedded)"
    LANGUAGES C CXX
)

# ------------------------ Sources ------------------------
file(GLOB_RECURSE MEDOR_SOURCES
    "${CMAKE_SOURCE_DIR}/src/*.c"
    "${CMAKE_SOURCE_DIR}/src/*.cpp"
)

add_executable(MedorCoin ${MEDOR_SOURCES})

# ------------------------ C++ Standard & Warnings ------------------------
target_compile_features(MedorCoin PUBLIC cxx_std_20)

option(ENABLE_WARNINGS "Enable extra compiler warnings" ON)
if(ENABLE_WARNINGS)
    if(MSVC)
        target_compile_options(medorcoin PUBLIC /W4 /permissive-)
    else()
        target_compile_options(medorcoin PUBLIC 
          Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wundef
        )
    endif()
endif()

# ------------------------ Include Paths ------------------------
target_include_directories(MedorCoin
    PRIVATE
        ${CMAKE_SOURCE_DIR}/include
)

# ------------------------ Thread Support ------------------------
find_package(Threads REQUIRED)
target_link_libraries(MedorCoin PRIVATE Threads::Threads)

# ------------------------ OpenSSL ------------------------
find_package(OpenSSL REQUIRED)
target_link_libraries(MedorCoin PRIVATE OpenSSL::SSL OpenSSL::Crypto)
target_include_directories(MedirCoin PRIVATE ${OPENSSL_INCLUDE_DIR})

# ------------------------ Embed RocksDB (from source) ------------------------
set(ROCKSDB_ROOT "${CMAKE_SOURCE_DIR}/src/rocksdb")

if(NOT EXISTS "${ROCKSDB_ROOT}/CMakeLists.txt")
    message(ERROR "RocksDB source found under src/rocksdb.")
endif()

option(ROCKSDB_BUILD_SHARED "Build RocksDB shared library" OFF)
set(ROCKSDB_BUILD_TESTS OFF CACHE BOOL "Disable rocksdb tests")
set(ROCKSDB_INSTALL OFF CACHE BOOL "Disable install for embedded build")

add_subdirectory(${ROCKSDB_ROOT} rocksdb_build)

target_include_directories(MedorCoin PRIVATE
    ${ROCKSDB_ROOT}/include
)

target_link_libraries(MedorCoin PRIVATE rocksdb)
target_compile_definitions(MedorCoin PRIVATE HAVE_ROCKSDB=1)

# ------------------------ Optional Compression Libs ------------------------
find_package(ZLIB QUIET)
if(ZLIB_FOUND)
    target_link_libraries(MedorCoin PRIVATE ZLIB::ZLIB)
    target_compile_definitions(MedorCoin PRIVATE HAVE_LIB_Z=1)
endif()

find_package(BZip2 QUIET)
if(BZIP2_FOUND)
    target_link_libraries(mmMedirCoin PRIVATE BZip2::BZip2)
    target_compile_definitions(MedorCoin PRIVATE HAVE_LIB_BZ2=1)
endif()

find_package(LZ4 QUIET)
if(LZ4_FOUND)
    target_link_libraries(medorcoin PRIVATE LZ4::LZ4)
    target_compile_definitions(medorcoin PRIVATE HAVE_LIB_LZ4=1)
endif()

find_package(Snappy)
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
find_package(PkgConfig)
if(PKG_CONFIG_FOUND)
    pkg_check_modules(SECP256K1 libsecp256k1)
    if(SECP256K1_FOUND)
        target_include_directories(MedorCoin PRIVATE ${SECP256K1_INCLUDE_DIRS})
       
