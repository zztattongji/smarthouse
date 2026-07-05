set(SOURCES "${SOURCES}"
    "${CMAKE_CURRENT_SOURCE_DIR}/main.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/sle_bus.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/msg_builder.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/sht30.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/bmp280.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/sgp30.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/pms5003.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../i2c/ssd1306.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../i2c/ssd1306_fonts.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../products/sle_gateway/sle_uart_client/sle_uart_client.c"
    PARENT_SCOPE)

set(PUBLIC_HEADER "${PUBLIC_HEADER}"
    "${CMAKE_CURRENT_SOURCE_DIR}/../i2c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../products/sle_gateway/sle_uart_client"
    PARENT_SCOPE)


