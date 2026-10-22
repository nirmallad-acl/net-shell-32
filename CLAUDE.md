# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project overview

`net-shell-32` is an ESP-IDF firmware project targeting the ESP32. It boots to a serial REPL (`esp32_net>` prompt, UART0) offering network commands (`ifconfig`, `wifiscan`, `wifi_connect`, `ping`), a BLE OTA firmware upgrade command (`fwupgrade start|stop|status`) and five diagnostic commands (`hw_version`, `fw_version`, `accel_xyz`, `reboot_reason`, `reboot`, plus serial-only `hw_rev_set`). The same five diagnostic commands are also served over a BLE UART-style CLI (always advertised from boot, Just Works pairing, see `docs/ble_cli.md`).

## Build system

This is a standard ESP-IDF (CMake-based) project — there is no Makefile, npm, or other build tooling. All commands go through `idf.py`, which requires the ESP-IDF environment to be sourced first (`$IDF_PATH/export.sh`) or invoked from a shell where it's already sourced.

Common commands:
- `idf.py set-target esp32` — set/confirm the build target (project is configured for `esp32`)
- `idf.py build` — configure and build the project
- `idf.py -p <PORT> flash` — flash the built firmware to a connected device
- `idf.py -p <PORT> monitor` — open the serial monitor (Ctrl+] to exit)
- `idf.py -p <PORT> flash monitor` — flash then immediately monitor
- `idf.py fullclean` — remove the `build/` directory and configuration cache
- `idf.py menuconfig` — interactive Kconfig configuration (writes `sdkconfig`, which is gitignored)

There is no CI and no README.md — this file is the only orientation doc. Host unit tests exist for the pure (no FreeRTOS/NimBLE) sources under `test/host/` (command in the Architecture notes below); everything else is verified with `idf.py build` (compile check) and on target. BLE behaviour and timing cannot be validated in QEMU.

## Architecture

- `main/main.c` — `app_main()` runs a *conditional* boot sequence (NVS, netif, event loop, Wi-Fi, CLI, `ble_host_init` (9.4), BLE OTA init (9.5), `device_info_init` (9.6), `accel_sensor_init` (9.7), `diag_cmds_init` + `ble_cli_init` (9.8), console and command registration incl. `diag_cmds_register_serial` (13.6), REPL, and last `ble_host_start` (15.6)). A step runs only if its prerequisites succeeded; skipped steps record `NET_CLI_ERR_SKIPPED`. Boot is degraded-not-fatal, except console init/start failure, which halts.
- `main/CMakeLists.txt` — declares an explicit `REQUIRES` list, which disables implicit dependency on all components. Every component used by `main.c` must be listed there, or the build fails with a misleading "header not found" error.
- `components/net_cli` — Wi-Fi/network commands, one `cmd_*.c` per command, registered by `net_cli_register_all()`.
- `components/ble_ota` — BLE (NimBLE) OTA with rollback and pairing. Layers: transport (`ble_ota_transport_nimble.c`), protocol (`ble_ota_proto*.c`), flash writer, health checks, `cmd_fwupgrade.c` (the `fwupgrade start|stop|status` command, in `components/ble_ota/`). Tunables live in its `Kconfig`.
- NETSHELL32-5 components (BLE UART CLI, diagnostics, ADXL345): `components/ble_host` (neutral NimBLE owner, started last in `main.c` step 15.6; clients register GATT services before start), `ble_cli` (BLE CLI client), `diag_cmds` (shared command layer for serial and BLE: `hw_version fw_version accel_xyz reboot_reason reboot`, serial-only `hw_rev_set`), `accel_sensor` (ADXL345 driver + simulator), `device_info` (version sources, reset reason). `ble_ota` is now a `ble_host` client: `fwupgrade start|stop` arm/disarm OTA, they no longer start or stop BLE. User docs: `docs/ble_cli.md`; hardware items: `docs/hw_validation_checklist.md`.
- Layering (extends the rule below): `ble_host` never depends on its clients (`ble_cli`, `ble_ota`) or on `diag_cmds`; `ble_cli` -> `ble_host`, `diag_cmds`; `diag_cmds` -> `accel_sensor`, `device_info`, `ble_ota`; ADXL345 register/I2C headers stay private to `accel_sensor`. Registration order matters: `ble_ota` must be registered before `ble_cli` (invariant I-1, handles of the OTA service stay as in NETSHELL32-2). `ble_ota_proto*.c`, `ble_ota_flash.c`, `ble_ota_health.c` were deliberately left unmodified by NETSHELL32-5 (the REQ-FUNC-74 diff check against master must stay empty); change them only after re-verifying assumption A-01 of the reboot guard (only `ble_ota_proto_rx()` on the host task leaves IDLE).
- Component convention: public header in `include/` exposes only `<stdbool.h>` and `esp_err.h` types (plus `<stdint.h>`/`<stddef.h>`); everything else is in `private_include/`; all dependencies are `PRIV_REQUIRES`. Keep IDF types out of public headers. Single approved exception: `ble_host_client.h` forward-declares `struct ble_gatt_svc_def;` (no NimBLE header included). GATT-client components (`ble_cli`, the `ble_ota` transport) may include NimBLE headers privately for their own service tables and access callbacks; what they must not do is own the host (`nimble_port_*`, `ble_gatts_count_cfg/add_svcs`, `ble_gap_adv_*`, `ble_gap_security_initiate`, `ble_hs_cfg` live only in `ble_host`). New source files must be added to the component's `SRCS`.
- IDF upgrade note: `ble_host` relies on properties of the ESP32 NimBLE NPL port (`npl_os_freertos.c`: event pool size == queue depth, de-duplication of an already queued event, `portMAX_DELAY` in task-context `ble_npl_eventq_put`). Re-verify them on any ESP-IDF upgrade (risk R-18).
- Version: the firmware version comes only from `version.txt` (validated by the top-level `CMakeLists.txt` before `project.cmake`); a `PROJECT_VER` given on the command line (`-D`) or present in the CMake cache fails the configure step (an environment variable of that name is not checked), so do not set it.
- Host unit tests (pure sources only, mocks, no hardware): `cmake -S test/host -B test/host/build && cmake --build test/host/build && ctest --test-dir test/host/build --output-on-failure` (needs host gcc/cmake and `IDF_PATH`). Test-only Kconfig fragments live in `test/configs/`; the `NS5_TEST_*` hooks in `main/Kconfig.projbuild` must never be enabled in a shipped image.
- Layering rule: lower layers must never call back into `main.c` or the command layer; dependencies flow one way (`main` → components). This is why `main.c` must not need getters added to `net_cli`.
- Boot-status facts for `ble_ota` health checks go through `ble_ota_note_boot_status()`.

