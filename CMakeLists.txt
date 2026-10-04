cmake_minimum_required(VERSION 3.15)

project(roomy VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)
# Link the C++ runtime statically so that the plugin doesn't depend on a redistributable being installed
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
	set(CMAKE_BUILD_TYPE Release)
endif()

find_package(Threads REQUIRED)

add_library(roomy_core STATIC
	src/config.cpp
	src/net.cpp
	src/suppressor.cpp
)
target_include_directories(roomy_core PUBLIC src)
target_link_libraries(roomy_core PUBLIC Threads::Threads)
if(WIN32)
	target_link_libraries(roomy_core PUBLIC ws2_32)
endif()
if(MSVC)
	target_compile_options(roomy_core PUBLIC /W4 /utf-8)
else()
	target_compile_options(roomy_core PUBLIC -Wall -Wextra)
endif()

add_library(roomy SHARED src/plugin.cpp)
target_include_directories(roomy PRIVATE third_party/mumble)
target_link_libraries(roomy PRIVATE roomy_core)
target_compile_definitions(roomy PRIVATE
	# The oldest plugin API, understood by Mumble 1.4 and everything after it
	MUMBLE_PLUGIN_API_MAJOR_MACRO=1
	MUMBLE_PLUGIN_API_MINOR_MACRO=0
	MUMBLE_PLUGIN_API_PATCH_MACRO=0
	ROOMY_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
	ROOMY_VERSION_MINOR=${PROJECT_VERSION_MINOR}
	ROOMY_VERSION_PATCH=${PROJECT_VERSION_PATCH}
)
if(UNIX AND NOT APPLE)
	target_link_options(roomy PRIVATE -static-libstdc++ -static-libgcc)
endif()

include(CTest)
if(BUILD_TESTING)
	add_executable(test_suppressor test/test_suppressor.cpp)
	target_link_libraries(test_suppressor PRIVATE roomy_core)
	add_test(NAME suppressor COMMAND test_suppressor)

	add_executable(test_net test/test_net.cpp)
	target_link_libraries(test_net PRIVATE roomy_core)
	add_test(NAME net COMMAND test_net)
endif()
