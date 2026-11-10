project(SIBR_FoveatedHTGSViewer_App)

file(GLOB SOURCES "*.cpp" "*.h" "*.hpp")
source_group("Source Files" FILES ${SOURCES})

find_package(yaml-cpp CONFIG REQUIRED)

add_executable(${PROJECT_NAME} ${SOURCES})
target_link_libraries(${PROJECT_NAME}
    ${ASSIMP_LIBRARIES}
    ${Boost_LIBRARIES}
    ${GLEW_LIBRARIES}
    ${OpenCV_LIBRARIES}
    ${OPENGL_LIBRARIES}
    OpenMP::OpenMP_CXX
    yaml-cpp::yaml-cpp
    sibr_assets
    sibr_htgs-renderer
    sibr_openxr
    sibr_renderer
    sibr_view
)
set_target_properties(${PROJECT_NAME} PROPERTIES FOLDER "projects/htgs/apps")

## High level macro to install in an homogen way all our ibr targets
include(install_runtime)
ibr_install_target(${PROJECT_NAME}
    INSTALL_PDB                         ## mean install also MSVC IDE *.pdb file (DEST according to target type)
    RESOURCES      ${RESOURCES}
    RSC_FOLDER     "htgs"
    STANDALONE  ${INSTALL_STANDALONE}   ## mean call install_runtime with bundle dependencies resolution
    COMPONENT   ${PROJECT_NAME}_install ## will create custom target to install only this project
)
