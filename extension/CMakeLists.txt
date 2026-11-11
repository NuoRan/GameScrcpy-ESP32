foreach(gs_file IN ITEMS manifest.json scripts/panel.ts icons/board.svg)
    configure_file("${CMAKE_CURRENT_SOURCE_DIR}/${gs_file}" "${GS_PACKAGE_DIR}/${gs_file}" COPYONLY)
endforeach()

foreach(gs_file IN ITEMS esp32.md esp32.zh_CN.md)
    configure_file("${PROJECT_SOURCE_DIR}/docs/help/${gs_file}" "${GS_PACKAGE_DIR}/help/${gs_file}" COPYONLY)
endforeach()

file(GLOB gs_firmware_files LIST_DIRECTORIES false "${PROJECT_SOURCE_DIR}/firmware/release/*")
foreach(gs_file IN LISTS gs_firmware_files)
    get_filename_component(gs_name "${gs_file}" NAME)
    configure_file("${gs_file}" "${GS_PACKAGE_DIR}/firmware/${gs_name}" COPYONLY)
endforeach()

configure_file("${PROJECT_SOURCE_DIR}/LICENSE" "${GS_PACKAGE_DIR}/LICENSE.txt" COPYONLY)
configure_file("${PROJECT_SOURCE_DIR}/third_party/nlohmann-json/LICENSE.MIT" "${GS_PACKAGE_DIR}/licenses/nlohmann-json.txt"
               COPYONLY)
configure_file("${PROJECT_SOURCE_DIR}/third_party/miniz/LICENSE" "${GS_PACKAGE_DIR}/licenses/miniz.txt" COPYONLY)
configure_file("${PROJECT_SOURCE_DIR}/third_party/tl-expected/COPYING" "${GS_PACKAGE_DIR}/licenses/tl-expected.txt"
               COPYONLY)
