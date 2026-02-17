cat > src/CMakeLists.txt << 'CMAKE'
Collect sources (explicitly exclude build/generated dirs if any)
file(GLOB_RECURSE SOURCE_FILES
  "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
)

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
if(ENABLE_WARNINGS)
  if(MSVC)
    target_compile_options(medorcoin PRIVATE /W4 /permissive-)
  else()
    target_compile_options(medorcoin PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wundef)
  endif()
endif()

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
      add_definitions(${ROCKSDB_CFLAGS_OTHER})
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

secp256k1 via pkg-config (common and reliable)
find_package(PkgConfig REQUIRED)
pkg_check_modules(SECP256K1 REQUIRED libsecp256k1)
target_include_directories(medorcoin PRIVATE ${SECP256K1_INCLUDE_DIRS})
target_link_libraries(medorcoin PRIVATE ${SECP256K1_LIBRARIES})
add_definitions(${SECP256K1_CFLAGS_OTHER})

Optional compression backends (link only if present)
include(CheckLibraryExists)
set(_maybe_libs z bz2 lz4 zstd snappy)
foreach(lib IN LISTS _maybe_libs)
  string(TOUPPER "${lib}" LIB_UP)
  set(HAVE_VAR "HAVE_LIB_${LIB_UP}")
Try a harmless symbol for presence; fallback to simple link check
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
  endif()
endforeach()

Define feature macros for conditional compilation if desired
if(ROCKSDB_FOUND OR HAVE_LIB_ROCKSDB)
  target_compile_definitions(medorcoin PRIVATE HAVE_ROCKSDB=1)
endif()
if(SECP256K1_FOUND)
  target_compile_definitions(medorcoin PRIVATE HAVE_SECP256K1=1)
endif()

Install
install(TARGETS medorcoin RUNTIME DESTINATION bin)
CMAKE
