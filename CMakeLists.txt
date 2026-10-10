cmake_minimum_required(VERSION 3.16)
project(plasma-video-wallpaper)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
include(CTest)

find_package(ECM REQUIRED NO_MODULE)
set(CMAKE_MODULE_PATH ${ECM_MODULE_PATH})
include(KDEInstallDirs)
include(KDECMakeSettings)

find_package(Qt6 REQUIRED COMPONENTS Core Gui Qml Quick OpenGL)
find_package(Threads REQUIRED)
find_package(Plasma REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(MPV REQUIRED IMPORTED_TARGET mpv)
pkg_check_modules(EGL REQUIRED IMPORTED_TARGET egl)

add_subdirectory(src)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

file(READ ${CMAKE_CURRENT_SOURCE_DIR}/package/metadata.json J)
string(JSON ID GET ${J} KPlugin Id)
plasma_install_package(package ${ID} wallpapers)
