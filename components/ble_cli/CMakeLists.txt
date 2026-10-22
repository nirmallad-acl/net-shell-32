# All dependencies are PRIV_REQUIRES: the public ble_cli.h includes only esp_err.h.
# Layering: ble_cli sits above ble_host and diag_cmds. It must never include
# ADXL345 register or I2C headers (REQ-FUNC-53); the sensor is reachable only
# through diag_cmds.
idf_component_register(SRCS "ble_cli.c"
                            "cli_line_asm.c"
                            "cli_frag.c"
                            "cli_tx_gate.c"
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "private_include"
                       PRIV_REQUIRES ble_host
                                     diag_cmds
                                     bt
                                     freertos
                                     esp_timer
                                     log)
