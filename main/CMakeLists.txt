## AquaGen Lite — sources (full feature set).
##
## INCLUDE_DIRS note: trombik/esp_wireguard exposes wireguard.h / crypto.h / wireguardif.h
## only from its internal src/ dir — wireguard_client.c includes them, so we add that path
## (mirrors the proven modbus_iot_gateway setup). Managed components (esp_wireguard, littlefs)
## are pulled by idf_component.yml and auto-linked — they must NOT be listed in REQUIRES
## (listing an unknown component name breaks the build), except littlefs whose public name is stable.
idf_component_register(
    SRCS
        "main.c"
        "app_config.c"
        "leds.c"
        "wifi_mgr.c"
        "modbus_meter.c"
        "sas_token.c"
        "azure_mqtt.c"
        "telemetry.c"
        "flash_buffer.c"
        "provisioning.c"
        "device_twin.c"
        "ota.c"
        "web_config.c"
        "wireguard_client.c"
    INCLUDE_DIRS
        "."
        "../managed_components/trombik__esp_wireguard/src"
    REQUIRES
        nvs_flash
        esp_wifi
        esp_netif
        esp_event
        esp_system
        driver
        esp_timer
        esp_hw_support
        lwip
        mqtt
        esp-tls
        mbedtls
        json
        littlefs
        esp_http_client
        esp_http_server
        app_update
)
