cmake_minimum_required(VERSION 3.21)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

project(MagSync 
    VERSION 1.1.0 
    LANGUAGES C
)

# ==============================================================================
# 0. COMPILER GUARD (CLANG/GCC ONLY)
# ==============================================================================
function(enforce_modern_compiler)
    if(CMAKE_GENERATOR MATCHES "Visual Studio")
        message(FATAL_ERROR "\n GENERATOR ERROR: Visual Studio/MSBuild is forbidden. Use Ninja.")
    endif()
    if(NOT CMAKE_C_COMPILER_ID MATCHES "Clang|AppleClang|GNU")
        message(FATAL_ERROR "\n COMPILER ERROR: MSVC is incapable of C23/Fiber switching.")
    endif()
endfunction()
enforce_modern_compiler()

set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)

# ==============================================================================
# 1. DEPENDENCIES & FEATURE DETECTION
# ==============================================================================
find_package(Threads REQUIRED)
find_package(OpenMP REQUIRED)

# Check for Interprocedural Optimization (LTO)
include(CheckIPOSupported)
check_ipo_supported(RESULT supported OUTPUT error)

# ==============================================================================
# 2. CORE LIBRARY (mag_core)
# ==============================================================================
add_library(mag_core STATIC 
    src/mag_mutex.c
    src/mag_thread.c
)
target_include_directories(mag_core PUBLIC include)
target_link_libraries(mag_core PRIVATE Threads::Threads)

# Compilation Flags
if(CMAKE_C_COMPILER_ID MATCHES "Clang|AppleClang|GNU")
    target_compile_options(mag_core PRIVATE 
        -Wall -Wextra -Wpedantic -Wshadow -Wcast-align 
        -Wstrict-prototypes -Wmissing-prototypes -Wimplicit-fallthrough
        -fno-stack-protector  # Required for Fiber stack swaps
    )

    if(NOT APPLE)
        target_compile_options(mag_core PRIVATE -fno-plt)
    endif()

    if(CMAKE_C_COMPILER_ID MATCHES "Clang|AppleClang")
        target_compile_options(mag_core PRIVATE 
            -Watomic-implicit-seq-cst -Wthread-safety -Werror=atomic-implicit-seq-cst
        )
        set(LTO_FLAG "-flto=thin")
    else()
        set(LTO_FLAG "-flto=auto")
    endif()

    target_compile_options(mag_core PRIVATE -O3 ${LTO_FLAG} -fomit-frame-pointer)
    target_link_options(mag_core PRIVATE ${LTO_FLAG})

    # Architecture Tuning
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
        target_compile_options(mag_core PRIVATE -march=native)
    else()
        target_compile_options(mag_core PRIVATE -mcpu=native)
    endif()
endif()

if(supported)
    set_property(TARGET mag_core PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
endif()

# ==============================================================================
# 3. TEST SUITE & BENCHMARKS
# ==============================================================================
# 1. Find all .c files in tests/
file(GLOB TEST_SOURCES "tests/*.c")

# 2. Loop ONCE to create targets
foreach(test_source ${TEST_SOURCES})
    get_filename_component(test_name ${test_source} NAME_WE)
    
    add_executable(${test_name} ${test_source})
    target_link_libraries(${test_name} PRIVATE mag_core Threads::Threads)
    target_include_directories(${test_name} PRIVATE tests) # For test_utils.h
    
    # Special case: OpenMP Benchmark needs the OpenMP runtime linked
    if(test_name STREQUAL "bench_omp")
        target_link_libraries(${test_name} PRIVATE OpenMP::OpenMP_C)
    endif()

    if(supported)
        set_property(TARGET ${test_name} PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
    endif()
endforeach()

# ==============================================================================
# 4. SANITIZERS (Toggleable Options)
# ==============================================================================
option(ENABLE_TSAN "Enable Thread Sanitizer" OFF)
option(ENABLE_ASAN "Enable Address Sanitizer" OFF)

if(ENABLE_TSAN)
    add_compile_options(-fsanitize=thread -g)
    add_link_options(-fsanitize=thread)
endif()

if(ENABLE_ASAN)
    # Note: ASan requires fno-omit-frame-pointer for readable traces
    add_compile_options(-fsanitize=address -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=address)
endif()