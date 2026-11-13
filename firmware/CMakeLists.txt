cmake_minimum_required(VERSION 3.16)

# Must be set before project.cmake so the shared C library is a component.
set(EXTRA_COMPONENT_DIRS "../common")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(cango)
