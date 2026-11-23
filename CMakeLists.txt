# Copyright (C) 2020, Inria
# GRAPHDECO research group, https://team.inria.fr/graphdeco
# All rights reserved.
# 
# This software is free for non-commercial, research and evaluation use 
# under the terms of the LICENSE.md file.
# 
# For inquiries contact sibr@inria.fr and/or George.Drettakis@inria.fr

cmake_policy(SET CMP0091 NEW)
CMAKE_MINIMUM_REQUIRED(VERSION 3.27.2)

set (CMAKE_SYSTEM_VERSION 10.0.15063.0 CACHE INTERNAL "Cmake system version" FORCE)
PROJECT(sibr_projects)

set(REQUIRED_VERSION "3.27.2")
set(CHECKED_VERSION "3.27.2")

if (CMAKE_VERSION VERSION_LESS REQUIRED_VERSION)
    message(WARNING "Deprecated version of cmake. Please update to at least ${REQUIRED_VERSION} (${CHECKED_VERSION} recommended).")
elseif (CMAKE_VERSION VERSION_GREATER CHECKED_VERSION)
	message(WARNING "Untested version of cmake. If you checked everything is working properly, please update ${CHECKED_VERSION} in the main CmakeLists.txt with the version you tested.")
endif()

## Set MSVC runtime to static
set_property(GLOBAL PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

## Include cmake stuff (functions/macros) : Modules files
list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake/windows)
list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake/windows/Modules)
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

include(git_describe)
git_describe(GIT_BRANCH SIBR_CORE_BRANCH GIT_COMMIT_HASH SIBR_CORE_COMMIT_HASH GIT_TAG SIBR_CORE_TAG GIT_VERSION SIBR_CORE_VERSION)

message(STATUS "SIBR version :\n BRANCH ${SIBR_CORE_BRANCH}\n COMMIT_HASH ${SIBR_CORE_COMMIT_HASH}\n TAG ${SIBR_CORE_TAG}\n VERSION ${SIBR_CORE_VERSION}")

## Allow C++23 + other flags
get_filename_component(currentBuildTool ${CMAKE_BUILD_TOOL} NAME_WE)	# tool that can launch the native build system. returned value may be the full path
add_definitions(-D_USE_MATH_DEFINES -DIMGUI_DEFINE_MATH_OPERATORS)# /D_ITERATOR_DEBUG_LEVEL=1 because you need all external DLl to compile with this flag too
file(TO_NATIVE_PATH "${CMAKE_SOURCE_DIR}" SIBR_TRIM_PREFIX)
add_compile_options("$<$<COMPILE_LANGUAGE:CXX>:/d1trimfile:${SIBR_TRIM_PREFIX}>"
                    "$<$<COMPILE_LANGUAGE:CUDA>:-Xcompiler=/d1trimfile:${SIBR_TRIM_PREFIX}>")
set(CMAKE_CONFIGURATION_TYPES "Debug;Release" CACHE STRING "" FORCE)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(INSTALL_STANDALONE ON)

## Set default build output binaries (used also in sub CMakeLists.txt) :
set(BIN_BUILT_DIR "bin")
set(ARCHI_BUILT_DIR "x64")
set(LIB_BUILT_DIR "lib64")

option(SEPARATE_CONFIGURATIONS "Clearer separation between configurations" OFF)
SET(CMAKE_INSTALL_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/install)
SET(CMAKE_INSTALL_PREFIX ${CMAKE_INSTALL_ROOT})

if(DEFINED CMAKE_BUILD_TYPE) ## for mono config type (make/nmake/ninja based)
	if(${CMAKE_BUILD_TYPE} MATCHES "Debug")
		set(CMAKE_DEBUG_POSTFIX "_d")
	elseif(${CMAKE_BUILD_TYPE} MATCHES "RelWithDebInfo")
		set(CMAKE_RELWITHDEBINFO_POSTFIX "_rwdi")
	elseif(${CMAKE_BUILD_TYPE} MATCHES "MinSizeRel")
		set(CMAKE_MINSIZEREL_POSTFIX "_msr")
	elseif(${CMAKE_BUILD_TYPE} MATCHES "Release")
		set(CMAKE_RELEASE_POSTFIX "")
	endif()

	if(SEPARATE_CONFIGURATIONS)
		SET(CMAKE_INSTALL_PREFIX_${CMAKE_BUILD_TYPE} 	${CMAKE_INSTALL_ROOT}/${CMAKE_BUILD_TYPE})
	else()
		SET(CMAKE_INSTALL_PREFIX_${CMAKE_BUILD_TYPE} 	${CMAKE_INSTALL_ROOT})
	endif()

	MESSAGE(STATUS "Install path set to ${CMAKE_INSTALL_PREFIX}.")
	SET(CMAKE_OUTPUT_LIB_${CMAKE_BUILD_TYPE} 	${CMAKE_INSTALL_PREFIX_${CMAKE_BUILD_TYPE}}/lib)
	SET(CMAKE_OUTPUT_BIN_${CMAKE_BUILD_TYPE} 	${CMAKE_INSTALL_PREFIX_${CMAKE_BUILD_TYPE}}/bin)

	set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_${CMAKE_BUILD_TYPE} 	${CMAKE_OUTPUT_LIB_${CMAKE_BUILD_TYPE}})
	set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_${CMAKE_BUILD_TYPE} 	${CMAKE_OUTPUT_LIB_${CMAKE_BUILD_TYPE}})
	set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${CMAKE_BUILD_TYPE} 	${CMAKE_OUTPUT_BIN_${CMAKE_BUILD_TYPE}})
	set(CMAKE_PDB_OUTPUT_DIRECTORY_${CMAKE_BUILD_TYPE} 		${CMAKE_OUTPUT_BIN_${CMAKE_BUILD_TYPE}})
