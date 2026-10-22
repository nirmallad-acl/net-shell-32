# All dependencies are PRIV_REQUIRES: the public headers include only
# <stdbool.h>, <stdint.h>, <stddef.h> and esp_err.h (plus one forward
# declaration of struct ble_gatt_svc_def, accepted at Gate 2 / G2-03), so no
# NimBLE type leaks to consumers.
#
# REQ-FUNC-70 build check: this component must never depend on any of its
# clients (the components that register GATT services with it). Do not name a
# client component anywhere in this file.
idf_component_register(SRCS "ble_host.c"
                            "host_registry.c"
                            "host_exec.c"
                            "adv_payload.c"
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "private_include"
                       PRIV_REQUIRES bt
                                     nvs_flash
                                     esp_timer
                                     freertos
                                     log
                                     esp_rom)
