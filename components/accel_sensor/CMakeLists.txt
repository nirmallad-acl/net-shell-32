# All dependencies are PRIV_REQUIRES: the public accel_sensor.h includes only
# <stdbool.h>, <stdint.h> and esp_err.h, so no I2C driver type leaks to
# consumers, and the ADXL345 register definitions stay private (REQ-FUNC-53).
idf_component_register(SRCS "accel_sensor.c"
                            "accel_bus_i2c.c"
                            "adxl345.c"
                            "accel_sim.c"
                            "accel_select.c"
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "private_include"
                       PRIV_REQUIRES esp_driver_i2c
                                     esp_driver_gpio
                                     esp_timer
                                     esp_hw_support
                                     freertos
                                     log)
