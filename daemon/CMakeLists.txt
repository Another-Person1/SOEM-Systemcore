cmake_minimum_required(VERSION 3.28)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE
   AND NOT CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$")
  find_program(EC_SYSTEMCORE_AARCH64_C_COMPILER aarch64-linux-gnu-gcc REQUIRED)
  find_program(EC_SYSTEMCORE_AARCH64_CXX_COMPILER aarch64-linux-gnu-g++ REQUIRED)
  set(CMAKE_SYSTEM_NAME Linux CACHE STRING "" FORCE)
  set(CMAKE_SYSTEM_PROCESSOR aarch64 CACHE STRING "" FORCE)
  set(CMAKE_C_COMPILER "${EC_SYSTEMCORE_AARCH64_C_COMPILER}" CACHE FILEPATH "" FORCE)
  set(CMAKE_CXX_COMPILER "${EC_SYSTEMCORE_AARCH64_CXX_COMPILER}" CACHE FILEPATH "" FORCE)
endif()

project(ec-systemcore-daemon VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build static libraries" FORCE)

option(LIMELIGHT_EC_WITH_WPILIB "Enable NT4 and WPILog integration" ON)
option(LIMELIGHT_EC_FULLY_STATIC "Request a fully static executable" ON)

if(LIMELIGHT_EC_FULLY_STATIC AND UNIX)
  set(CMAKE_FIND_LIBRARY_SUFFIXES ".a")
endif()

get_filename_component(SOEM_ROOT "${CMAKE_CURRENT_LIST_DIR}/../third_party/SOEM" ABSOLUTE)

if(NOT TARGET soem)
  set(SOEM_BUILD_SAMPLES OFF CACHE BOOL "" FORCE)
  add_subdirectory("../third_party/SOEM" "${CMAKE_CURRENT_BINARY_DIR}/third_party/SOEM")
endif()

add_executable(ec-systemcore-daemon
  src/main.cpp
)

set_target_properties(ec-systemcore-daemon PROPERTIES
  OUTPUT_NAME ec-systemcore-daemon
)

target_include_directories(ec-systemcore-daemon PRIVATE
  "${CMAKE_CURRENT_LIST_DIR}/include"
  "${CMAKE_CURRENT_LIST_DIR}/../third_party/SOEM"
  "${CMAKE_CURRENT_LIST_DIR}/../third_party/SOEM/soem"
  "${CMAKE_CURRENT_LIST_DIR}/../third_party/SOEM/osal"
)

find_package(Threads REQUIRED)

target_link_libraries(ec-systemcore-daemon PRIVATE
  soem
  Threads::Threads
)

if(LIMELIGHT_EC_WITH_WPILIB)
  find_package(ntcore CONFIG REQUIRED)
  find_package(wpiutil CONFIG REQUIRED)

  if(TARGET ntcore::ntcore)
    target_link_libraries(ec-systemcore-daemon PRIVATE ntcore::ntcore)
  elseif(TARGET ntcore)
    target_link_libraries(ec-systemcore-daemon PRIVATE ntcore)
  else()
    message(FATAL_ERROR "ntcore was found, but no ntcore CMake target is available")
  endif()

  if(TARGET wpiutil::wpiutil)
    target_link_libraries(ec-systemcore-daemon PRIVATE wpiutil::wpiutil)
  elseif(TARGET wpiutil)
    target_link_libraries(ec-systemcore-daemon PRIVATE wpiutil)
  else()
    message(FATAL_ERROR "wpiutil was found, but no wpiutil CMake target is available")
  endif()

  target_compile_definitions(ec-systemcore-daemon PRIVATE
    LIMELIGHT_EC_WITH_WPILIB=1
  )
else()
  target_compile_definitions(ec-systemcore-daemon PRIVATE
    LIMELIGHT_EC_WITH_WPILIB=0
  )
endif()

target_compile_options(ec-systemcore-daemon PRIVATE
  -Wall
  -Wextra
  -Wpedantic
  -Wconversion
  -Wshadow
)

target_link_options(ec-systemcore-daemon PRIVATE
  -static-libgcc
  -static-libstdc++
)

if(LIMELIGHT_EC_FULLY_STATIC)
  target_link_options(ec-systemcore-daemon PRIVATE -static)
endif()

install(TARGETS ec-systemcore-daemon RUNTIME DESTINATION usr/bin)
install(FILES ec-systemcore.service DESTINATION etc/systemd/system)
