# All dependencies are PRIV_REQUIRES: the public net_cli.h includes only
# <stdbool.h> and esp_err.h, so no Wi-Fi/network types leak to consumers.
# This prevents bidirectional dependencies: a lower layer cannot call back
# into main.c or the command layer.
idf_component_register(SRCS "net_cli.c"
                            "net_cli_wifi_state.c"
                            "cmd_ifconfig.c"
                            "cmd_wifiscan.c"
                            "cmd_wifi_connect.c"
                            "cmd_ping.c"
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "private_include"
                       PRIV_REQUIRES console
                                     esp_wifi
                                     esp_netif
                                     esp_event
                                     lwip)
