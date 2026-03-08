
cmake_minimum_required(VERSION 3.16)

project(MessengerServer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 REQUIRED COMPONENTS Core Network)

set(CMAKE_AUTOMOC ON)

set(SERVER_SOURCES
    main.cpp
    server.cpp
)

set(SERVER_HEADERS
    server.h
)

add_executable(server ${SERVER_SOURCES} ${SERVER_HEADERS})

target_link_libraries(server
    Qt6::Core
    Qt6::Network
)
