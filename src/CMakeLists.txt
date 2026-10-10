set(CMAKE_AUTOMOC ON)

add_library(plasmavideowallpaperplugin MODULE
    mpvitem.cpp playbacksession.cpp plugin.cpp)

set_target_properties(plasmavideowallpaperplugin PROPERTIES
    PREFIX "lib")

target_link_libraries(plasmavideowallpaperplugin
    Qt6::Core Qt6::Gui Qt6::Qml Qt6::Quick Qt6::OpenGL
    PkgConfig::MPV PkgConfig::EGL Threads::Threads)

install(TARGETS plasmavideowallpaperplugin
    DESTINATION ${KDE_INSTALL_QMLDIR}/org/kde/videowallpaper)
install(FILES qmldir
    DESTINATION ${KDE_INSTALL_QMLDIR}/org/kde/videowallpaper)
