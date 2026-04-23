#pragma once

// Architecture detection
#if defined(__x86_64__) || defined(_M_X64)
#   define ARCH_X64
#elif defined(__aarch64__) || defined(_M_ARM64)
#   define ARCH_ARM64
#endif

// OS detection
#if defined(_WIN32)
#   define OS_WIN
#elif defined(__APPLE__)
#   define OS_APPLE
#endif

// Symbol mangling: macOS/iOS require a leading underscore for C symbols
#if defined(OS_APPLE)
#   define CSYM(name) _##name
#else
#   define CSYM(name) name
#endif

// Function declaration macros
#if defined(OS_WIN)
#   define ASM_FUNC_START(name) .global name; .text; .align 16; name:
#   define ASM_FUNC_END(name)
#else
#   define ASM_FUNC_START(name) .global name; .text; .align 16; .type name, @function; name:
#   define ASM_FUNC_END(name) .size name, .-name
#endif