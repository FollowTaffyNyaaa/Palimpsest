cmake_minimum_required(VERSION 3.24)
project(PalimpsestCore VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(palimpsest_core SHARED
    src/paper_ir.cpp
    src/pdfium_runtime.cpp
    src/formula_layout.cpp
    src/abstract_layout.cpp
    src/inspector.cpp
)

target_include_directories(palimpsest_core
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_definitions(palimpsest_core PRIVATE
    PALIMPSEST_CORE_EXPORTS
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    UNICODE
    _UNICODE
)

if(WIN32)
    target_link_libraries(palimpsest_core PRIVATE windowscodecs ole32)
endif()

add_executable(paper_inspector tools/paper_inspector/main.cpp)
target_link_libraries(paper_inspector PRIVATE palimpsest_core)

include(CTest)
if(BUILD_TESTING)
    add_executable(formula_layout_tests
        tests/formula_layout_tests.cpp
        src/formula_layout.cpp
    )
    target_include_directories(formula_layout_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    set_target_properties(formula_layout_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
    )
    add_test(NAME formula_layout_tests COMMAND formula_layout_tests)

    add_executable(abstract_layout_tests
        tests/abstract_layout_tests.cpp
        src/abstract_layout.cpp
    )
    target_include_directories(abstract_layout_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    set_target_properties(abstract_layout_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
    )
    add_test(NAME abstract_layout_tests COMMAND abstract_layout_tests)
endif()

set_target_properties(palimpsest_core paper_inspector PROPERTIES
    PREFIX ""
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
    ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib
)

if(MINGW)
    target_compile_options(palimpsest_core PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(paper_inspector PRIVATE -Wall -Wextra -Wpedantic)
endif()
