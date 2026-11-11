add_library(gs_third_party_doctest INTERFACE)
add_library(gs::doctest ALIAS gs_third_party_doctest)
target_include_directories(gs_third_party_doctest SYSTEM INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}/doctest"
)
target_compile_definitions(gs_third_party_doctest INTERFACE DOCTEST_CONFIG_USE_STD_HEADERS)

add_library(gs_third_party_tl_expected INTERFACE)
add_library(gs::tl_expected ALIAS gs_third_party_tl_expected)
target_include_directories(gs_third_party_tl_expected SYSTEM INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}/tl-expected/include"
)

add_library(gs_third_party_nlohmann_json INTERFACE)
add_library(gs::nlohmann_json ALIAS gs_third_party_nlohmann_json)
target_include_directories(gs_third_party_nlohmann_json SYSTEM INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}/nlohmann-json/include"
)

add_library(gs_third_party_miniz STATIC
    "${CMAKE_CURRENT_SOURCE_DIR}/miniz/miniz.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/miniz/miniz.h"
)
add_library(gs::miniz ALIAS gs_third_party_miniz)
target_include_directories(gs_third_party_miniz SYSTEM PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/miniz")
target_compile_definitions(gs_third_party_miniz
    PUBLIC MINIZ_NO_ZLIB_COMPATIBLE_NAMES
    PRIVATE _CRT_SECURE_NO_WARNINGS
)