## Configuration

- `sdkconfig.defaults` is the source of truth for non-default Kconfig values (`sdkconfig` is gitignored). Add new settings there, with justification comments.
- `partitions.csv` — custom 4 MB dual-OTA layout (`ota_0`/`ota_1`, no factory app). App rollback is enabled: new images boot PENDING_VERIFY and must pass `ble_ota` health checks. Changing partition sizes affects OTA image limits.
- Prompt output must stay plain ASCII: `CONFIG_LOG_COLORS=n` is pinned deliberately.
- Workitem specs (`tech_spec.md`, requirement IDs such as FR-xxx cited in code comments) live in `workitem/<JIRA-ID>-<feature-name>/`, not in git. The firmware-agents plugin uses `Workitem Root: workitem` because writes under `.claude/` always prompt for permission.

## Development environment

A devcontainer is provided (`.devcontainer/`) based on the official `espressif/idf` Docker image, intended for QEMU-based ESP-IDF development without physical hardware. It installs `udev`, sources `export.sh` automatically in `.bashrc`, and runs `--privileged` (needed for device/QEMU access).

VS Code config (`.vscode/`) assumes the ESP-IDF VS Code extension and a local ESP-IDF install at `~/.espressif/v6.0.2/esp-idf`; `clangd` is configured to read `compile_commands.json` from `build/` (generated by `idf.py build`) for IntelliSense/completion. `.clangd` strips `-f*`/`-m*` compiler flags that clangd doesn't understand from the Xtensa toolchain.

## Git branch naming convention

When creating a new branch for a JIRA/workitem ID (e.g. the firmware-agents `new-feature` pipeline's Phase 3 "Branch Setup"), name it:

    user/<user-name>/<JIRA-ID>

- No additional prefix beyond `user/<user-name>/` unless the user explicitly specifies one for that branch.
- `<user-name>` identifies the developer (e.g. derived from `git config user.name` or the local part of their email address) — resolve it at branch-creation time rather than hardcoding a specific person.
- If a branch with that exact name already exists, append a numeric suffix `_<num>`, starting at `_1` and incrementing (`_2`, `_3`, ...) until an unused name is found — e.g. `user/nirmal/NETSHELL32-1`, then `user/nirmal/NETSHELL32-1_1`, `user/nirmal/NETSHELL32-1_2`, etc.

## Pull request convention

- PR title: `<JIRA-ID>: <title>` (e.g. `NETSHELL32-1: Implement serial network CLI`).
- PR body follows the template at `.github/PULL_REQUEST_TEMPLATE.md` — bold section labels **Description**, **JIRA**, **Documentation**, **Testing and Results** (the last including a test-case table with steps and Pass/Fail status, and a link to the raw test/log evidence). GitHub auto-fills this template for PRs opened via its UI; use `gh pr create --body-file .github/PULL_REQUEST_TEMPLATE.md` (then edit) when opening one via the CLI.
- Never commit a raw log/output file containing a real credential (Wi-Fi password, token, etc.) — redact it first. Test-evidence logs referenced in a PR belong under `docs/verification/<JIRA-ID>/` (`*.log` is gitignored except under `docs/verification/`), separate from any gitignored agent-internal working state.

## Target hardware

- **Board:** ESP32 DevKit-C v1 (30-pin standard pinout)
- **Module:** ESP32-WROOM-32
- **Flash:** 4MB
- **Console:** UART0 (115200 8N1) via Micro-USB (CH340G converter)
- **Buttons:** Boot and EN (board-level, not firmware-controlled)
