set(SIBR_PROJECT "htgs-renderer")
project(sibr_${SIBR_PROJECT} LANGUAGES CXX CUDA)
enable_language(CUDA)

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/VRHTGSCudaBackend)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/HTGSCudaBackend)

find_package(CUDAToolkit REQUIRED)

file(GLOB SOURCES "*.cpp" "*.h" "*.hpp")
source_group("Source Files" FILES ${SOURCES})

file(GLOB SHADERS "shaders/*.frag" "shaders/*.vert" "shaders/*.geom")
source_group("Source Files\\shaders" FILES ${SHADERS})

file(GLOB RESOURCES "fonts/*.ttf")
source_group("Resources Files" FILES ${RESOURCES})

file(GLOB SOURCES "*.cpp" "*.h" "*.hpp" "shaders/*.frag" "shaders/*.vert" "shaders/*.geom")

## Specify target rules
add_library(${PROJECT_NAME} STATIC ${SOURCES})

include_directories(${Boost_INCLUDE_DIRS} ${CMAKE_CUDA_TOOLKIT_INCLUDE_DIRECTORIES} ${VRHTGSRasterizer_INCLUDE_DIRS} ${HTGSRasterizer_INCLUDE_DIRS} .)
target_include_directories(${PROJECT_NAME} PUBLIC ${VRHTGSRasterizer_INCLUDE_DIRS} ${HTGSRasterizer_INCLUDE_DIRS} .)
target_link_libraries(${PROJECT_NAME}
    CUDA::cudart
    ${Boost_LIBRARIES}
    ${ASSIMP_LIBRARIES}
    ${GLEW_LIBRARIES}
    ${OPENGL_LIBRARIES}
    ${OpenCV_LIBRARIES}
    glfw
    sibr_system
    sibr_view
    sibr_assets
    sibr_renderer
    VRHTGSRasterizer
    HTGSRasterizer
)

set_target_properties(${PROJECT_NAME} PROPERTIES FOLDER "projects/${SIBR_PROJECT}/renderer")

## High level macro to install in an homogen way all our ibr targets
include(install_runtime)
ibr_install_target(${PROJECT_NAME}
    INSTALL_PDB                         ## mean install also MSVC IDE *.pdb file (DEST according to target type)
    SHADERS ${SHADERS}
    RSC_FOLDER ${SIBR_PROJECT}
    RESOURCES  ${RESOURCES}

    #STANDALONE  ${INSTALL_STANDALONE}   ## mean call install_runtime with bundle dependencies resolution
    COMPONENT   ${PROJECT_NAME}_install ## will create custom target to install only this project
)
