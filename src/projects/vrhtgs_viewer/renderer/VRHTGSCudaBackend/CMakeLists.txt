CMAKE_MINIMUM_REQUIRED(VERSION 3.27.2)

project(VRHTGSRasterizer LANGUAGES CUDA CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CUDA_STANDARD 20)
set(CMAKE_CUDA_FLAGS "${CMAKE_CUDA_FLAGS} --expt-relaxed-constexpr")

set(CUDA_BACKEND_DIR ${CMAKE_CURRENT_SOURCE_DIR}/VRHTGS/VRHTGSCudaBackend/VRHTGSCudaBackend)

# Only the plain CUDA sources (the *_api.cu files are PyTorch bindings).
# inference_stereo.cu is left out because the viewer only uses mono inference.
file(GLOB SOURCES
    "${CUDA_BACKEND_DIR}/rasterization/include/config.h"
    "${CUDA_BACKEND_DIR}/rasterization/include/inference.h"
    "${CUDA_BACKEND_DIR}/rasterization/include/kernels/*.cuh"
    "${CUDA_BACKEND_DIR}/rasterization/include/utils/*.h"
    "${CUDA_BACKEND_DIR}/rasterization/include/utils/*.cuh"
    "${CUDA_BACKEND_DIR}/rasterization/src/inference.cu"
    "${CUDA_BACKEND_DIR}/visualization/include/enums.h"
    "${CUDA_BACKEND_DIR}/visualization/include/visualization.h"
    "${CUDA_BACKEND_DIR}/visualization/include/visualization_config.h"
    "${CUDA_BACKEND_DIR}/visualization/include/kernels/*.cuh"
    "${CUDA_BACKEND_DIR}/visualization/src/visualization.cu"
    "${CUDA_BACKEND_DIR}/utils/*.h"
)
list(FILTER SOURCES EXCLUDE REGEX ".*torch_utils\\.h$")
add_library(VRHTGSRasterizer STATIC ${SOURCES})

set(VRHTGSRasterizer_INCLUDE_DIRS
    ${CMAKE_CURRENT_SOURCE_DIR}/VRHTGS/VRHTGSCudaBackend
    PARENT_SCOPE
)
target_include_directories(VRHTGSRasterizer BEFORE PUBLIC
    ${CUDA_BACKEND_DIR}/rasterization/include
    ${CUDA_BACKEND_DIR}/visualization/include
    ${CUDA_BACKEND_DIR}/utils
)
set(VRHTGS_CUDA_ARCHITECTURES "native" CACHE STRING "CUDA architectures for the HTGS/VRHTGS backends")
set_target_properties(VRHTGSRasterizer PROPERTIES CUDA_ARCHITECTURES "${VRHTGS_CUDA_ARCHITECTURES}")
set_target_properties(VRHTGSRasterizer PROPERTIES LINKER_LANGUAGE CUDA)
set_target_properties(VRHTGSRasterizer PROPERTIES FOLDER "extlibs")

option(VRHTGS_CUDA_LINE_INFO "Embed CUDA source line info (compute-sanitizer, Nsight; embeds absolute source paths)" ON)
target_compile_options(VRHTGSRasterizer PRIVATE $<$<COMPILE_LANGUAGE:CUDA>:
  $<$<BOOL:${VRHTGS_CUDA_LINE_INFO}>:--generate-line-info>
  --use_fast_math
>)
