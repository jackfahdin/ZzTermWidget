cmake_minimum_required(VERSION 3.16)

# Version mirrors lib/qtermwidget_version.h
project(ZzQTermWidget
    VERSION 2.3.0
    DESCRIPTION "Cross-platform Qt terminal widget (QTermWidget fork)"
    HOMEPAGE_URL "https://github.com/QQxiaoming/ZzQTermWidget"
    LANGUAGES C CXX
)

# ConPTY backend (lib/ptyqt) uses std::jthread / std::stop_token, which require C++20.
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
        Debug Release RelWithDebInfo MinSizeRel)
endif()

include(GNUInstallDirs)

option(ZZQTERMWIDGET_BUILD_EXAMPLE "Build the example terminal application" ON)
option(ZZQTERMWIDGET_INSTALL "Generate install rules for ZzQTermWidget" ON)

# The ptyqt subdirectory exposes its own install switch; keep it in sync.
set(ZZPTYQT_INSTALL ${ZZQTERMWIDGET_INSTALL} CACHE BOOL "Install the ptyqt submodule" FORCE)

find_package(QT NAMES Qt6 REQUIRED COMPONENTS Core)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS
    Core
    Gui
    Widgets
    Network
    Xml
    Multimedia
)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

add_subdirectory(lib)

if(ZZQTERMWIDGET_BUILD_EXAMPLE)
    add_executable(qtermwidget_example examples_main.cpp)
    target_link_libraries(qtermwidget_example PRIVATE ZzQTermWidget::qtermwidget)
endif()
