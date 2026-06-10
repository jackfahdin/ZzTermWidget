add_subdirectory(ptyqt)

# utf8proc.c already #includes utf8proc_data.c, so only utf8proc.c is compiled here.
set(QTERMWIDGET_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/utf8proc/utf8proc.c
    ${CMAKE_CURRENT_SOURCE_DIR}/util/CharWidth.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/util/ColorScheme.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/util/Filter.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/util/History.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/util/HistorySearch.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/util/KeyboardTranslator.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/util/SearchBar.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/util/TerminalCharacterDecoder.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Emulation.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Vt102Emulation.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Screen.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/ScreenWindow.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/TerminalDisplay.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/qtermwidget.cpp
)

set(QTERMWIDGET_HEADERS
    ${CMAKE_CURRENT_SOURCE_DIR}/utf8proc/utf8proc.h
    ${CMAKE_CURRENT_SOURCE_DIR}/util/CharWidth.h
    ${CMAKE_CURRENT_SOURCE_DIR}/util/CharacterColor.h
    ${CMAKE_CURRENT_SOURCE_DIR}/util/Character.h
    ${CMAKE_CURRENT_SOURCE_DIR}/util/ColorScheme.h
    ${CMAKE_CURRENT_SOURCE_DIR}/util/Filter.h
    ${CMAKE_CURRENT_SOURCE_DIR}/util/History.h
    ${CMAKE_CURRENT_SOURCE_DIR}/util/HistorySearch.h
    ${CMAKE_CURRENT_SOURCE_DIR}/util/KeyboardTranslator.h
    ${CMAKE_CURRENT_SOURCE_DIR}/util/SearchBar.h
    ${CMAKE_CURRENT_SOURCE_DIR}/util/TerminalCharacterDecoder.h
    ${CMAKE_CURRENT_SOURCE_DIR}/Emulation.h
    ${CMAKE_CURRENT_SOURCE_DIR}/Vt102Emulation.h
    ${CMAKE_CURRENT_SOURCE_DIR}/Screen.h
    ${CMAKE_CURRENT_SOURCE_DIR}/ScreenWindow.h
    ${CMAKE_CURRENT_SOURCE_DIR}/TerminalDisplay.h
    ${CMAKE_CURRENT_SOURCE_DIR}/qtermwidget.h
    ${CMAKE_CURRENT_SOURCE_DIR}/qtermwidget_version.h
)

set(QTERMWIDGET_FORMS
    ${CMAKE_CURRENT_SOURCE_DIR}/util/SearchBar.ui
)

set(QTERMWIDGET_RESOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/res.qrc
)

add_library(qtermwidget STATIC
    ${QTERMWIDGET_SOURCES}
    ${QTERMWIDGET_HEADERS}
    ${QTERMWIDGET_FORMS}
    ${QTERMWIDGET_RESOURCES}
)
add_library(ZzQTermWidget::qtermwidget ALIAS qtermwidget)

set_target_properties(qtermwidget PROPERTIES
    AUTOMOC ON
    AUTOUIC ON
    AUTORCC ON
    VERSION ${PROJECT_VERSION}
)

# SearchBar.h does #include "ui_SearchBar.h"; tell AUTOUIC where the .ui lives.
set_target_properties(qtermwidget PROPERTIES
    AUTOUIC_SEARCH_PATHS ${CMAKE_CURRENT_SOURCE_DIR}/util
)

if(MSVC)
    target_compile_options(qtermwidget PRIVATE /W4)
else()
    target_compile_options(qtermwidget PRIVATE -Wall -Wextra)
endif()

target_include_directories(qtermwidget PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/util>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/utf8proc>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/ZzQTermWidget>
)

target_link_libraries(qtermwidget PUBLIC
    Qt${QT_VERSION_MAJOR}::Core
    Qt${QT_VERSION_MAJOR}::Gui
    Qt${QT_VERSION_MAJOR}::Widgets
    Qt${QT_VERSION_MAJOR}::Network
    Qt${QT_VERSION_MAJOR}::Xml
    Qt${QT_VERSION_MAJOR}::Multimedia
    ZzPtyQt::ptyqt
)

# ----------------------------------------------------------------------------
# Install rules + CMake package config so consumers can use find_package(ZzQTermWidget)
# ----------------------------------------------------------------------------
if(ZZQTERMWIDGET_INSTALL)
    include(GNUInstallDirs)
    include(CMakePackageConfigHelpers)

    install(TARGETS qtermwidget
        EXPORT ZzQTermWidgetTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/ZzQTermWidget
    )

    install(FILES ${QTERMWIDGET_HEADERS}
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/ZzQTermWidget
    )

    install(EXPORT ZzQTermWidgetTargets
        FILE ZzQTermWidgetTargets.cmake
        NAMESPACE ZzQTermWidget::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ZzQTermWidget
    )

    configure_package_config_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/ZzQTermWidgetConfig.cmake.in
        ${CMAKE_CURRENT_BINARY_DIR}/ZzQTermWidgetConfig.cmake
        INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ZzQTermWidget
    )

    write_basic_package_version_file(
        ${CMAKE_CURRENT_BINARY_DIR}/ZzQTermWidgetConfigVersion.cmake
        VERSION ${PROJECT_VERSION}
        COMPATIBILITY SameMajorVersion
    )

    install(FILES
        ${CMAKE_CURRENT_BINARY_DIR}/ZzQTermWidgetConfig.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/ZzQTermWidgetConfigVersion.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ZzQTermWidget
    )
endif()
