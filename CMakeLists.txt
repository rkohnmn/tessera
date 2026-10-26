# ═══════════════════════════════════════════════════════════════════════════════
#  Tessera — instant, offline exploration of very large delimited data files
# ═══════════════════════════════════════════════════════════════════════════════
cmake_minimum_required(VERSION 3.20)

project(tessera
    VERSION 1.0.0
    DESCRIPTION "Instant, offline explorer for very large CSV/TSV/JSONL files"
    HOMEPAGE_URL "https://github.com/rkohnmn/tessera"
    LANGUAGES CXX
)

# ── Global settings ────────────────────────────────────────────────────────────
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release RelWithDebInfo MinSizeRel)
endif()

# Fully static CRT on MSVC so tessera.exe is a single portable file.
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# ── Options ───────────────────────────────────────────────────────────────────
option(TESSERA_BUILD_TESTS  "Build the unit test suite"            ON)
option(TESSERA_LTO          "Enable interprocedural optimisation"   ON)
option(TESSERA_STRICT       "Turn compiler warnings into errors"    OFF)

find_package(Threads REQUIRED)

# ── Warning profile ───────────────────────────────────────────────────────────
add_library(tessera_warnings INTERFACE)
if(MSVC)
    target_compile_options(tessera_warnings INTERFACE /W4 /permissive- /utf-8 /Zc:__cplusplus $<$<BOOL:${TESSERA_STRICT}>:/WX>)
else()
    target_compile_options(tessera_warnings INTERFACE
        -Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor
        -Wcast-align -Wunused -Woverloaded-virtual -Wnull-dereference
        -Wdouble-promotion -Wformat=2
        $<$<BOOL:${TESSERA_STRICT}>:-Werror>
    )
endif()

if(TESSERA_LTO)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT TESSERA_IPO_OK OUTPUT TESSERA_IPO_MSG LANGUAGES CXX)
    if(TESSERA_IPO_OK)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
    endif()
endif()

# ── Core library (portable, engine + TUI + CLI plumbing) ──────────────────────
add_library(tessera_core STATIC
    src/common/util.cpp
    src/core/source.cpp
    src/core/dialect.cpp
    src/core/dataset.cpp
    src/core/indexer.cpp
    src/core/sidecar.cpp
    src/core/profile.cpp
    src/query/parser.cpp
    src/query/exec.cpp
    src/io/format.cpp
    src/io/writer.cpp
    src/cli/commands.cpp
    src/tui/terminal.cpp
    src/tui/screen.cpp
    src/tui/app.cpp
    src/tui/views.cpp
)

target_include_directories(tessera_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(tessera_core PUBLIC Threads::Threads PRIVATE tessera_warnings)
target_compile_definitions(tessera_core PUBLIC
    TESSERA_VERSION="${PROJECT_VERSION}"
    TESSERA_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
)

if(WIN32)
    target_compile_definitions(tessera_core PUBLIC TESSERA_WINDOWS=1 _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
else()
    target_compile_definitions(tessera_core PUBLIC TESSERA_POSIX=1)
endif()

# ── Executable ────────────────────────────────────────────────────────────────
add_executable(tessera src/main.cpp)
target_link_libraries(tessera PRIVATE tessera_core)
if(WIN32)
    target_link_libraries(tessera PRIVATE shell32)
endif()

if(MINGW)
    # Statically link the C++ runtime; the resulting .exe has no DLL dependencies.
    target_link_options(tessera PRIVATE -static -static-libgcc -static-libstdc++)
endif()

install(TARGETS tessera RUNTIME DESTINATION .)

# ── Tests ─────────────────────────────────────────────────────────────────────
if(TESSERA_BUILD_TESTS)
    enable_testing()
    add_executable(tessera_tests
        tests/test_main.cpp
        tests/test_text.cpp
        tests/test_scan.cpp
        tests/test_query.cpp
        tests/test_edge_cases.cpp
        tests/test_sidecar.cpp
        tests/test_cli.cpp
        tests/test_profile_export.cpp
    )
    target_link_libraries(tessera_tests PRIVATE tessera_core tessera_warnings)
    add_test(NAME unit COMMAND tessera_tests)
    set_tests_properties(unit PROPERTIES TIMEOUT 300)
endif()

# ── Optional convenience target: `cmake --build build --target check` ─────────
add_custom_target(check
    COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure
    DEPENDS tessera_tests
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    COMMENT "Running Tessera test suite"
)
