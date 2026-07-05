set(SOURCES "${SOURCES}"
    "${CMAKE_CURRENT_SOURCE_DIR}/main.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../sle_bus.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../msg_builder.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../products/sle_uart/sle_uart_client/sle_uart_client.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../products/sle_uart/sle_uart_client/led_ctrl.c"
    PARENT_SCOPE)

set(PUBLIC_HEADER "${PUBLIC_HEADER}"
    "${CMAKE_CURRENT_SOURCE_DIR}/.."
    "${CMAKE_CURRENT_SOURCE_DIR}/../../products/sle_uart/sle_uart_client"
    PARENT_SCOPE)
