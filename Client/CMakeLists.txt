cmake_minimum_required(VERSION 3.16)

project(MessengerClient LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Network Widgets Sql)

add_executable(client WIN32
    main.cpp
    chatwindow.cpp
    chatwindow.h
    clientflowcontroller.cpp
    clientflowcontroller.h
    connectionwindow.cpp
    connectionwindow.h
    historystore.cpp
    historystore.h
    loginwindow.cpp
    loginwindow.h
    networkclient.cpp
    networkclient.h
    protocol.h
)

target_link_libraries(client
    Qt6::Core
    Qt6::Network
    Qt6::Widgets
    Qt6::Sql
)

if (WIN32)
    get_target_property(QT_QMAKE_EXECUTABLE Qt6::qmake IMPORTED_LOCATION)
    get_filename_component(QT_BIN_DIR "${QT_QMAKE_EXECUTABLE}" DIRECTORY)
    set(QT_WINDEPLOYQT_EXECUTABLE "${QT_BIN_DIR}/windeployqt.exe")

    set(QT_WINDEPLOYQT_MODE --release)
    if (CMAKE_BUILD_TYPE STREQUAL "Debug")
        set(QT_WINDEPLOYQT_MODE --debug)
    endif()

    add_custom_command(TARGET client POST_BUILD
        COMMAND "${QT_WINDEPLOYQT_EXECUTABLE}"
            ${QT_WINDEPLOYQT_MODE}
            --compiler-runtime
            --no-translations
            "$<TARGET_FILE:client>"
        COMMENT "Deploying Qt runtime for client"
        VERBATIM
    )
endif()
