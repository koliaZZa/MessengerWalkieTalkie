cmake_minimum_required(VERSION 3.16)

set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Network Sql Test Widgets)

add_executable(server_security_tests
    server_security_tests.cpp
    ../Server/serverstorage.cpp
    ../Server/serverstorage.h
    ../Server/ratelimiter.cpp
    ../Server/ratelimiter.h
    ../Shared/authprotocol.h
)

target_include_directories(server_security_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/Server
    ${CMAKE_SOURCE_DIR}/Shared
)

target_link_libraries(server_security_tests
    Qt6::Core
    Qt6::Sql
    Qt6::Test
)

add_test(NAME server_security_tests COMMAND server_security_tests)

add_executable(server_rate_limiter_tests
    server_rate_limiter_tests.cpp
    ../Server/ratelimiter.cpp
    ../Server/ratelimiter.h
)

target_include_directories(server_rate_limiter_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/Server
)

target_link_libraries(server_rate_limiter_tests
    Qt6::Core
    Qt6::Test
)

add_test(NAME server_rate_limiter_tests COMMAND server_rate_limiter_tests)

add_executable(server_integration_tests
    server_integration_tests.cpp
    ../Server/server.cpp
    ../Server/server.h
    ../Server/connection.cpp
    ../Server/connection.h
    ../Server/worker.cpp
    ../Server/worker.h
    ../Server/serverstorage.cpp
    ../Server/serverstorage.h
    ../Server/ratelimiter.cpp
    ../Server/ratelimiter.h
    ../Server/logger.cpp
    ../Server/logger.h
    ../Shared/authprotocol.h
    ../Shared/transportprotocol.h
)

target_include_directories(server_integration_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/Server
    ${CMAKE_SOURCE_DIR}/Shared
)

target_link_libraries(server_integration_tests
    Qt6::Core
    Qt6::Network
    Qt6::Sql
    Qt6::Test
)

add_test(NAME server_integration_tests COMMAND server_integration_tests)

if (WIN32)
    get_target_property(QT_QMAKE_EXECUTABLE Qt6::qmake IMPORTED_LOCATION)
    get_filename_component(QT_BIN_DIR "${QT_QMAKE_EXECUTABLE}" DIRECTORY)
    set(QT_WINDEPLOYQT_EXECUTABLE "${QT_BIN_DIR}/windeployqt.exe")

    set(QT_WINDEPLOYQT_MODE --release)
    if (CMAKE_BUILD_TYPE STREQUAL "Debug")
        set(QT_WINDEPLOYQT_MODE --debug)
    endif()

    add_custom_command(TARGET server_integration_tests POST_BUILD
        COMMAND "${QT_WINDEPLOYQT_EXECUTABLE}"
            ${QT_WINDEPLOYQT_MODE}
            --compiler-runtime
            --no-translations
            "$<TARGET_FILE:server_integration_tests>"
        COMMENT "Deploying Qt runtime for server_integration_tests"
        VERBATIM
    )
endif()

add_executable(client_history_tests
    client_history_tests.cpp
    ../Client/historystore.cpp
    ../Client/historystore.h
    ../Shared/authprotocol.h
)

target_include_directories(client_history_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/Client
    ${CMAKE_SOURCE_DIR}/Shared
)

target_link_libraries(client_history_tests
    Qt6::Core
    Qt6::Sql
    Qt6::Test
)

add_test(NAME client_history_tests COMMAND client_history_tests)

add_executable(chat_state_tests
    chat_state_tests.cpp
    ../Client/chatstate.cpp
    ../Client/chatstate.h
    ../Shared/authprotocol.h
)

target_include_directories(chat_state_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/Client
    ${CMAKE_SOURCE_DIR}/Shared
)

target_link_libraries(chat_state_tests
    Qt6::Core
    Qt6::Sql
    Qt6::Test
)

add_test(NAME chat_state_tests COMMAND chat_state_tests)

add_executable(chat_window_integration_tests
    chat_window_integration_tests.cpp
    ../Client/chatstate.cpp
    ../Client/chatstate.h
    ../Client/chatwindow.cpp
    ../Client/chatwindow.h
    ../Client/historystore.cpp
    ../Client/historystore.h
    ../Client/networkclient.cpp
    ../Client/networkclient.h
    ../Client/networkpacketdispatcher.cpp
    ../Client/networkpacketdispatcher.h
    ../Client/networktransportstate.cpp
    ../Client/networktransportstate.h
    ../Client/protocol.h
    ../Server/server.cpp
    ../Server/server.h
    ../Server/connection.cpp
    ../Server/connection.h
    ../Server/worker.cpp
    ../Server/worker.h
    ../Server/serverstorage.cpp
    ../Server/serverstorage.h
    ../Server/ratelimiter.cpp
    ../Server/ratelimiter.h
    ../Server/logger.cpp
    ../Server/logger.h
    ../Shared/authprotocol.h
    ../Shared/transportprotocol.h
)

target_include_directories(chat_window_integration_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/Client
    ${CMAKE_SOURCE_DIR}/Server
    ${CMAKE_SOURCE_DIR}/Shared
)

target_link_libraries(chat_window_integration_tests
    Qt6::Core
    Qt6::Network
    Qt6::Sql
    Qt6::Test
    Qt6::Widgets
)

add_test(NAME chat_window_integration_tests COMMAND chat_window_integration_tests)

add_executable(client_flow_state_tests
    client_flow_state_tests.cpp
    ../Client/clientflowstate.cpp
    ../Client/clientflowstate.h
    ../Shared/authprotocol.h
)

target_include_directories(client_flow_state_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/Client
    ${CMAKE_SOURCE_DIR}/Shared
)

