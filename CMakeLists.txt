cmake_minimum_required(VERSION 3.16)
project(TLHoN_SE VERSION 0.1 LANGUAGES CXX)

set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 REQUIRED COMPONENTS Core Widgets)

add_library(savecore STATIC
    savepaths.h savepaths.cpp
    savedocument.h savedocument.cpp
    savestore.h savestore.cpp
    jsonparser.h jsonparser.cpp
)
target_link_libraries(savecore PUBLIC Qt6::Core)
target_include_directories(savecore PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

set(EDITOR_SOURCES
    mainwindow.cpp mainwindow.h mainwindow.ui
    stylefromfile.cpp stylefromfile.h
    tab_stats.cpp tab_stats.h tab_stats.ui
    tab_equipment.cpp tab_equipment.h tab_equipment.ui
    tab_settings.cpp tab_settings.h tab_settings.ui
    tab_about.cpp tab_about.h tab_about.ui
    positiveintvalidator.cpp positiveintvalidator.h
    resource.qrc
)
# The upstream file.rc is not tracked. Allow local resource customization
# without requiring a missing file in a clean checkout.
if(WIN32 AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/file.rc")
    list(APPEND EDITOR_SOURCES file.rc)
endif()

qt_add_executable(TLHoN_SE main.cpp ${EDITOR_SOURCES})
target_link_libraries(TLHoN_SE PRIVATE Qt6::Widgets savecore)
set_target_properties(TLHoN_SE PROPERTIES WIN32_EXECUTABLE TRUE MACOSX_BUNDLE TRUE
    MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
    MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR})
include(GNUInstallDirs)
install(TARGETS TLHoN_SE
    BUNDLE DESTINATION .
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

include(CTest)
if(BUILD_TESTING)
    find_package(Qt6 REQUIRED COMPONENTS Test)
    add_executable(savecore_tests tests/savecore_tests.cpp)
    target_link_libraries(savecore_tests PRIVATE savecore Qt6::Test)
    target_compile_definitions(savecore_tests PRIVATE FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/Resources/Tests")
    add_test(NAME savecore_tests COMMAND savecore_tests)

    add_executable(editor_tests tests/editor_tests.cpp ${EDITOR_SOURCES})
    target_link_libraries(editor_tests PRIVATE savecore Qt6::Widgets Qt6::Test)
    target_compile_definitions(editor_tests PRIVATE FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/Resources/Tests")
    add_test(NAME editor_tests COMMAND editor_tests)
    set_tests_properties(editor_tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endif()
