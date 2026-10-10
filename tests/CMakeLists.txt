set(CMAKE_AUTOMOC ON)
add_executable(wallpaper-lifecycle
    lifecycle.cpp
    ../src/mpvitem.cpp
    ../src/playbacksession.cpp)
target_compile_definitions(wallpaper-lifecycle PRIVATE WALLPAPER_TESTING)
target_include_directories(wallpaper-lifecycle PRIVATE ../src)
target_link_libraries(wallpaper-lifecycle PRIVATE
    Qt6::Core Qt6::Gui Qt6::Quick Qt6::Qml Qt6::OpenGL
    PkgConfig::MPV PkgConfig::EGL Threads::Threads)

# Native session tests do not need a compositor or a video file.
add_executable(session-lifecycle session.cpp ../src/playbacksession.cpp)
target_include_directories(session-lifecycle PRIVATE ../src)
target_compile_definitions(session-lifecycle PRIVATE WALLPAPER_TESTING)
target_link_libraries(session-lifecycle PRIVATE Qt6::Core PkgConfig::MPV Threads::Threads)
add_test(NAME session-lifecycle COMMAND session-lifecycle)
set_tests_properties(session-lifecycle PROPERTIES TIMEOUT 30 ENVIRONMENT "LC_NUMERIC=C")

# Import the actual packaged DSO, rather than linking the player into the host.
add_executable(qml-lifecycle qml.cpp)
target_link_libraries(qml-lifecycle PRIVATE Qt6::Core Qt6::Gui Qt6::Quick Qt6::Qml)
