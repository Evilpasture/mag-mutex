cmake_minimum_required(VERSION 3.21)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

project(MagMutex 
    VERSION 1.0.0 
    LANGUAGES C
)

function(enforce_modern_compiler)
    # --- ANTI-MSBUILD / ANTI-VS GENERATOR GUARD ---
    # MSBuild is the bane of build systems. We need a modern/reliable generator (Ninja/Make).
    if(CMAKE_GENERATOR MATCHES "Visual Studio")
        message(
            FATAL_ERROR
            "\n ################################################################\n"
            "  GENERATOR ERROR - HALTED: Visual Studio / MSBuild detected.\n"
            " ################################################################\n"
            "  Culverin's build system is incompatible with the \n"
            "  Visual Studio project system's rigid toolchain handling.\n"
            "  - The reason being: The MSBuild generator will default to cl.exe, \n"
            "  which is currently forbidden from compiling this project. \n"
            "\n"
            "  I DEMAND you to use the 'Ninja' generator for Clang to work correctly.\n"
            "\n"
            "  Instruction:\n"
            "  1. Install Ninja (pip install ninja or winget install ninja)\n"
            "  2. Wipe your 'build' folder.\n"
            "  3. Run: cmake -G \"Ninja\" -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ ..\n"
            " ################################################################\n"
        )
    endif()
    if(CMAKE_C_COMPILER_ID AND NOT CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
        message(STATUS "Detected Compiler ID: ${CMAKE_C_COMPILER_ID}")
        message(STATUS "Culverin: MSVC detected. Searching for alternatives...")

        find_program(CLANG_PATH NAMES clang clang-cl)
        find_program(GCC_PATH NAMES gcc)

        set(HELP_MSG
            "\n################################################################\n"
        )
        string(
            APPEND HELP_MSG
            " Guard triggered: MSVC (cl.exe) is incapable of compilation.\n"
        )
        string(
            APPEND HELP_MSG
            "################################################################\n"
        )
        string(
            APPEND HELP_MSG
            " C23 features and Jolt optimization require Clang or GCC.\n\n"
        )

        if(CLANG_PATH OR GCC_PATH)
            string(APPEND HELP_MSG " Alternatives on your computer:\n")
            if(CLANG_PATH)
                string(
                    APPEND HELP_MSG
                    "  - Clang's emplacement: ${CLANG_PATH}\n"
                )
            endif()
            if(GCC_PATH)
                string(APPEND HELP_MSG "  - GCC's emplacement:   ${GCC_PATH}\n")
            endif()
            string(
                APPEND HELP_MSG
                "\n Instruction: Decimate your 'build' folder and run:\n"
            )
            string(
                APPEND HELP_MSG
                " cmake -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ ..\n"
            )
        else()
            string(
                APPEND HELP_MSG
                " NO SUITABLE COMPILER FOUND in system PATH.\n"
            )
            string(
                APPEND HELP_MSG
                " Please download and install LLVM (Clang) from:\n"
            )
            string(
                APPEND HELP_MSG
                " https://github.com/llvm/llvm-project/releases\n"
            )
        endif()

        string(
            APPEND HELP_MSG
            "################################################################\n"
        )
        message(FATAL_ERROR "${HELP_MSG}")
    endif()
endfunction()

enforce_modern_compiler()

set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)

# ==============================================================================
# 1. DEPENDENCIES & LIBRARIES
# ==============================================================================

find_package(Threads REQUIRED)

# The Core Mutex Library
add_library(mag_mutex_lib STATIC src/mag_mutex.c)
target_include_directories(mag_mutex_lib PUBLIC include)
target_link_libraries(mag_mutex_lib PRIVATE Threads::Threads)

# The Benchmark Executable
add_executable(MagMutex src/main.c)
target_link_libraries(MagMutex PRIVATE mag_mutex_lib Threads::Threads)

# ==============================================================================
# 2. HIGH-DISCIPLINE & PERFORMANCE FLAGS
# ==============================================================================

if(CMAKE_C_COMPILER_ID MATCHES "Clang|AppleClang|GNU")
    # Strictness
    target_compile_options(mag_mutex_lib PRIVATE 
        -Wall -Wextra -Wpedantic -Wshadow -Wcast-align 
        -Wstrict-prototypes -Wmissing-prototypes -Wimplicit-fallthrough
        -Watomic-implicit-seq-cst -Wthread-safety -Werror=atomic-implicit-seq-cst
    )

    # Performance
    set(LTO_FLAG "-flto=thin")
    if(CMAKE_C_COMPILER_ID STREQUAL "GNU")
        set(LTO_FLAG "-flto=auto")
    endif()

    target_compile_options(mag_mutex_lib PRIVATE -O3 ${LTO_FLAG} -fomit-frame-pointer -fno-stack-protector)
    target_link_options(MagMutex PRIVATE ${LTO_FLAG})

    # CPU-Specific (ARM vs x86)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
        target_compile_options(mag_mutex_lib PRIVATE -march=native)
    else()
        target_compile_options(mag_mutex_lib PRIVATE -mcpu=native)
    endif()
endif()

# Enable Interprocedural Optimization (IPO/LTO) globally if supported
include(CheckIPOSupported)
check_ipo_supported(RESULT supported OUTPUT error)
if(supported)
    set_property(TARGET mag_mutex_lib PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
    set_property(TARGET MagMutex PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
endif()

# ==============================================================================
# 3. DEBUG TOOLS (TSan)
# ==============================================================================
option(ENABLE_TSAN "Enable Thread Sanitizer" OFF)
if(ENABLE_TSAN)
    message(STATUS "Thread Sanitizer (TSan) enabled")
    target_compile_options(mag_mutex_lib PUBLIC -fsanitize=thread -g)
    target_link_options(mag_mutex_lib PUBLIC -fsanitize=thread)
    target_compile_options(MagMutex PRIVATE -fsanitize=thread -g)
    target_link_options(MagMutex PRIVATE -fsanitize=thread)
endif()