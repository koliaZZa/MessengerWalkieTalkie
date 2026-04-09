cmake_minimum_required(VERSION 3.16)

project(MessengerServer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Network Sql)

add_executable(server
    main.cpp
    server.cpp
    server.h
    protocol.h
    connection.cpp
    connection.h
    worker.cpp
    worker.h
    serverstorage.cpp
    serverstorage.h
    ratelimiter.cpp
    ratelimiter.h
    logger.cpp
    logger.h
    ../Shared/authprotocol.h
    ../Shared/transportprotocol.h
)

target_link_libraries(server
    Qt6::Core
    Qt6::Network
    Qt6::Sql
)

target_include_directories(server PRIVATE ${CMAKE_SOURCE_DIR}/Shared)

if (WIN32)
    get_target_property(QT_QMAKE_EXECUTABLE Qt6::qmake IMPORTED_LOCATION)
    get_filename_component(QT_BIN_DIR "${QT_QMAKE_EXECUTABLE}" DIRECTORY)
    set(QT_WINDEPLOYQT_EXECUTABLE "${QT_BIN_DIR}/windeployqt.exe")

    set(QT_WINDEPLOYQT_MODE --release)
    if (CMAKE_BUILD_TYPE STREQUAL "Debug")
        set(QT_WINDEPLOYQT_MODE --debug)
    endif()

    add_custom_command(TARGET server POST_BUILD
        COMMAND "${QT_WINDEPLOYQT_EXECUTABLE}"
            ${QT_WINDEPLOYQT_MODE}
            --compiler-runtime
            --no-translations
            "$<TARGET_FILE:server>"
        COMMENT "Deploying Qt runtime for server"
        VERBATIM
    )
endif()

