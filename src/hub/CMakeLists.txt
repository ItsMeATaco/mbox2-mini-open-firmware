# Mbox Hub: the tray app that shows the Mbox's meters and health, logs dropouts and updates its firmware.
add_executable(mbox2hub WIN32
  analysis.cpp
  app.cpp
  asio_driver.cpp
  device_link.cpp
  eeprom.cpp
  maintenance.cpp
  meters.cpp
  ui.cpp
  update.cpp
)
target_include_directories(mbox2hub PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_compile_definitions(mbox2hub PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS
  MBOX2_VERSION="${PROJECT_VERSION}" MBOX2_REPOSITORY="${MBOX2_REPOSITORY}")
target_link_libraries(mbox2hub PRIVATE winusb cfgmgr32 d2d1 dwrite dwmapi ole32 mmdevapi shell32 windowscodecs
  winhttp bcrypt)
# Static C runtime: the released exe runs without the Visual C++ redistributable.
set_target_properties(mbox2hub PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# Checks the analysis (loudness, true peak, spectrum, phase) against known signals.
add_executable(mbox2hub_analysis_test analysis.cpp analysis_test.cpp)
target_include_directories(mbox2hub_analysis_test PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_compile_definitions(mbox2hub_analysis_test PRIVATE _CRT_SECURE_NO_WARNINGS)
