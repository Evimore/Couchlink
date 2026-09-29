cmake_minimum_required(VERSION 3.20)
project(Couchlink VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(COUCHLINK_BUILD_HOST "Build couchlink-host and couchlink-sim" ON)
option(COUCHLINK_BUILD_TESTS "Build unit tests" ON)
set(COUCHLINK_VERSION "${PROJECT_VERSION}" CACHE STRING "Version the programs report (release builds set it)")

if(MSVC)
  set(COUCHLINK_WARNINGS /W4 /permissive-)
else()
  set(COUCHLINK_WARNINGS -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
endif()

if(COUCHLINK_BUILD_TESTS)
  enable_testing()
endif()

add_subdirectory(core)
if(COUCHLINK_BUILD_HOST)
  add_subdirectory(host)
endif()
