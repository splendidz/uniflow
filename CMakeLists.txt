# uniflow - cross-platform build for the C++ examples.
#
#   cmake -S . -B build
#   cmake --build build
#
# On Linux/macOS this builds the console examples (the two flagship examples,
# city_traffic and pick_and_place, fall back to an ANSI console renderer off
# Windows). On Windows it also builds the Win32 GDI back-ends and weather_llm.
# The framework itself is a single header with no external dependencies.
cmake_minimum_required(VERSION 3.16)
project(uniflow LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)

# The framework: a single header. Consumers just need it on the include path.
add_library(uniflow INTERFACE)
target_include_directories(uniflow INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/cpp)
target_link_libraries(uniflow INTERFACE Threads::Threads)

# Build one example executable from every .cpp in its folder.
function(uniflow_example name)
    file(GLOB sources CONFIGURE_DEPENDS
         ${CMAKE_CURRENT_SOURCE_DIR}/cpp/examples/${name}/*.cpp)
    add_executable(${name} ${sources})
    target_link_libraries(${name} PRIVATE uniflow)
endfunction()

# Console examples - build on every platform.
foreach(example
        simulator
        message_dispatch
        queue_drain
        shared_ostream
        city_traffic
        pick_and_place)
    uniflow_example(${example})
endforeach()

if(WIN32)
    # The flagship dual renderers use the Win32 GDI back-end on Windows.
    target_link_libraries(city_traffic   PRIVATE user32 gdi32)
    target_link_libraries(pick_and_place PRIVATE user32 gdi32)
    # weather_llm uses WinHTTP and is Windows-only.
    uniflow_example(weather_llm)
    target_link_libraries(weather_llm PRIVATE winhttp)
endif()
