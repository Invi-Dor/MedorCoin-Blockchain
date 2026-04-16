# =============================================================================
# DISTRIBUTION & PERFORMANCE SETTINGS
# =============================================================================
option(PRODUCTION_RELEASE "Set to ON for broad distribution compatibility" OFF)
option(USE_NATIVE_ARCH     "Optimise for the current machine's CPU"       ON)

macro(medor_link_common TARGET)
    if(TARGET ${TARGET})
        target_compile_features(${TARGET} PUBLIC cxx_std_20)

        if(NOT MSVC)
            # 1. Base Hardening (Always On)
            target_compile_options(${TARGET} PRIVATE 
                -Wall -Wextra -fstack-protector-strong -D_FORTIFY_SOURCE=2)

            # 2. Performance vs. Portability Logic
            if(PRODUCTION_RELEASE)
                # Generic target for broad compatibility (x86-64-v2 or similar)
                # This ensures it runs on almost any 64-bit CPU from the last 10 years.
                target_compile_options(${TARGET} PRIVATE -O3 -mtune=generic)
                message(STATUS "Build Mode: PRODUCTION (High Compatibility)")
            elseif(USE_NATIVE_ARCH)
                # Local dev speed boost
                target_compile_options(${TARGET} PRIVATE -O3 -march=native)
                message(STATUS "Build Mode: DEVELOPMENT (Optimised for this machine)")
            else()
                target_compile_options(${TARGET} PRIVATE -O3)
            endif()
        endif()
        
       # Add these specific library targets to your existing target_link_libraries block
target_link_libraries(${TARGET} PUBLIC 
    leveldb 
    secp256k1 
    evmc::evmc
)

target_include_directories(${TARGET} PUBLIC 
    ${leveldb_SOURCE_DIR}/include
    ${secp256k1_SOURCE_DIR}/include
    ${secp256k1_BINARY_DIR}/include # ADD THIS: some headers are generated here
    ${evmc_SOURCE_DIR}/include
)

        # ... rest of your target_include_directories and target_link_libraries ...
    endif()
endmacro()
