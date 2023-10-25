cmake_minimum_required(VERSION 3.10)

project(QuattrocentoLSLApp
	LANGUAGES C CXX
	VERSION 1.0.0
	)

# also look for CMake modules in the cmake subfolder
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# find an installed liblsl in paths set by the user (LSL_INSTALL_ROOT)
# and some default paths
find_package(LSL REQUIRED
	HINTS ${LSL_INSTALL_ROOT}
	PATH_SUFFIXES lib/cmake/LSL)
get_filename_component(LSL_PATH ${LSL_CONFIG} DIRECTORY)
message(STATUS "Found LSL lib in ${LSL_PATH}")
LSLAPP_Setup_Boilerplate()

# Qt is a popular-enough library that cmake already knows how to find it.
# 
# Nevertheless, it is often necessary in Windows to give cmake a hint where to
# find it by adding the following to your cmake invocation:
# -DQt5_Dir="C:/Qt/<version>/<platform>/lib/cmake/Qt5"
# Where <version> is 5.something and <platform> is something like msvc2017_64
# If you are on MacOS and you installed Qt5 via homebrew, then use the following:
# -DQt5_DIR=$(brew --prefix qt5)/lib/cmake/Qt5
#
# Then, to import Qt, just call `find_package` with the components your app uses
# (usually just `Widgets`).
set(CMAKE_INCLUDE_CURRENT_DIR ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)
find_package(Qt5 REQUIRED COMPONENTS Widgets Network)

find_package(Threads REQUIRED)

# add executable targets, the default target has the same name as the project
add_executable(${PROJECT_NAME} MACOSX_BUNDLE WIN32
	main.cpp
	mainwindow.cpp
	mainwindow.hpp
	mainwindow.ui
	reader.hpp
	reader.cpp
)

target_link_libraries(${PROJECT_NAME}
	PRIVATE
	Qt5::Widgets
	Qt5::Network
	Threads::Threads
	LSL::lsl
)

target_compile_features(${PROJECT_NAME} PRIVATE cxx_std_14)

# setting up deployment (let CPack generate a zip/dmg file)
installLSLApp(${PROJECT_NAME})
installLSLAuxFiles(${PROJECT_NAME}
	${PROJECT_NAME}.cfg
)
LSLGenerateCPackConfig()