target_link_libraries(client_flow_state_tests
    Qt6::Core
    Qt6::Sql
    Qt6::Test
)

add_test(NAME client_flow_state_tests COMMAND client_flow_state_tests)

add_executable(network_transport_state_tests
    network_transport_state_tests.cpp
    ../Client/networktransportstate.cpp
    ../Client/networktransportstate.h
)

target_include_directories(network_transport_state_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/Client
)

target_link_libraries(network_transport_state_tests
    Qt6::Core
    Qt6::Test
)

add_test(NAME network_transport_state_tests COMMAND network_transport_state_tests)

add_executable(network_packet_dispatcher_tests
    network_packet_dispatcher_tests.cpp
    ../Client/networkpacketdispatcher.cpp
    ../Client/networkpacketdispatcher.h
    ../Shared/authprotocol.h
)

target_include_directories(network_packet_dispatcher_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/Client
    ${CMAKE_SOURCE_DIR}/Shared
)

target_link_libraries(network_packet_dispatcher_tests
    Qt6::Core
    Qt6::Test
)

add_test(NAME network_packet_dispatcher_tests COMMAND network_packet_dispatcher_tests)

add_executable(client_network_integration_tests
    client_network_integration_tests.cpp
    ../Client/networkclient.cpp
    ../Client/networkclient.h
    ../Client/networkpacketdispatcher.cpp
    ../Client/networkpacketdispatcher.h
    ../Client/networktransportstate.cpp
    ../Client/networktransportstate.h
    ../Client/protocol.h
    ../Server/server.cpp
    ../Server/server.h
    ../Server/connection.cpp
    ../Server/connection.h
    ../Server/worker.cpp
    ../Server/worker.h
    ../Server/serverstorage.cpp
    ../Server/serverstorage.h
    ../Server/ratelimiter.cpp
    ../Server/ratelimiter.h
    ../Server/logger.cpp
    ../Server/logger.h
    ../Shared/authprotocol.h
    ../Shared/transportprotocol.h
)

target_include_directories(client_network_integration_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/Client
    ${CMAKE_SOURCE_DIR}/Server
    ${CMAKE_SOURCE_DIR}/Shared
)

target_link_libraries(client_network_integration_tests
    Qt6::Core
    Qt6::Network
    Qt6::Sql
    Qt6::Test
)

add_test(NAME client_network_integration_tests COMMAND client_network_integration_tests)

if (WIN32)
    add_custom_command(TARGET chat_window_integration_tests POST_BUILD
        COMMAND "${QT_WINDEPLOYQT_EXECUTABLE}"
            ${QT_WINDEPLOYQT_MODE}
            --compiler-runtime
            --no-translations
            "$<TARGET_FILE:chat_window_integration_tests>"
        COMMENT "Deploying Qt runtime for chat_window_integration_tests"
        VERBATIM
    )

    add_custom_command(TARGET client_network_integration_tests POST_BUILD
        COMMAND "${QT_WINDEPLOYQT_EXECUTABLE}"
            ${QT_WINDEPLOYQT_MODE}
            --compiler-runtime
            --no-translations
            "$<TARGET_FILE:client_network_integration_tests>"
        COMMENT "Deploying Qt runtime for client_network_integration_tests"
        VERBATIM
    )
endif()

add_executable(client_flow_controller_integration_tests
    client_flow_controller_integration_tests.cpp
    ../Client/chatstate.cpp
    ../Client/chatstate.h
    ../Client/chatwindow.cpp
    ../Client/chatwindow.h
    ../Client/clientflowcontroller.cpp
    ../Client/clientflowcontroller.h
    ../Client/clientflowstate.cpp
    ../Client/clientflowstate.h
    ../Client/connectionwindow.cpp
    ../Client/connectionwindow.h
    ../Client/historystore.cpp
    ../Client/historystore.h
    ../Client/loginwindow.cpp
    ../Client/loginwindow.h
    ../Client/networkclient.cpp
    ../Client/networkclient.h
    ../Client/networkpacketdispatcher.cpp
    ../Client/networkpacketdispatcher.h
    ../Client/networktransportstate.cpp
    ../Client/networktransportstate.h
    ../Client/protocol.h
    ../Server/server.cpp
    ../Server/server.h
    ../Server/connection.cpp
    ../Server/connection.h
    ../Server/worker.cpp
    ../Server/worker.h
    ../Server/serverstorage.cpp
    ../Server/serverstorage.h
    ../Server/ratelimiter.cpp
    ../Server/ratelimiter.h
    ../Server/logger.cpp
    ../Server/logger.h
    ../Shared/authprotocol.h
    ../Shared/transportprotocol.h
)

target_include_directories(client_flow_controller_integration_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/Client
    ${CMAKE_SOURCE_DIR}/Server
    ${CMAKE_SOURCE_DIR}/Shared
)

target_link_libraries(client_flow_controller_integration_tests
    Qt6::Core
    Qt6::Network
    Qt6::Sql
    Qt6::Test
    Qt6::Widgets
)

add_test(NAME client_flow_controller_integration_tests COMMAND client_flow_controller_integration_tests)

if (WIN32)
    add_custom_command(TARGET client_flow_controller_integration_tests POST_BUILD
        COMMAND "${QT_WINDEPLOYQT_EXECUTABLE}"
            ${QT_WINDEPLOYQT_MODE}
            --compiler-runtime
            --no-translations
            "$<TARGET_FILE:client_flow_controller_integration_tests>"
        COMMENT "Deploying Qt runtime for client_flow_controller_integration_tests"
        VERBATIM
    )
endif()

