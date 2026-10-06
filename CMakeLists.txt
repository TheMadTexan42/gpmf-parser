# CMakeLists.txt
cmake_minimum_required (VERSION 3.8.2)
project (gpmf-parser)

set(CMAKE_SUPPRESS_REGENERATION true)
set(CMAKE_CONFIGURATION_TYPES "Debug;Release")

file(GLOB LIB_SOURCES "GPMF_parser.c" "GPMF_utils.c")
set(DEMO_READERS "demo/GPMF_mp4reader.c" "demo/GPMF_rawreader.c" "demo/GPMF_source.c")
set(SOURCES ${LIB_SOURCES} "demo/GPMF_demo.c" "demo/GPMF_print.c" ${DEMO_READERS})

add_executable(GPMF_PARSER_BIN ${SOURCES})
set_target_properties(GPMF_PARSER_BIN PROPERTIES OUTPUT_NAME "${PROJECT_NAME}")
add_library(GPMF_PARSER_LIB ${LIB_SOURCES})
set_target_properties(GPMF_PARSER_LIB PROPERTIES OUTPUT_NAME "${PROJECT_NAME}")

add_executable(GPMF_GPX_BIN "demo/GPMF_to_gpx.c" ${DEMO_READERS})
set_target_properties(GPMF_GPX_BIN PROPERTIES OUTPUT_NAME "gpmf2gpx")
target_link_libraries(GPMF_GPX_BIN PRIVATE GPMF_PARSER_LIB)
if(UNIX)
    target_link_libraries(GPMF_PARSER_BIN PRIVATE m)
    target_link_libraries(GPMF_PARSER_LIB PUBLIC m)
endif()

set(PC_LINK_FLAGS "-l${PROJECT_NAME}")
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/${PROJECT_NAME}.pc.in" "${CMAKE_BINARY_DIR}/${PROJECT_NAME}.pc" @ONLY)

install(TARGETS GPMF_PARSER_BIN DESTINATION "bin")
install(TARGETS GPMF_GPX_BIN DESTINATION "bin")
install(TARGETS GPMF_PARSER_LIB DESTINATION "lib")
install(FILES "${CMAKE_BINARY_DIR}/${PROJECT_NAME}.pc" DESTINATION "lib/pkgconfig")

install(FILES "GPMF_parser.h" "GPMF_common.h" "GPMF_utils.h" DESTINATION "include/gpmf-parser")
install(FILES "demo/GPMF_mp4reader.h" DESTINATION "include/gpmf-parser/demo")

include(CTest)
if(BUILD_TESTING)
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND)
        add_test(NAME gpmf_file_inputs COMMAND ${Python3_EXECUTABLE}
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_file_inputs.py"
            --demo $<TARGET_FILE:GPMF_PARSER_BIN> --gpx $<TARGET_FILE:GPMF_GPX_BIN>
            --samples "${CMAKE_CURRENT_SOURCE_DIR}/samples")
    endif()
endif()
