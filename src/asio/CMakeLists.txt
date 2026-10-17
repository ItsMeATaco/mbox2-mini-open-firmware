# ASIO driver for DAWs: a user-mode COM DLL on top of WASAPI exclusive mode, and its test host.
# Static C runtime, so the installed DLL is a single file.
add_library(mbox2asio SHARED
  dll.cpp
  driver.cpp
  engine.cpp
  log.cpp
  mbox2asio.def
)
target_include_directories(mbox2asio PRIVATE ${PROJECT_SOURCE_DIR}/src ${ASIO_SDK_DIR}/common)
target_compile_definitions(mbox2asio PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_link_libraries(mbox2asio PRIVATE ole32 avrt winmm shell32 advapi32 user32)
set_target_properties(mbox2asio PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# Loads the driver straight from the DLL (or through COM with --registered), plays, records and times it.
add_executable(asiotest asiotest.cpp)
target_include_directories(asiotest PRIVATE ${PROJECT_SOURCE_DIR}/src ${ASIO_SDK_DIR}/common)
target_compile_definitions(asiotest PRIVATE UNICODE _UNICODE NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_link_libraries(asiotest PRIVATE ole32)
add_dependencies(asiotest mbox2asio)
