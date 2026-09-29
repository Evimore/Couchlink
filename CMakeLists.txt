cmake_minimum_required(VERSION 3.20)
project(InputLine VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(INPUTLINE_BUILD_HOST "Build inputline-host and inputline-sim" ON)
option(INPUTLINE_BUILD_TESTS "Build unit tests" ON)
set(INPUTLINE_VERSION "${PROJECT_VERSION}" CACHE STRING "Version the programs report (release builds set it)")

if(MSVC)
  set(INPUTLINE_WARNINGS /W4 /permissive-)
else()
  set(INPUTLINE_WARNINGS -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
endif()

if(INPUTLINE_BUILD_TESTS)
  enable_testing()
endif()

add_subdirectory(core)
if(INPUTLINE_BUILD_HOST)
  add_subdirectory(host)
endif()
