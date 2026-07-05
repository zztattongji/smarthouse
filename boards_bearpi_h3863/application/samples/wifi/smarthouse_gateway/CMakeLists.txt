set(SOURCES "${SOURCES}"
    "${CMAKE_CURRENT_SOURCE_DIR}/main.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/sle_bus.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/msg_builder.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../mqtt_demo/wifi/wifi_connect.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../products/sle_gateway/sle_uart_server/sle_uart_server.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../products/sle_gateway/sle_uart_server/sle_uart_server_adv.c"
    PARENT_SCOPE)

set(PUBLIC_HEADER "${PUBLIC_HEADER}"
    "${CMAKE_CURRENT_SOURCE_DIR}/../mqtt_demo/wifi"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../products/sle_gateway/sle_uart_server"
    PARENT_SCOPE)

