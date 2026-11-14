# mbox2-firstinstall: the experimental first install of the open firmware on a stock Mbox 2 Mini from Windows,
# through Avid's own USB update (DFU) mode. Runs as administrator (WinUSB setup and the hub's port cycle).
add_executable(mbox2-firstinstall main.cpp dfu.cpp usb_win.cpp)
target_include_directories(mbox2-firstinstall PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_compile_definitions(mbox2-firstinstall PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_link_libraries(mbox2-firstinstall PRIVATE winusb setupapi cfgmgr32 advapi32)
set_target_properties(mbox2-firstinstall PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
if(MSVC)
  target_link_options(mbox2-firstinstall PRIVATE "/MANIFESTUAC:level='requireAdministrator' uiAccess='false'")
endif()

# The DFU download against a simulated Avid update mode, and the descriptor parser against the Mini's real
# descriptors.
add_executable(mbox2-firstinstall_dfu_test dfu.cpp dfu_test.cpp)
target_include_directories(mbox2-firstinstall_dfu_test PRIVATE ${PROJECT_SOURCE_DIR}/src)
set_target_properties(mbox2-firstinstall_dfu_test PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
