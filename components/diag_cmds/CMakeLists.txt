# All dependencies are PRIV_REQUIRES: the public diag_cmds.h includes only
# <stdbool.h>, <stddef.h>, <stdint.h> and esp_err.h. Layering: diag_cmds sits
# above accel_sensor, device_info and ble_ota (reboot guard) and below ble_cli;
# it must never depend on ble_cli or ble_host (the BLE transport reaches it
# only through diag_ctx_t).
idf_component_register(SRCS "diag_cmds.c"
                            "diag_serial.c"
                            "diag_format.c"
                            "diag_resmon.c"
                            "accel_sched.c"
                            "accel_session.c"
                            "accel_session_core.c"
                            "cmd_hw_version.c"
                            "cmd_fw_version.c"
                            "cmd_reboot_reason.c"
                            "cmd_reboot.c"
                            "cmd_accel_xyz.c"
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "private_include"
                       PRIV_REQUIRES accel_sensor
                                     device_info
                                     ble_ota
                                     console
                                     nvs_flash
                                     esp_driver_uart
                                     esp_timer
                                     esp_system
                                     freertos
                                     heap
                                     log)
