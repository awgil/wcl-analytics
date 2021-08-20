cmake_minimum_required(VERSION 3.21)
project(wcl-analytics CXX)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/.bin")

include(FetchContent)

# third-party: C++ Requests
option(CPR_BUILD_TESTS OFF) # disable CPR tests...
FetchContent_Declare(cpr
	GIT_REPOSITORY https://github.com/whoshuu/cpr.git
	GIT_TAG master
)
FetchContent_MakeAvailable(cpr)

# third-party: nlohmann json
option(JSON_ImplicitConversions OFF) # disable implicit conversions
FetchContent_Declare(nlohmann_json
	GIT_REPOSITORY https://github.com/ArthurSonzogni/nlohmann_json_cmake_fetchcontent
	GIT_TAG master
)
FetchContent_MakeAvailable(nlohmann_json)

# main project
file(GLOB_RECURSE src RELATIVE "${CMAKE_SOURCE_DIR}" CONFIGURE_DEPENDS "src/*")
add_executable(wcl-analytics ${src} nlohmann_json.natvis)
target_compile_features(wcl-analytics PRIVATE cxx_std_23)
target_compile_options(wcl-analytics PRIVATE /W4 /WX)
target_link_libraries(wcl-analytics PRIVATE cpr::cpr nlohmann_json::nlohmann_json)
target_link_options(wcl-analytics PRIVATE /DEBUG)

set_property(DIRECTORY ${CMAKE_CMAKE_CURRENT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT wcl-analytics)

