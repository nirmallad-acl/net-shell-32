# All dependencies are PRIV_REQUIRES: the public device_info.h includes only
# <stdbool.h>, <stddef.h> and esp_err.h. No Bluetooth dependency (REQ-FUNC-26).
idf_component_register(SRCS "device_info.c"
                            "devinfo_validate.c"
                            "reset_reason_map.c"
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "private_include"
                       PRIV_REQUIRES nvs_flash
                                     esp_app_format
                                     esp_system
                                     log)
