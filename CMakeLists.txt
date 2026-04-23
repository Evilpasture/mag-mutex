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

# Make OpenMP optional (Default OFF to keep core light)
option(ENABLE_OPENMP "Enable OpenMP for benchmarks" OFF)

if(ENABLE_OPENMP)
    find_package(OpenMP)
    if(NOT OpenMP_C_FOUND)
        message(FATAL_ERROR "OpenMP requested but not found.")
    endif()
endif()

# Check for Interprocedural Optimization (LTO)
include(CheckIPOSupported)
check_ipo_supported(RESULT supported OUTPUT error)

# ==============================================================================
# 2. CORE LIBRARY (mag_mutex_lib)
# ==============================================================================
add_library(mag_mutex_lib STATIC 
    src/mag_mutex.c
    src/mag_thread.c
)
target_include_directories(mag_mutex_lib PUBLIC include)
target_link_libraries(mag_mutex_lib PRIVATE Threads::Threads)

# Compilation Flags
if(CMAKE_C_COMPILER_ID MATCHES "Clang|AppleClang|GNU")
    target_compile_options(mag_mutex_lib PRIVATE 
        -Wall -Wextra -Wpedantic -Wshadow -Wcast-align 
        -Wstrict-prototypes -Wmissing-prototypes -Wimplicit-fallthrough
        -fno-stack-protector  # Required for Fiber stack swaps
    )

    if(NOT APPLE)
        target_compile_options(mag_mutex_lib PRIVATE -fno-plt)
    endif()

    if(CMAKE_C_COMPILER_ID MATCHES "Clang|AppleClang")
        target_compile_options(mag_mutex_lib PRIVATE 
            -Watomic-implicit-seq-cst -Wthread-safety -Werror=atomic-implicit-seq-cst
        )
        set(LTO_FLAG "-flto=thin")
    else()
        set(LTO_FLAG "-flto=auto")
    endif()

    target_compile_options(mag_mutex_lib PRIVATE -O3 ${LTO_FLAG} -fomit-frame-pointer)
    target_link_options(mag_mutex_lib PRIVATE ${LTO_FLAG})

    # Architecture Tuning
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
        target_compile_options(mag_mutex_lib PRIVATE -march=native)
    else()
        target_compile_options(mag_mutex_lib PRIVATE -mcpu=native)
    endif()
endif()

if(supported)
    set_property(TARGET mag_mutex_lib PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
endif()

# ==============================================================================
# 3. TEST SUITE & BENCHMARKS
# ==============================================================================
file(GLOB TEST_SOURCES "tests/*.c")

foreach(test_source ${TEST_SOURCES})
    get_filename_component(test_name ${test_source} NAME_WE)
    
    # Check if this is an OpenMP test and if we have OpenMP enabled
    if(test_name STREQUAL "bench_omp" AND NOT ENABLE_OPENMP)
        message(STATUS "Skipping ${test_name} (OpenMP not enabled)")
        continue() # Skip this iteration of the loop
    endif()
    
    add_executable(${test_name} ${test_source})
    target_link_libraries(${test_name} PRIVATE mag_mutex_lib Threads::Threads)
    target_include_directories(${test_name} PRIVATE tests)
    
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
option(ENABLE_ASAN "Enable Address Sanitizer" ON)

if(ENABLE_TSAN)
    add_compile_options(-fsanitize=thread -g)
    add_link_options(-fsanitize=thread)
endif()

if(ENABLE_ASAN)
    # Note: ASan requires fno-omit-frame-pointer for readable traces
    add_compile_options(-fsanitize=address -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=address)
endif()