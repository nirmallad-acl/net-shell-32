# V6-2: declaring ANY REQUIRES list on `main` disables its implicit
# dependency on every component in the build (tools/cmake/project.cmake).
# This list must therefore stay COMPLETE -- every component a symbol in
# main.c comes from must be listed here, or the build fails with a
# misleading "header not found" error that does not point at this file.
# See tech_spec.md §5.2 / R-9.
idf_component_register(SRCS "main.c"
                       INCLUDE_DIRS "."
                       REQUIRES net_cli
                                console
                                esp_wifi
                                esp_netif
                                esp_event
                                nvs_flash)
