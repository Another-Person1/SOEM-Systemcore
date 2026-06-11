cmake_minimum_required(VERSION 3.28)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE
   AND NOT CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$")
  set(CMAKE_SYSTEM_NAME Linux CACHE STRING "" FORCE)
  set(CMAKE_SYSTEM_PROCESSOR aarch64 CACHE STRING "" FORCE)
  set(CMAKE_C_COMPILER aarch64-linux-gnu-gcc CACHE FILEPATH "" FORCE)
  set(CMAKE_CXX_COMPILER aarch64-linux-gnu-g++ CACHE FILEPATH "" FORCE)
endif()

project(ec-systemcore VERSION 1.0.0 LANGUAGES C CXX)

add_subdirectory(daemon)
