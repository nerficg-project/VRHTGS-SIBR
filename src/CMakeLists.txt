# Copyright (C) 2020, Inria
# GRAPHDECO research group, https://team.inria.fr/graphdeco
# All rights reserved.
#
# This software is free for non-commercial, research and evaluation use
# under the terms of the LICENSE.md file.
#
# For inquiries contact sibr@inria.fr and/or George.Drettakis@inria.fr


################################################################################
# This CMakeLists.txt manages which projects should be built and add their     #
# dependencies.                                                                #
################################################################################
set(SIBR_FOLDER "core")
set_property(GLOBAL PROPERTY PREDEFINED_TARGETS_FOLDER "")

option(BUILD_SIBR "Build core libs of SIBR (sibr_system, sibr_graphics, sibr_view, sibr_assets, ...)" ON)

#https://stackoverflow.com/questions/7787823/cmake-how-to-get-the-name-of-all-subdirectories-of-a-directory
MACRO(SUBDIRLIST result curdir)
  FILE(GLOB children RELATIVE ${curdir} ${curdir}/*)
  SET(dirlist "")
  foreach(child ${children})
    IF(IS_DIRECTORY ${curdir}/${child})
      LIST(APPEND dirlist ${child})
    ENDIF()
  endforeach()
  SET(${result} ${dirlist})
ENDMACRO()

set(SIBR_PROJECTS_FOLDER           "${CMAKE_CURRENT_SOURCE_DIR}/projects")
SUBDIRLIST(SUBDIRS ${SIBR_PROJECTS_FOLDER})

list(APPEND PROJECT_SUBFOLDERS "apps" "preprocess" "renderer" "scripts" "library")

## DEPS ##
include(include_once)

message(STATUS "\n\n****************** Handling core dependencies ******************")

include_once(dependencies)		## Map/bind 3rdParty/external dependencies packages to cmake

message(STATUS "****************************************************************\n\n")

foreach(subdir ${SUBDIRS})
  set(${subdir}_ROOT_DIR "${SIBR_PROJECTS_FOLDER}/${subdir}")
  set(PROJECT_NAME "BUILD_IBR_${subdir}")
  string(TOUPPER ${PROJECT_NAME} PROJECT_NAME)
  if(${${PROJECT_NAME}})
    foreach(PROJECT_SUBFOLDER ${PROJECT_SUBFOLDERS})
      if(EXISTS "${${subdir}_ROOT_DIR}/${PROJECT_SUBFOLDER}/cmake/Modules")
        list(APPEND CMAKE_MODULE_PATH ${${subdir}_ROOT_DIR}/${PROJECT_SUBFOLDER}/cmake/Modules)
      endif()

      if(EXISTS "${${subdir}_ROOT_DIR}/${PROJECT_SUBFOLDER}/cmake/dependencies.cmake")
        message(STATUS "********* Handling ${subdir} ${PROJECT_SUBFOLDER} dependencies *********")
        include_once("${${subdir}_ROOT_DIR}/${PROJECT_SUBFOLDER}/cmake/dependencies.cmake")

        message(STATUS "****************************************************************\n\n")
      endif()
    endforeach()
  endif()
endforeach()

include_directories(.)

if (BUILD_SIBR)
  add_subdirectory(core/system)
  add_subdirectory(core/graphics)
  add_subdirectory(core/renderer)
  add_subdirectory(core/raycaster)
  add_subdirectory(core/view)
  add_subdirectory(core/scene)
  add_subdirectory(core/assets)
  add_subdirectory(core/imgproc)
  add_subdirectory(core/video)
  add_subdirectory(core/openxr)
endif()

set(PROJECTS_ON_AT_FIRST_BUILD "vrhtgs_viewer")

foreach(subdir ${SUBDIRS})
  message(STATUS "Adding ${subdir} project")
  set(PROJECT_NAME "BUILD_IBR_${subdir}")
  string(TOUPPER ${PROJECT_NAME} PROJECT_NAME)

  if(NOT (DEFINED ${PROJECT_NAME}))
    foreach(PROJECT_SUBFOLDER ${PROJECT_SUBFOLDERS})
      if(EXISTS "${${subdir}_ROOT_DIR}/${PROJECT_SUBFOLDER}/CMakeLists.txt")
        if(subdir IN_LIST PROJECTS_ON_AT_FIRST_BUILD)
          option(${PROJECT_NAME} "Build project \"${subdir}\"" ON)
        else()
          option(${PROJECT_NAME} "Build project \"${subdir}\"" OFF)
        endif()
        break()
      endif()
    endforeach()
  endif()

  message(STATUS "${PROJECT_NAME} is ${${PROJECT_NAME}}")

  if(${${PROJECT_NAME}})
    if(EXISTS "${${subdir}_ROOT_DIR}/CMakeLists.txt")
      add_subdirectory("${${subdir}_ROOT_DIR}")
    else()
      foreach(PROJECT_SUBFOLDER ${PROJECT_SUBFOLDERS})
        if(EXISTS "${${subdir}_ROOT_DIR}/${PROJECT_SUBFOLDER}/CMakeLists.txt")
          add_subdirectory("${${subdir}_ROOT_DIR}/${PROJECT_SUBFOLDER}")
        endif()
      endforeach()
    endif()
  endif()
endforeach()
  
set(SIBR_PROJECTS_SAMPLES_SUBPAGE_REF "${SIBR_PROJECTS_SAMPLES_SUBPAGE_REF_LOCAL}" PARENT_SCOPE)
set(SIBR_PROJECTS_OURS_SUBPAGE_REF "${SIBR_PROJECTS_OURS_SUBPAGE_REF_LOCAL}" PARENT_SCOPE)
set(SIBR_PROJECTS_TOOLBOX_SUBPAGE_REF "${SIBR_PROJECTS_TOOLBOX_SUBPAGE_REF_LOCAL}" PARENT_SCOPE)
set(SIBR_PROJECTS_OTHERS_SUBPAGE_REF "${SIBR_PROJECTS_OTHERS_SUBPAGE_REF_LOCAL}" PARENT_SCOPE)
set(SIBR_PROJECTS_SAMPLES_REF_REF "${SIBR_PROJECTS_SAMPLES_REF_REF_LOCAL}" PARENT_SCOPE)
set(SIBR_PROJECTS_OURS_REF_REF "${SIBR_PROJECTS_OURS_REF_REF_LOCAL}" PARENT_SCOPE)
set(SIBR_PROJECTS_TOOLBOX_REF_REF "${SIBR_PROJECTS_TOOLBOX_REF_REF_LOCAL}" PARENT_SCOPE)
set(SIBR_PROJECTS_OTHERS_REF_REF "${SIBR_PROJECTS_OTHERS_REF_REF_LOCAL}" PARENT_SCOPE)
