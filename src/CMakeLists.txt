Sources
file(GLOB_RECURSE SOURCE_FILES
  "${CMAKE_CURRENT_SOURCE_DIR}/.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/.cpp"
)

if(NOT SOURCE_FILES)
  message(FATAL_ERROR "No source files found in ${CMAKE_CURRENT_SOURCE_DIR}")
endif()

Target
add_executable(medorcoin ${SOURCE_FILES})

Include paths
target_include_directories(medorcoin
  PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/net
    ${CMAKE_SOURCE_DIR}/src/crypto
    ${CMAKE_SOURCE_DIR}/src/db
    ${CMAKE_SOURCE_DIR}/src/evm
)

C++ standard and warnings
target_compile_features(medorcoin PRIVATE cxx_std_20)
option(ENABLE_WARNINGS "Enable extra compiler warnings" ON)
if(ENABLE_WARNINGS)
  if(MSVC)
    target_compile_options(medorcoin PRIVATE /W4 /permissive-)
  else()
    target_compile_options(medorcoin PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wundef)
  endif()
endif()

Position independent code (useful for static libs linked into shared, sanitizers, etc.)
set_property(TARGET medorcoin PROPERTY POSITION_INDEPENDENT_CODE ON)

Threads
find_package(Threads REQUIRED)
target_link_libraries(medorcoin PRIVATE Threads::Threads)

OpenSSL
find_package(OpenSSL REQUIRED)
target_link_libraries(medorcoin PRIVATE OpenSSL::SSL OpenSSL::Crypto)

RocksDB (try CMake package, then pkg-config, then -lrocksdb)
set(_rocksdb_found FALSE)
find_package(RocksDB QUIET)
if(RocksDB_FOUND)
  target_link_libraries(medorcoin PRIVATE RocksDB::rocksdb)
  set(_rocksdb_found TRUE)
endif()

if(NOT _rocksdb_found)
  find_package(PkgConfig QUIET)
  if(PKG_CONFIG_FOUND)
    pkg_check_modules(ROCKSDB QUIET rocksdb)
    if(ROCKSDB_FOUND)
      target_include_directories(medorcoin PRIVATE ${ROCKSDB_INCLUDE_DIRS})
      target_link_libraries(medorcoin PRIVATE ${ROCKSDB_LIBRARIES})
      target_compile_options(medorcoin PRIVATE ${ROCKSDB_CFLAGS_OTHER})
      set(_rocksdb_found TRUE)
    endif()
  endif()
endif()

if(NOT _rocksdb_found)
  include(CheckLibraryExists)
  check_library_exists(rocksdb rocksdb_open "" HAVE_LIB_ROCKSDB)
  if(HAVE_LIB_ROCKSDB)
    target_link_libraries(medorcoin PRIVATE rocksdb)
    set(_rocksdb_found TRUE)
  endif()
endif()

if(NOT _rocksdb_found)
  message(FATAL_ERROR
    "RocksDB not found.\n"
    "Install librocksdb-dev (Debian/Ubuntu), rocksdb-devel (Fedora), or brew install rocksdb.\n"
    "Or set CMAKE_PREFIX_PATH/ROCKSDB_ROOT or PKG_CONFIG_PATH."
  )
endif()

secp256k1 via pkg-config
find_package(PkgConfig REQUIRED)
pkg_check_modules(SECP256K1 REQUIRED libsecp256k1)
target_include_directories(medorcoin PRIVATE ${SECP256K1_INCLUDE_DIRS})
target_link_libraries(medorcoin PRIVATE ${SECP256K1_LIBRARIES})
target_compile_options(medorcoin PRIVATE ${SECP256K1_CFLAGS_OTHER})

Optional compression backends (link only if present)
include(CheckLibraryExists)
set(_maybe_libs z bz2 lz4 zstd snappy)
foreach(lib IN LISTS _maybe_libs)
  string(TOUPPER "${lib}" LIB_UP)
  set(HAVE_VAR "HAVE_LIB_${LIB_UP}")
  if(lib STREQUAL "z")
    check_library_exists(${lib} inflate "" ${HAVE_VAR})
  elseif(lib STREQUAL "bz2")
    check_library_exists(${lib} BZ2_bzCompress "" ${HAVE_VAR})
  elseif(lib STREQUAL "lz4")
    check_library_exists(${lib} LZ4_createStream "" ${HAVE_VAR})
  elseif(lib STREQUAL "zstd")
    check_library_exists(${lib} ZSTD_createCCtx "" ${HAVE_VAR})
  elseif(lib STREQUAL "snappy")
    check_library_exists(${lib} snappy_compress "" ${HAVE_VAR})
  else()
    check_library_exists(${lib} ${lib}_version "" ${HAVE_VAR})
  endif()
  if(${HAVE_VAR})
    target_link_libraries(medorcoin PRIVATE ${lib})
    target_compile_definitions(medorcoin PRIVATE "HAVE_LIB_${LIB_UP}=1")
  endif()
endforeach()

Feature macros
if(RocksDB_FOUND OR HAVE_LIB_ROCKSDB)
  target_compile_definitions(medorcoin PRIVATE HAVE_ROCKSDB=1)
endif()
if(SECP256K1_FOUND)
  target_compile_definitions(medorcoin PRIVATE HAVE_SECP256K1=1)
endif()

Sanitizers (optional)
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

Coverage (GCC/Clang)
option(ENABLE_COVERAGE "Enable coverage flags" OFF)
if(ENABLE_COVERAGE AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(medorcoin PRIVATE -O0 --coverage -fprofile-arcs -ftest-coverage)
  target_link_options(medorcoin PRIVATE --coverage -fprofile-arcs -ftest-coverage)
endif()

Install
include(GNUInstallDirs)
install(TARGETS medorcoin RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
CMAKE
