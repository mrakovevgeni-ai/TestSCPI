cmake_minimum_required(VERSION 3.16)
project(Scpi VERSION 1.1.0 LANGUAGES CXX)

include(CTest)
include(GNUInstallDirs)
include(GenerateExportHeader)
include(CMakePackageConfigHelpers)

find_package(Qt6 6.2 REQUIRED COMPONENTS Core Network)

add_library(scpi SHARED
    include/scpi/scpiclient.h
    src/scpiclient.cpp
    src/responseparser.h
    src/responseparser.cpp
    src/reconnectbackoff.h
    src/scpioperation.h
    src/scpioperation.cpp
    src/scpisession.h
    src/scpisession.cpp
    src/tcptransport.h
    src/tcptransport.cpp
)
add_library(Scpi::scpi ALIAS scpi)
set_target_properties(scpi PROPERTIES
    AUTOMOC ON
    CXX_EXTENSIONS OFF
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
)
target_compile_features(scpi PUBLIC cxx_std_17)
target_link_libraries(scpi PUBLIC Qt6::Core Qt6::Network)
generate_export_header(scpi EXPORT_FILE_NAME generated/scpi/scpi_export.h)
target_include_directories(scpi PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/generated>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

option(SCPI_BUILD_EXAMPLE "Build the QCoreApplication example" ON)
if(SCPI_BUILD_EXAMPLE)
    add_executable(scpi_example examples/main.cpp)
    target_link_libraries(scpi_example PRIVATE Scpi::scpi)
endif()

option(SCPI_BUILD_EMULATOR "Build the TCP instrument emulator" ON)
if(SCPI_BUILD_EMULATOR OR BUILD_TESTING)
    add_library(scpi_emulator_support STATIC
        emulator/instrumentemulator.cpp
        emulator/instrumentemulator.h
    )
    target_compile_features(scpi_emulator_support PUBLIC cxx_std_17)
    target_include_directories(scpi_emulator_support PUBLIC emulator)
    target_link_libraries(scpi_emulator_support PUBLIC Qt6::Core Qt6::Network)
endif()

if(SCPI_BUILD_EMULATOR)
    add_executable(scpi_emulator emulator/main.cpp)
    target_link_libraries(scpi_emulator PRIVATE scpi_emulator_support)
    install(TARGETS scpi_emulator RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
endif()

if(BUILD_TESTING)
    find_package(Qt6 6.2 REQUIRED COMPONENTS Test)

    add_executable(scpi_parser_tests tests/parser_test.cpp src/responseparser.cpp)
    target_include_directories(scpi_parser_tests PRIVATE src)

    add_executable(scpi_client_tests tests/client_test.cpp)
    add_executable(scpi_recovery_tests tests/recovery_test.cpp)
    add_executable(scpi_emulator_tests tests/emulator_test.cpp)
    target_link_libraries(scpi_emulator_tests PRIVATE scpi_emulator_support)

    foreach(test_target IN ITEMS scpi_parser_tests scpi_client_tests scpi_recovery_tests scpi_emulator_tests)
        target_link_libraries(${test_target} PRIVATE Scpi::scpi Qt6::Test)
        set_target_properties(${test_target} PROPERTIES AUTOMOC ON)
        add_test(NAME ${test_target} COMMAND ${test_target})
        set_tests_properties(${test_target} PROPERTIES TIMEOUT 30)
    endforeach()
endif()

set(scpi_package_dir "${CMAKE_INSTALL_LIBDIR}/cmake/Scpi")
configure_package_config_file(cmake/ScpiConfig.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/ScpiConfig.cmake"
    INSTALL_DESTINATION "${scpi_package_dir}"
)
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/ScpiConfigVersion.cmake"
    COMPATIBILITY SameMajorVersion
)

install(TARGETS scpi EXPORT ScpiTargets
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
)
install(FILES
    include/scpi/scpiclient.h
    "${CMAKE_CURRENT_BINARY_DIR}/generated/scpi/scpi_export.h"
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/scpi"
)
install(EXPORT ScpiTargets NAMESPACE Scpi:: DESTINATION "${scpi_package_dir}")
install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/ScpiConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/ScpiConfigVersion.cmake"
    DESTINATION "${scpi_package_dir}"
)
