find_package(QT NAMES Qt6 Qt7 REQUIRED COMPONENTS Core Network)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Core Network)

set(PTYQT_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/ptyqt.cpp
)
set(PTYQT_HEADERS
    ${CMAKE_CURRENT_SOURCE_DIR}/iptyprocess.h
    ${CMAKE_CURRENT_SOURCE_DIR}/ptyqt.h
)

if(WIN32)
    # Native ConPty backend (Windows 10 1903+), used for both MSVC and MinGW.
    list(APPEND PTYQT_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/conptyprocess.cpp)
    list(APPEND PTYQT_HEADERS ${CMAKE_CURRENT_SOURCE_DIR}/conptyprocess.h)
else()
    # Standard POSIX pty backend (Linux / macOS / other UNIX)
    list(APPEND PTYQT_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/unixptyprocess.cpp)
    list(APPEND PTYQT_HEADERS ${CMAKE_CURRENT_SOURCE_DIR}/unixptyprocess.h)
endif()

add_library(ptyqt STATIC ${PTYQT_SOURCES} ${PTYQT_HEADERS})
add_library(ZzPtyQt::ptyqt ALIAS ptyqt)

set_target_properties(ptyqt PROPERTIES AUTOMOC ON)

if(MSVC)
    target_compile_options(ptyqt PRIVATE /W4)
else()
    target_compile_options(ptyqt PRIVATE -Wall -Wextra)
endif()

target_include_directories(ptyqt PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/ZzPtyQt>
)

target_link_libraries(ptyqt PUBLIC
    Qt${QT_VERSION_MAJOR}::Core
    Qt${QT_VERSION_MAJOR}::Network
)

if(WIN32)
    # ConPty needs Advapi32 (CreateProcessAsUserW); the rest match the original link set.
    target_link_libraries(ptyqt PUBLIC
        Advapi32
        wsock32 ws2_32 crypt32 iphlpapi netapi32 version winmm userenv
    )
else()
    target_link_libraries(ptyqt PUBLIC pthread dl)
endif()

# ----------------------------------------------------------------------------
# Install rules + CMake package config so consumers can use find_package(ZzPtyQt)
# ----------------------------------------------------------------------------
if(ZZPTYQT_INSTALL)
    include(GNUInstallDirs)
    include(CMakePackageConfigHelpers)

    install(TARGETS ptyqt
        EXPORT ZzPtyQtTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/ZzPtyQt
    )

    install(FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/iptyprocess.h
        ${CMAKE_CURRENT_SOURCE_DIR}/ptyqt.h
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/ZzPtyQt
    )

    install(EXPORT ZzPtyQtTargets
        FILE ZzPtyQtTargets.cmake
        NAMESPACE ZzPtyQt::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ZzPtyQt
    )

    configure_package_config_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/ZzPtyQtConfig.cmake.in
        ${CMAKE_CURRENT_BINARY_DIR}/ZzPtyQtConfig.cmake
        INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ZzPtyQt
    )

    write_basic_package_version_file(
        ${CMAKE_CURRENT_BINARY_DIR}/ZzPtyQtConfigVersion.cmake
        VERSION ${PROJECT_VERSION}
        COMPATIBILITY SameMajorVersion
    )

    install(FILES
        ${CMAKE_CURRENT_BINARY_DIR}/ZzPtyQtConfig.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/ZzPtyQtConfigVersion.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ZzPtyQt
    )
endif()
