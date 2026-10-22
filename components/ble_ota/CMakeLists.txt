# All dependencies are PRIV_REQUIRES: the public ble_ota.h includes only
# <stdbool.h>, <stdint.h> and esp_err.h, so no NimBLE/esp_ota_*/Wi-Fi/FreeRTOS
# type leaks to consumers -- same reasoning as net_cli's CMakeLists.txt.
# NETSHELL32-5: the NimBLE host is owned by ble_host; this component is one of
# its clients (ble_ota_gate.c is the pure arm/guard decision module).
idf_component_register(SRCS "ble_ota.c"
                            "ble_ota_gate.c"
                            "ble_ota_proto_defs.c"
                            "ble_ota_transport_nimble.c"
                            "ble_ota_proto.c"
                            "ble_ota_flash.c"
                            "ble_ota_health.c"
                            "cmd_fwupgrade.c"
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "private_include"
                       PRIV_REQUIRES console
                                     bt
                                     ble_host
                                     app_update
                                     esp_partition
                                     nvs_flash
                                     esp_timer
                                     esp_wifi
                                     esp_event
                                     esp_system
                                     freertos)
