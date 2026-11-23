CMAKE_MINIMUM_REQUIRED(VERSION 3.27.2)

project(HTGSRasterizer LANGUAGES CUDA CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
set(CMAKE_CUDA_STANDARD 20)

set(CUDA_BACKEND_DIR ${CMAKE_CURRENT_SOURCE_DIR}/HTGS/HTGSCudaBackend/HTGSCudaBackend)

file(GLOB SOURCES
    "${CUDA_BACKEND_DIR}/rasterization/include/hybrid_blend/*.h"
    "${CUDA_BACKEND_DIR}/rasterization/include/hybrid_blend/kernels/*.h"
    "${CUDA_BACKEND_DIR}/rasterization/include/kernel_utils.cuh"
    "${CUDA_BACKEND_DIR}/rasterization/include/rasterization_utils.h"
    "${CUDA_BACKEND_DIR}/rasterization/include/shared_kernels.cuh"
    "${CUDA_BACKEND_DIR}/rasterization/src/hybrid_blend/*.cu"
    "${CUDA_BACKEND_DIR}/rasterization/src/shared_kernels.cu"
    "${CUDA_BACKEND_DIR}/utils/*.h"
)
add_library(HTGSRasterizer STATIC ${SOURCES})

set(HTGSRasterizer_INCLUDE_DIRS
    ${CMAKE_CURRENT_SOURCE_DIR}/HTGS/HTGSCudaBackend
    PARENT_SCOPE
)
target_include_directories(HTGSRasterizer PUBLIC
    ${CUDA_BACKEND_DIR}/rasterization/include
    ${CUDA_BACKEND_DIR}/utils
)
set_target_properties(HTGSRasterizer PROPERTIES CUDA_ARCHITECTURES "86")
set_target_properties(HTGSRasterizer PROPERTIES LINKER_LANGUAGE CUDA)
set_target_properties(HTGSRasterizer PROPERTIES FOLDER "extlibs")

option(VRHTGS_CUDA_LINE_INFO "Embed CUDA source line info (compute-sanitizer, Nsight; embeds absolute source paths)" ON)
target_compile_options(HTGSRasterizer PRIVATE $<$<COMPILE_LANGUAGE:CUDA>:
  $<$<BOOL:${VRHTGS_CUDA_LINE_INFO}>:--generate-line-info>
  --use_fast_math
>)
