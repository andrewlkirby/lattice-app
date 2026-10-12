# Warning and code-generation settings for Lattice's own targets (never applied to JUCE or other
# third-party sources). See docs/09-performance-charter.md §5.

option(LATTICE_WARNINGS_AS_ERRORS "Treat warnings as errors (CI turns this on)" OFF)

function(lattice_set_warnings target)
    if(MSVC)
        # C4324 ("structure was padded due to alignment specifier") is the intended effect of the
        # alignas(64) the charter asks for on data shared between threads.
        target_compile_options(${target} PRIVATE /W4 /permissive- /Zc:__cplusplus /utf-8 /wd4324)
        if(LATTICE_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE /WX)
        endif()
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wshadow -Wnon-virtual-dtor
                                                 -Wcast-align -Woverloaded-virtual)
        if(LATTICE_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    endif()
endfunction()

# Release code generation (applies to every target, JUCE included): -O2 rather than -O3, LTO,
# an x86-64-v2 baseline, and no global fast-math. Wider SIMD (AVX2/AVX-512) is chosen at run time
# per kernel, never by a global flag. MSVC has no x86-64-v2 switch; its SSE2 default is kept.
macro(lattice_set_global_codegen)
    if(NOT MSVC)
        set(CMAKE_C_FLAGS_RELEASE "-O2 -DNDEBUG")
        set(CMAKE_CXX_FLAGS_RELEASE "-O2 -DNDEBUG")
        if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|amd64")
            add_compile_options(-march=x86-64-v2)
        endif()
    endif()

    include(CheckIPOSupported)
    check_ipo_supported(RESULT _lattice_ipo_ok OUTPUT _lattice_ipo_msg LANGUAGES C CXX)
    if(_lattice_ipo_ok)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
    else()
        message(WARNING "LTO not available: ${_lattice_ipo_msg}")
    endif()
endmacro()
