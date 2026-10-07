# ARM astc-encoder 5.7.0 core library (compressor only; no CLI). See
# extern/README.md. Built as a static library for the Remastered importer.
#
# The SIMD level is the lowest one every CPU of the platform has: SSE2 on
# x86-64 (any desktop), NEON on arm64, plain C++ elsewhere.
find_package(Threads REQUIRED)

set(_astc_src
    astcenc_averages_and_directions.cpp
    astcenc_block_sizes.cpp
    astcenc_color_quantize.cpp
    astcenc_color_unquantize.cpp
    astcenc_compress_symbolic.cpp
    astcenc_compute_variance.cpp
    astcenc_decompress_symbolic.cpp
    astcenc_diagnostic_trace.cpp
    astcenc_entry.cpp
    astcenc_find_best_partitioning.cpp
    astcenc_ideal_endpoints_and_weights.cpp
    astcenc_image.cpp
    astcenc_integer_sequence.cpp
    astcenc_mathlib.cpp
    astcenc_mathlib_softfloat.cpp
    astcenc_partition_tables.cpp
    astcenc_percentile_tables.cpp
    astcenc_pick_best_endpoint_format.cpp
    astcenc_quantization.cpp
    astcenc_symbolic_physical.cpp
    astcenc_weight_align.cpp
    astcenc_weight_quant_xfer_tables.cpp)
list(TRANSFORM _astc_src PREPEND "${CMAKE_CURRENT_SOURCE_DIR}/Source/")

add_library(astcenc STATIC ${_astc_src})
add_library(astcenc::astcenc ALIAS astcenc)
target_include_directories(astcenc PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/Source")
target_link_libraries(astcenc PRIVATE Threads::Threads)
target_compile_definitions(astcenc PRIVATE ASTCENC_NO_INVARIANCE=1 ASTCENC_AVX=0 ASTCENC_SVE=0
    ASTCENC_POPCNT=0 ASTCENC_F16C=0)

string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" _astc_arch)
if(CMAKE_SIZEOF_VOID_P EQUAL 8 AND _astc_arch MATCHES "^(x86_64|amd64)$")
    target_compile_definitions(astcenc PRIVATE ASTCENC_NEON=0 ASTCENC_SSE=20 ASTCENC_X86_GATHERS=0)
    if(NOT MSVC)
        target_compile_options(astcenc PRIVATE -msse2 -mno-sse4.1)
    endif()
elseif(CMAKE_SIZEOF_VOID_P EQUAL 8 AND _astc_arch MATCHES "^(aarch64|arm64)$")
    target_compile_definitions(astcenc PRIVATE ASTCENC_NEON=1 ASTCENC_SSE=0)
else()
    target_compile_definitions(astcenc PRIVATE ASTCENC_NEON=0 ASTCENC_SSE=0)
endif()

if(MSVC)
    target_compile_options(astcenc PRIVATE /EHsc /wd4324)
    target_compile_definitions(astcenc PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(astcenc PRIVATE -ffp-contract=fast -fno-math-errno -w)
endif()
