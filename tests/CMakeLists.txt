function(gs_add_unit_test name)
    cmake_parse_arguments(PARSE_ARGV 1 arg "" "" "SOURCES;LIBRARIES")
    add_executable(${name} unit/main.cpp ${arg_SOURCES})
    target_include_directories(${name} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/unit")
    target_link_libraries(${name} PRIVATE gs::doctest ${arg_LIBRARIES})
    gs_apply_compiler_policy(${name})
    add_test(NAME ${name} COMMAND ${name})
endfunction()

gs_add_unit_test(gs_esp32_tests
    SOURCES
        unit/esp32/BluetoothTests.cpp
        unit/esp32/ContactSlotsTests.cpp
        unit/esp32/DiagnosisTests.cpp
        unit/esp32/FakeBluetooth.h
        unit/esp32/FakeBoard.h
        unit/esp32/FakeRom.h
        unit/esp32/FirmwareTests.cpp
        unit/esp32/FlasherTests.cpp
        unit/esp32/HidScheduleTests.cpp
        unit/esp32/KeysTests.cpp
        unit/esp32/LinkTests.cpp
        unit/esp32/PortTests.cpp
        unit/esp32/ProtocolTests.cpp
        unit/esp32/TestSupport.h
        "${PROJECT_SOURCE_DIR}/firmware/main/gs_hid_sched.c"
    LIBRARIES
        gs::esp32
        gs::espflash
        gs::miniz
        gs::nlohmann_json
        gs::support
)
target_include_directories(gs_esp32_tests PRIVATE "${PROJECT_SOURCE_DIR}/firmware/main")
target_compile_definitions(gs_esp32_tests PRIVATE
    GS_ESP32_VECTORS="${PROJECT_SOURCE_DIR}/protocol/vectors.json"
    GS_ESP32_RELEASE_DIR="${PROJECT_SOURCE_DIR}/firmware/release"
)

gs_add_unit_test(gs_touchbox_tests
    SOURCES
        unit/touchbox/DescribeTests.cpp
        unit/touchbox/FleetTests.cpp
        unit/touchbox/PluginTests.cpp
        unit/touchbox/TextsTests.cpp
    LIBRARIES
        gs::touchbox
        gs::nlohmann_json
)
target_compile_definitions(gs_touchbox_tests PRIVATE
    GS_EXTENSION_MANIFEST="${PROJECT_SOURCE_DIR}/extension/manifest.json"
    GS_PACKAGE_DIR="${GS_PACKAGE_DIR}"
)
