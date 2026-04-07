cmake_minimum_required(VERSION 3.21)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

project(MagMutex 
    VERSION 1.0.0 
    LANGUAGES C
)

set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)

# --- 1. Find Dependencies ---

# Find system threads
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

# Find Python 3.14 Development files
# On many systems, 3.14t might be found via the 'Development.Module' component.
# We set the strategy to LOCATION to help find non-standard experimental builds.
set(Python_FIND_STRATEGY LOCATION)
find_package(Python 3.14 COMPONENTS Development REQUIRED)

message(STATUS "Found Python: ${Python_EXECUTABLE}")
message(STATUS "Python Include Dirs: ${Python_INCLUDE_DIRS}")

# --- 2. Define the Mutex Library ---

add_library(mag_mutex_lib src/mag_mutex.c)
target_include_directories(mag_mutex_lib PUBLIC include)
target_link_libraries(mag_mutex_lib PRIVATE Threads::Threads)

# --- 3. Define the Test Executable (Triple Threat) ---

add_executable(MagMutex src/main.c)

# We include Python headers as SYSTEM to suppress warnings within the Python headers themselves
target_include_directories(MagMutex SYSTEM PRIVATE ${Python_INCLUDE_DIRS})

target_link_libraries(MagMutex 
    PRIVATE 
        mag_mutex_lib 
        Threads::Threads
        Python::Python   # Links the Python library (3.14t)
)

# --- 4. High-Discipline Warnings ---

if(CMAKE_C_COMPILER_ID MATCHES "Clang|AppleClang|GNU")
    set(DISCIPLINE_FLAGS
        -Wall
        -Wextra
        -Wpedantic
        -Wshadow
        -Wcast-align
        -Wstrict-prototypes
        -Wmissing-prototypes
        -Wimplicit-fallthrough
        -Watomic-implicit-seq-cst
        -Wthread-safety
    )

    target_compile_options(mag_mutex_lib PRIVATE ${DISCIPLINE_FLAGS})
    target_compile_options(MagMutex PRIVATE ${DISCIPLINE_FLAGS})
    target_compile_options(mag_mutex_lib PRIVATE -Werror=atomic-implicit-seq-cst)
endif()

# --- 5. Thread Sanitizer (TSan) ---
option(ENABLE_TSAN "Enable Thread Sanitizer" OFF)
if(ENABLE_TSAN)
    message(STATUS "Thread Sanitizer (TSan) enabled")
    target_compile_options(mag_mutex_lib PUBLIC -fsanitize=thread -g)
    target_link_options(mag_mutex_lib PUBLIC -fsanitize=thread)
    
    target_compile_options(MagMutex PRIVATE -fsanitize=thread -g)
    target_link_options(MagMutex PRIVATE -fsanitize=thread)
endif()