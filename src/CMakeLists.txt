cat > src/CMakeLists.txt << 'EOF'
cmake_minimum_required(VERSION 3.16)

project(MedorCoinNode LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)

# Collect .cpp files from both root and subfolders
file(GLOB_RECURSE ROOT_CPP_FILES
    "${PROJECT_SOURCE_DIR}/*.cpp"
)

add_executable(medorcoin ${SOURCE_FILES})

target_include_directories(medorcoin
    PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/net
        ${CMAKE_SOURCE_DIR}/src/crypto
)


# RocksDB
find_package(RocksDB REQUIRED)
target_link_libraries(medorcoin PRIVATE RocksDB::rocksdb)

# OpenSSL
find_package(OpenSSL REQUIRED)
target_link_libraries(medorcoin
    PRIVATE
        OpenSSL::SSL
        OpenSSL::Crypto
)
file(GLOB_RECURSE SOURCE_FILES
    "${CMAKE_SOURCE_DIR}/*.cpp"
)
# secp256k1 via pkg-config
find_package(PkgConfig REQUIRED)
pkg_check_modules(SECP256K1 REQUIRED libsecp256k1)

target_include_directories(medorcoin
    PRIVATE
        ${SECP256K1_INCLUDE_DIRS}
)

target_link_libraries(medorcoin
    PRIVATE
        ${SECP256K1_LIBRARIES}
)

find_package(Threads REQUIRED)
target_link_libraries(medorcoin
    PRIVATE
        Threads::Threads
)
EOF
