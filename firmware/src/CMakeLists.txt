idf_component_register(
    SRCS        "main.c"
                "hmu_frame.c"
                "bridge.c"
                "source_usb.c"
                "source_uart.c"
                "sink_debug.c"
                "net.c"
                "sink_tcp.c"
                "sink_hbrfeth.c"
                "sink_hmuartlgw_legacy.c"
                "log_buffer.c"
                "webui.c"
                "mdns_glue.c"
                "ota_check.c"
                "improv_glue.cpp"
    INCLUDE_DIRS "."
    REQUIRES     usb driver usb_host_cdc_acm
                 nvs_flash esp_wifi esp_event esp_netif lwip
                 esp_http_server esp_http_client esp-tls
                 app_update esp_app_format esp_timer
                 json
                 improv_wifi_busware
                 mdns
    EMBED_FILES
        "${CMAKE_CURRENT_SOURCE_DIR}/../web/index.html.gz"
        "${CMAKE_CURRENT_SOURCE_DIR}/../web/app.css.gz"
        "${CMAKE_CURRENT_SOURCE_DIR}/../web/app.js.gz"
        "${CMAKE_CURRENT_SOURCE_DIR}/../web/assets/busmatic.png"
        "${CMAKE_CURRENT_SOURCE_DIR}/../web/assets/busmatic_white.png"
)
