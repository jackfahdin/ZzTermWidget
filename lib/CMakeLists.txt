add_subdirectory(third_party/ptyqt)

# utf8proc.c already #includes utf8proc_data.c, so only utf8proc.c is compiled here.
set(QTERMWIDGET_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/utf8proc/utf8proc.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/util/CharWidth.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/util/ColorScheme.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/util/Filter.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/util/History.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/util/HistorySearch.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/util/KeyboardTranslator.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/util/SearchBar.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/util/TerminalCharacterDecoder.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/emulation/Emulation.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/emulation/Vt102Emulation.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/emulation/Screen.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/emulation/SixelDecoder.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/emulation/ScreenWindow.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/display/TerminalDisplay.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/widget/qtermwidget.cpp
)

set(QTERMWIDGET_HEADERS
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/utf8proc/utf8proc.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/util/CharWidth.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/util/CharacterColor.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/util/Character.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/util/ColorScheme.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/util/Filter.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/util/History.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/util/HistorySearch.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/util/KeyboardTranslator.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/util/SearchBar.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/util/TerminalCharacterDecoder.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/emulation/Emulation.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/emulation/Vt102Emulation.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/emulation/Screen.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/emulation/SixelDecoder.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/emulation/ScreenWindow.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/display/TerminalDisplay.h
    ${CMAKE_CURRENT_SOURCE_DIR}/include/qtermwidget.h
    ${CMAKE_CURRENT_SOURCE_DIR}/include/qtermwidget_version.h
)

set(QTERMWIDGET_FORMS
    ${CMAKE_CURRENT_SOURCE_DIR}/src/util/SearchBar.ui
)

set(QTERMWIDGET_RESOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/resources/res.qrc
)

add_library(qtermwidget STATIC
    ${QTERMWIDGET_SOURCES}
    ${QTERMWIDGET_HEADERS}
    ${QTERMWIDGET_FORMS}
    ${QTERMWIDGET_RESOURCES}
)
add_library(ZzQTermWidget::qtermwidget ALIAS qtermwidget)

# 翻译：将 .ts 编译为 .qm，并内嵌到 :/lib/qtermwidget/translations
if(TARGET Qt6::lrelease)
    file(GLOB QTERMWIDGET_TS_FILES ${CMAKE_CURRENT_SOURCE_DIR}/resources/translations/*.ts)
    qt_add_lrelease(qtermwidget
        TS_FILES ${QTERMWIDGET_TS_FILES}
        QM_FILES_OUTPUT_VARIABLE QTERMWIDGET_QM_FILES
    )
    # 为 .qm 文件生成一个 .qrc，交给 AUTORCC 直接编译进静态库归档。
    # 若对静态库使用 qt_add_resources()，会生成一个独立 object library，
    # 它既不属于 libqtermwidget.a，也无法通过 install(EXPORT) 安装。
    set(QTERMWIDGET_TRANSLATIONS_QRC ${CMAKE_CURRENT_BINARY_DIR}/qtermwidget_translations.qrc)
    set(QRC_CONTENT "<RCC>\n    <qresource prefix=\"/lib/qtermwidget/translations\">\n")
    foreach(QM_FILE IN LISTS QTERMWIDGET_QM_FILES)
        get_filename_component(QM_FILE_NAME ${QM_FILE} NAME)
        string(APPEND QRC_CONTENT "        <file alias=\"${QM_FILE_NAME}\">${QM_FILE}</file>\n")
    endforeach()
    string(APPEND QRC_CONTENT "    </qresource>\n</RCC>\n")
    file(WRITE ${QTERMWIDGET_TRANSLATIONS_QRC} ${QRC_CONTENT})
    target_sources(qtermwidget PRIVATE ${QTERMWIDGET_TRANSLATIONS_QRC})
    set_source_files_properties(${QTERMWIDGET_TRANSLATIONS_QRC} PROPERTIES
        OBJECT_DEPENDS "${QTERMWIDGET_QM_FILES}")
    # 库代码在该宏下调用 Q_INIT_RESOURCE()；静态库归档中的资源对象文件
    # 若无人显式引用会被链接器剥离。
    target_compile_definitions(qtermwidget PRIVATE QTERMWIDGET_EMBED_TRANSLATIONS)
endif()

set_target_properties(qtermwidget PROPERTIES
    AUTOMOC ON
    AUTOUIC ON
    AUTORCC ON
    VERSION ${PROJECT_VERSION}
)

# SearchBar.h does #include "ui_SearchBar.h"; tell AUTOUIC where the .ui lives.
set_target_properties(qtermwidget PROPERTIES
    AUTOUIC_SEARCH_PATHS ${CMAKE_CURRENT_SOURCE_DIR}/src/util
)

if(MSVC)
    target_compile_options(qtermwidget PRIVATE /W4)
else()
    target_compile_options(qtermwidget PRIVATE -Wall -Wextra)
endif()

target_include_directories(qtermwidget PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/emulation>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/display>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/widget>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/util>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/third_party/utf8proc>
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
