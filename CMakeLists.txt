cmake_minimum_required(VERSION 3.16)
project(plasma-video-wallpaper)

find_package(ECM REQUIRED NO_MODULE)
set(CMAKE_MODULE_PATH ${ECM_MODULE_PATH})
include(KDEInstallDirs)
include(KDECMakeSettings)

find_package(Qt6 REQUIRED COMPONENTS Core Gui Qml Quick)
find_package(Plasma REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(MPV REQUIRED IMPORTED_TARGET mpv)

add_subdirectory(src)

file(READ ${CMAKE_CURRENT_SOURCE_DIR}/package/metadata.json J)
string(JSON ID GET ${J} KPlugin Id)
plasma_install_package(package ${ID} wallpapers)
