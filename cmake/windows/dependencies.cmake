# Copyright (C) 2020, Inria
# GRAPHDECO research group, https://team.inria.fr/graphdeco
# All rights reserved.
# 
# This software is free for non-commercial, research and evaluation use 
# under the terms of the LICENSE.md file.
# 
# For inquiries contact sibr@inria.fr and/or George.Drettakis@inria.fr


## Included once for all sub project.
## It contain the whole cmake instructions to find necessary common dependencies.
## 3rdParty (provided by sibr_addlibrary win3rdParty or from external packages) are then available in cmake sub projects.
##
## Do not include this file more than once but you can modify it to fit to your own project.
## So please, read it carefully because you can use on of these dependencies for your project or appen new one.
##
## As it is included after camke options, you can use conditional if(<CMAKE_PROJ_OPT>)/endif() to encapsulate your 3rdParty.
##

## win3rdParty function allowing to auto check/download/update binaries dependencies for current windows compiler
## Please open this file in order to get more documentation and usage examples.
include(Win3rdParty)

include(sibr_library)

Win3rdPartyGlobalCacheAction()

############
## Find OpenGL
############
find_package(OpenGL REQUIRED)


############
## Find GLEW
############
set(GLEW_VERBOSE ON)
find_package(GLEW REQUIRED)
IF(GLEW_FOUND)
    include_directories(${GLEW_INCLUDE_DIR})
ELSE(GLEW_FOUND)
    message("GLEW not found. Set GLEW_DIR to base directory of GLEW.")
ENDIF(GLEW_FOUND)


##############
## Find ASSIMP
##############
find_package(ASSIMP REQUIRED)
include_directories(${ASSIMP_INCLUDE_DIR})


################
## Find FFMPEG
################
find_package(FFMPEG QUIET)
include_directories(${FFMPEG_INCLUDE_DIR})


###################
## Find embree3
###################
find_package(embree 3 CONFIG REQUIRED)


###################
## Find eigen3
###################
find_package(Eigen3 REQUIRED)
include_directories(${EIGEN3_INCLUDE_DIR})
add_definitions(-DEIGEN_INITIALIZE_MATRICES_BY_ZERO)


#############
## Find Boost
#############
set(Boost_REQUIRED_COMPONENTS "system;chrono;date_time" CACHE INTERNAL "Boost Required Components")

find_package(Boost 1.71.0 REQUIRED COMPONENTS ${Boost_REQUIRED_COMPONENTS})

if(WIN32)
    add_compile_options("$<$<COMPILE_LANGUAGE:CXX>:/EHsc>")
endif()

if(Boost_LIB_DIAGNOSTIC_DEFINITIONS)
    add_definitions(${Boost_LIB_DIAGNOSTIC_DEFINITIONS})
endif()

include_directories(${BOOST_INCLUDEDIR} ${Boost_INCLUDE_DIRS})
link_directories(${BOOST_LIBRARYDIR} ${Boost_LIBRARY_DIRS})


##############
## Find OpenMP
##############
find_package(OpenMP)


###########################
## Find native file dialogs
###########################
find_package(nfd)


##############
## Find OpenCV
##############
if (WIN32)
	if (${MSVC_TOOLSET_VERSION} EQUAL 143)
		MESSAGE("SPECIAL OPENCV HANDLING")
		set(opencv_set_arguments 
		CHECK_CACHED_VAR OpenCV_DIR PATH "install" ## see OpenCVConfig.cmake
	    )
	elseif (MSVC11 OR MSVC12)
	    set(opencv_set_arguments 
		CHECK_CACHED_VAR OpenCV_DIR PATH "opencv/build" ## see OpenCVConfig.cmake
	    )
	elseif (MSVC14)
	    set(opencv_set_arguments 
		CHECK_CACHED_VAR OpenCV_DIR PATH "opencv-4.5.0/build" ## see OpenCVConfig.cmake
	    )
	else ()
	    message("There is no provided OpenCV library for your compiler, relying on find_package to find it")
	endif()
else()
	    message("There is no provided OpenCV library for your compiler, relying on find_package to find it")
endif()

find_package(OpenCV REQUIRED) ## Use directly the OpenCVConfig.cmake provided

add_definitions(-DOPENCV_TRAITS_ENABLE_DEPRECATED)

if(OpenCV_INCLUDE_DIRS)
    foreach(inc ${OpenCV_INCLUDE_DIRS})
        if(NOT EXISTS ${inc})
            set(OpenCV_INCLUDE_DIR "" CACHE PATH "additional custom include DIR (in case of trouble to find it (fedora 17 opencv package))")
        endif()
    endforeach()
    if(OpenCV_INCLUDE_DIR)
        list(APPEND OpenCV_INCLUDE_DIRS ${OpenCV_INCLUDE_DIR})
        include_directories(${OpenCV_INCLUDE_DIRS})
    endif()
endif()

###################
## Find GLFW
###################
find_package(glfw3 REQUIRED)


###################
## Find ImGui
###################
find_package(imgui REQUIRED)


###################
## Find nanoflann
###################
find_package(nanoflann REQUIRED)


###################
## Find Picojson and RapidXML
###################
find_path(PICOJSON_INCLUDE_DIRS "picojson/picojson.h")

find_path(RAPIDXML_INCLUDE_DIRS "rapidxml/rapidxml.hpp")


###################
## Find OpenXR
###################
find_package(OpenXR REQUIRED)


###################
## Find mrf and xatlas
###################
sibr_gitlibrary(TARGET mrf
    GIT_REPOSITORY 	"https://gitlab.inria.fr/sibr/libs/mrf.git"
    GIT_TAG			"564e5e0b395c788d2f8b2cf4f879fed2493faea7"
)

sibr_gitlibrary(TARGET xatlas
    GIT_REPOSITORY 	"https://gitlab.inria.fr/sibr/libs/xatlas.git"
    GIT_TAG			"0fbe06a5368da13fcdc3ee48d4bdb2919ed2a249"
    INCLUDE_DIRS 	"source/xatlas"
)

Win3rdPartyGlobalCacheAction()
