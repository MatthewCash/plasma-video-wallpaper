set(CMAKE_AUTOMOC ON)

add_library(plasmavideowallpaperplugin MODULE
    mpvitem.cpp plugin.cpp)

set_target_properties(plasmavideowallpaperplugin PROPERTIES
    PREFIX "lib")

target_link_libraries(plasmavideowallpaperplugin
    Qt6::Core Qt6::Gui Qt6::Qml Qt6::Quick
    PkgConfig::MPV)

install(TARGETS plasmavideowallpaperplugin
    DESTINATION ${KDE_INSTALL_QMLDIR}/org/kde/videowallpaper)
install(FILES qmldir
    DESTINATION ${KDE_INSTALL_QMLDIR}/org/kde/videowallpaper)
