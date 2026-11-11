add_library(gs_touchbox STATIC
    include/gs/touchbox/Describe.h
    include/gs/touchbox/Fleet.h
    include/gs/touchbox/Host.h
    include/gs/touchbox/Plugin.h
    include/gs/touchbox/Texts.h
    src/Describe.cpp
    src/Extension.cpp
    src/Extension.h
    src/Fleet.cpp
    src/Host.cpp
    src/Jobs.cpp
    src/Plugin.cpp
    src/Texts.cpp
)
add_library(gs::touchbox ALIAS gs_touchbox)

target_include_directories(gs_touchbox PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${PROJECT_SOURCE_DIR}/sdk/include"
)
target_link_libraries(gs_touchbox
    PUBLIC gs::esp32 gs::espflash gs::support
    PRIVATE gs::nlohmann_json)
gs_apply_compiler_policy(gs_touchbox)

add_library(gs_ext_esp32 SHARED src/Main.cpp)
set_target_properties(gs_ext_esp32 PROPERTIES
    OUTPUT_NAME esp32
    RUNTIME_OUTPUT_DIRECTORY "${GS_PACKAGE_DIR}$<0:>"
    PDB_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/symbols$<0:>"
)
target_link_libraries(gs_ext_esp32 PRIVATE gs::touchbox)
gs_apply_compiler_policy(gs_ext_esp32)
