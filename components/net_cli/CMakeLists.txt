# All dependencies are PRIV_REQUIRES: net_cli.h includes only <stdbool.h> and
# esp_err.h (a common requirement), so nothing in the public header leaks an
# esp_wifi/esp_netif/lwip/console type to a consumer (NFR-22 / P-B1).
#
# `main` is deliberately NOT listed and main/ is not on any include path --
# this is the NFR-23 enforcement point (P-B2): a lower layer that tried to
# call back into the command layer, or into main.c, would fail to compile.
#
# See tech_spec.md §5.2 for the full rationale, including why `lwip` belongs
# here (ping/ping_sock.h, V6-1) rather than in main's REQUIRES.
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
