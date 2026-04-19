cmake_minimum_required(VERSION 3.16)

# Build only `main` and its transitive dependencies.
set(MINIMAL_BUILD ON)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(xiao_cam_test)
