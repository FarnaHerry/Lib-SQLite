cmake_minimum_required(VERSION 3.20)

# CMake initializes Apple deployment defaults during the first project() call.
# Explicit settings and toolchains retain ownership of their target platform.
if (CMAKE_HOST_APPLE AND (NOT CMAKE_SYSTEM_NAME OR CMAKE_SYSTEM_NAME STREQUAL "Darwin")
        AND NOT CMAKE_TOOLCHAIN_FILE AND "$ENV{CMAKE_TOOLCHAIN_FILE}" STREQUAL ""
        AND NOT DEFINED CMAKE_OSX_DEPLOYMENT_TARGET AND "$ENV{MACOSX_DEPLOYMENT_TARGET}" STREQUAL "")
    set(CMAKE_OSX_DEPLOYMENT_TARGET "12.0" CACHE STRING "Minimum macOS deployment target")
endif ()

project(huxerui_sqlite VERSION 0.1.0 LANGUAGES NONE)

include("${CMAKE_CURRENT_SOURCE_DIR}/HuxerUIProject.cmake")
if (HUXERUI_PROJECT_PLAN_ONLY)
    return()
endif ()

file(GLOB_RECURSE LIBRARY_SOURCE_FILES CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cc"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cxx"
)
list(APPEND LIBRARY_SOURCE_FILES
        "${CMAKE_CURRENT_SOURCE_DIR}/src/sqlite_backend.c"
)
if (ANDROID)
    file(GLOB_RECURSE LIBRARY_ANDROID_SOURCE_FILES CONFIGURE_DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/platform/android/src/main/cpp/*.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/platform/android/src/main/cpp/*.cc"
            "${CMAKE_CURRENT_SOURCE_DIR}/platform/android/src/main/cpp/*.cxx"
    )
    list(APPEND LIBRARY_SOURCE_FILES ${LIBRARY_ANDROID_SOURCE_FILES})
endif ()
if (WIN32)
    file(GLOB_RECURSE LIBRARY_WINDOWS_SOURCE_FILES CONFIGURE_DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/platform/windows/src/*.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/platform/windows/src/*.cc"
            "${CMAKE_CURRENT_SOURCE_DIR}/platform/windows/src/*.cxx"
    )
    list(APPEND LIBRARY_SOURCE_FILES ${LIBRARY_WINDOWS_SOURCE_FILES})
endif ()
if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
    file(GLOB_RECURSE LIBRARY_LINUX_SOURCE_FILES CONFIGURE_DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/platform/linux/src/*.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/platform/linux/src/*.cc"
            "${CMAKE_CURRENT_SOURCE_DIR}/platform/linux/src/*.cxx"
    )
    list(APPEND LIBRARY_SOURCE_FILES ${LIBRARY_LINUX_SOURCE_FILES})
endif ()
if (APPLE AND NOT IOS)
    file(GLOB_RECURSE LIBRARY_MACOS_SOURCE_FILES CONFIGURE_DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/platform/macos/src/*.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/platform/macos/src/*.cc"
            "${CMAKE_CURRENT_SOURCE_DIR}/platform/macos/src/*.cxx"
            "${CMAKE_CURRENT_SOURCE_DIR}/platform/macos/src/*.mm"
    )
    list(APPEND LIBRARY_SOURCE_FILES ${LIBRARY_MACOS_SOURCE_FILES})
endif ()
if (EMSCRIPTEN)
    file(GLOB_RECURSE LIBRARY_WEB_SOURCE_FILES CONFIGURE_DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/platform/web/src/*.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/platform/web/src/*.cc"
            "${CMAKE_CURRENT_SOURCE_DIR}/platform/web/src/*.cxx"
    )
    list(APPEND LIBRARY_SOURCE_FILES ${LIBRARY_WEB_SOURCE_FILES})
endif ()

huxerui_add_library(huxerui_sqlite
        SOURCES
            ${LIBRARY_SOURCE_FILES}
        RESOURCES
            resources
        RESOURCE_NAMESPACE
            sqlite
)

add_library(HuxerUI::SQLite ALIAS huxerui_sqlite)

if (HUXERUI_LIBRARY_GRAPH_ONLY)
    return()
endif ()

target_include_directories(huxerui_sqlite PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
        "$<INSTALL_INTERFACE:include>"
)
target_include_directories(huxerui_sqlite PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/third_party/sqlite"
)
target_compile_definitions(huxerui_sqlite PRIVATE
        SQLITE_DEFAULT_FOREIGN_KEYS=1
        SQLITE_DQS=0
        SQLITE_OMIT_DEPRECATED
        SQLITE_OMIT_LOAD_EXTENSION
        SQLITE_THREADSAFE=1
)
if (IOS)
    target_compile_definitions(huxerui_sqlite PRIVATE HAVE_GETHOSTUUID=0)
endif ()
if (CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/src/sqlite_backend.c"
            PROPERTIES COMPILE_OPTIONS "-Wno-unused-function"
    )
endif ()

if (CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
    set(HUXERUI_SQLITE_BUILD_TESTS_DEFAULT ON)
else ()
    set(HUXERUI_SQLITE_BUILD_TESTS_DEFAULT OFF)
endif ()
option(HUXERUI_SQLITE_BUILD_TESTS
        "Build HuxerUI SQLite tests"
        ${HUXERUI_SQLITE_BUILD_TESTS_DEFAULT}
)

if (HUXERUI_SQLITE_BUILD_TESTS AND NOT HUXERUI_LIBRARY_GRAPH_ONLY)
    enable_testing()
    add_executable(huxerui_sqlite_value_tests
            tests/value_tests.cpp
    )
    target_compile_features(huxerui_sqlite_value_tests PRIVATE cxx_std_20)
    target_link_libraries(huxerui_sqlite_value_tests PRIVATE
            HuxerUI::SQLite
            HuxerUI::huxerui_static
    )
    add_test(NAME huxerui_sqlite_value_tests COMMAND huxerui_sqlite_value_tests)

    add_executable(huxerui_sqlite_backend_tests
            tests/backend_tests.cpp
    )
    target_compile_features(huxerui_sqlite_backend_tests PRIVATE cxx_std_20)
    target_include_directories(huxerui_sqlite_backend_tests PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    target_link_libraries(huxerui_sqlite_backend_tests PRIVATE HuxerUI::SQLite)
    add_test(NAME huxerui_sqlite_backend_tests COMMAND huxerui_sqlite_backend_tests)

    if (MSVC)
        target_compile_options(huxerui_sqlite_value_tests PRIVATE /UNDEBUG)
        target_compile_options(huxerui_sqlite_backend_tests PRIVATE /UNDEBUG)
    else ()
        target_compile_options(huxerui_sqlite_value_tests PRIVATE -UNDEBUG)
        target_compile_options(huxerui_sqlite_backend_tests PRIVATE -UNDEBUG)
    endif ()

    add_executable(huxerui_sqlite_integration_tests
            tests/integration_tests.cpp
    )
    target_compile_features(huxerui_sqlite_integration_tests PRIVATE cxx_std_20)
    target_link_libraries(huxerui_sqlite_integration_tests PRIVATE
            HuxerUI::SQLite
            HuxerUI::huxerui_static
    )
    add_test(NAME huxerui_sqlite_integration_tests COMMAND huxerui_sqlite_integration_tests)
endif ()
