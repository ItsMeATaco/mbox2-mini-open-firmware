cmake_minimum_required(VERSION 3.20)
# The release version: the hub compares it with the tag of the latest GitHub release.
project(mbox2 VERSION 1.0.1 LANGUAGES CXX)

set(MBOX2_REPOSITORY "ItsMeATaco/mbox2-mini-open-firmware" CACHE STRING "GitHub repository (owner/name) whose releases the hub updates from")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
  add_compile_options(/W4 /permissive- /utf-8)
  # Reproducible binaries (no build timestamps): the hub tells an outdated installed ASIO driver from the one next
  # to it by their contents.
  add_compile_options(/Brepro)
  add_link_options(/Brepro)
endif()

# Mbox Hub: meters, health, dropout log, analysis, ASIO settings and firmware updates (src/hub).
add_subdirectory(src/hub)

# ASIO driver for DAWs and its test host. Needs the interface headers from Steinberg's ASIO SDK, which this
# repository carries in third_party/asiosdk/common (GPLv3 option of the SDK's license).
set(ASIO_SDK_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/asiosdk CACHE PATH "Steinberg ASIO SDK (the folder holding common/asio.h)")
if(EXISTS ${ASIO_SDK_DIR}/common/asio.h)
  add_subdirectory(src/asio)
else()
  message(STATUS "ASIO SDK not found in ${ASIO_SDK_DIR}: skipping the ASIO driver")
endif()