endif()
foreach(CONFIG_TYPES ${CMAKE_CONFIGURATION_TYPES}) ## for multi config types (MSVC based)
	string(TOUPPER ${CONFIG_TYPES} CONFIG_TYPES_UC)
	if(${CONFIG_TYPES} MATCHES "Debug")
		set(CMAKE_DEBUG_POSTFIX "_d")
	elseif(${CONFIG_TYPES} MATCHES "RelWithDebInfo")
		set(CMAKE_RELWITHDEBINFO_POSTFIX "_rwdi")
	elseif(${CONFIG_TYPES} MATCHES "MinSizeRel")
		set(CMAKE_MINSIZEREL_POSTFIX "_msr")
	elseif(${CMAKE_BUILD_TYPE} MATCHES "Release")
		set(CMAKE_RELEASE_POSTFIX "")
	endif()

	if(SEPARATE_CONFIGURATIONS)
		SET(CMAKE_INSTALL_PREFIX_${CONFIG_TYPES_UC} ${CMAKE_INSTALL_ROOT}/${CONFIG_TYPES})
	else()
		SET(CMAKE_INSTALL_PREFIX_${CONFIG_TYPES_UC} ${CMAKE_INSTALL_ROOT})
	endif()
	
	MESSAGE(STATUS "Install path for ${CONFIG_TYPES} set to ${CMAKE_INSTALL_PREFIX_${CONFIG_TYPES_UC}}.")
	SET(CMAKE_OUTPUT_LIB_${CONFIG_TYPES_UC} 	${CMAKE_INSTALL_PREFIX_${CONFIG_TYPES_UC}}/lib)
	SET(CMAKE_OUTPUT_BIN_${CONFIG_TYPES_UC} 	${CMAKE_INSTALL_PREFIX_${CONFIG_TYPES_UC}}/bin)

	set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_${CONFIG_TYPES_UC} 	${CMAKE_OUTPUT_LIB_${CONFIG_TYPES_UC}})
	set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_${CONFIG_TYPES_UC} 	${CMAKE_OUTPUT_LIB_${CONFIG_TYPES_UC}})
	set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${CONFIG_TYPES_UC} 	${CMAKE_OUTPUT_BIN_${CONFIG_TYPES_UC}})
	set(CMAKE_PDB_OUTPUT_DIRECTORY_${CONFIG_TYPES_UC} 		${CMAKE_OUTPUT_BIN_${CONFIG_TYPES_UC}})
endforeach()


set(SIBR_PROGRAMARGS "" CACHE STRING "Default program arguments used in Visual Studio target properties")
if ("${SIBR_PROGRAMARGS}" STREQUAL "")
  if (DEFINED ENV{SIBR_PROGRAMARGS})
    set(SIBR_PROGRAMARGS "$ENV{SIBR_PROGRAMARGS}" CACHE STRING "Default program arguments used in Visual Studio target properties" FORCE)
    message( STATUS "Using program options found in environment variable 'SIBR_PROGRAMARGS' => '${SIBR_PROGRAMARGS}'")
  else()
    message(
      "Note you can provide default program options for Visual Studio target properties by either setting"
      " a value for the cmake cached variable 'SIBR_PROGRAMARGS' or by setting a new environment "
      "variable 'SIBR_PROGRAMARGS'")
  endif()
endif()

add_custom_target(PREBUILD ALL)

## Include all projects
set(SIBR_PROJECTS_SAMPLES_SUBPAGE_REF "")
set(SIBR_PROJECTS_OURS_SUBPAGE_REF "")
set(SIBR_PROJECTS_TOOLBOX_SUBPAGE_REF "")
set(SIBR_PROJECTS_OTHERS_SUBPAGE_REF "")
set(SIBR_PROJECTS_SAMPLES_REF_REF "")
set(SIBR_PROJECTS_OURS_REF_REF "")
set(SIBR_PROJECTS_TOOLBOX_REF_REF "")
set(SIBR_PROJECTS_OTHERS_REF_REF "")
ADD_SUBDIRECTORY(src)
