cmake_minimum_required(VERSION 3.16)
project(Messenger VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(CTest)

add_subdirectory(Server)
add_subdirectory(Client)

if (BUILD_TESTING)
    add_subdirectory(Tests)
endif()

