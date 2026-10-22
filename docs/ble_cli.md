# BLE UART CLI, device diagnostics and ADXL345 sensor (NETSHELL32-5)

This document is the user documentation for the BLE command-line interface, the five
diagnostic commands, the version and hardware-revision sources, the ADXL345 sensor
support and the OTA arm/disarm behaviour. Status of unverified items is stated
explicitly: **anything marked PENDING has not been validated on hardware.**

## 1. Quick start with nRF Connect (Android)

If the phone was ever bonded to a NETSHELL32-2 image, or the board's flash was erased
(`idf.py erase-flash`), do step 0 first.

0. **Forget the old bond.** Android Settings > Bluetooth > NETSHELL32 > **Forget**. In
   nRF Connect delete the bond and clear the scanner list (toggle Bluetooth off and on to
   drop the Android GATT cache). After any `idf.py erase-flash` the device has lost its
   bond store but the phone keeps its keys, so encryption fails and Android does not
   re-pair on its own until you forget the device.
1. Power the board. The device advertises as **NETSHELL32** within a few seconds of the
   `esp32_net>` prompt appearing (no `fwupgrade start` is needed).
2. In nRF Connect scan and tap **CONNECT**. The device itself requests pairing (Just
   Works, no PIN): accept the prompt. **Accept it promptly: an unsecured link is
   terminated 60 s after connecting** (section 6).
3. Open the CLI service (UUID in section 2). Tap the notify (triple arrow) icon on the
   **TX** characteristic.
4. On the **RX** characteristic choose the write icon, type **UTF-8/TEXT** and send a
   command followed by a line feed (nRF Connect: use the "new line" option, or the BYTE
   ARRAY type and append `0A`), for example `hw_version` + LF.
5. Responses arrive as notifications on TX, one line per response, LF terminated.

The same five commands also work on the serial console (`esp32_net>`, UART0).

## 2. GATT service

| Attribute | UUID | Properties | Security |
| :--- | :--- | :--- | :--- |
| CLI service (primary) | `8F3B1A20-5C6D-4E7F-9A0B-1C2D3E4F5A60` | | |
| RX (phone to device) | `8F3B1A21-5C6D-4E7F-9A0B-1C2D3E4F5A60` | Write, Write Without Response | encrypted link required, bond checked at runtime |
| TX (device to phone) | `8F3B1A22-5C6D-4E7F-9A0B-1C2D3E4F5A60` | Notify | CCCD per NimBLE |

The OTA service (`fb1e4001-54ae-4a28-9f74-dfccb248601d`, RX `...4002...`, TX `...4003...`)
is unchanged from NETSHELL32-2 and is registered first, so its handles are identical to
NETSHELL32-2 (`FWUPG_HANDLES rx=<h> tx=<h>` in the log shows them).

Encoding: ASCII. Input line terminators: LF or CRLF. Output terminator: LF.

## 3. Commands

| Command | Response |
| :--- | :--- |
| `hw_version` | `OK hw_version=<rev>` or `ERR unavailable cmd=hw_version` |
| `fw_version` | `OK fw_version=<ver>` or `ERR unavailable cmd=fw_version` |
| `reboot_reason` | `OK reboot_reason=<token> code=<n>` |
| `accel_xyz` | `OK accel_xyz started interval_ms=1000 samples=15`, 15 x `DATA ...`, then `OK accel_xyz complete samples=15[ errors=<k>]` |
| `reboot` | `OK rebooting`, then the device restarts |

Examples:

```text
> hw_version
OK hw_version=Alpha_1.0.0_HW
> reboot_reason
OK reboot_reason=power_on code=1
> accel_xyz
OK accel_xyz started interval_ms=1000 samples=15
DATA sample=1 x=0.012 y=-0.103 z=0.998 unit=g age_ms=0 source=simulated
...
OK accel_xyz complete samples=15
> reboot
OK rebooting
```

`reboot_reason` tokens: `unknown power_on external software panic int_wdt task_wdt wdt
deepsleep brownout sdio usb jtag efuse power_glitch cpu_lockup` (codes 0..15).

Response grammar: `OK <...>` | `DATA <...>` | `ERR <code>[ cmd=<name>][ key=value...]`, with
`<code>` one of `unknown_command invalid_argument oversized_input unavailable sensor_error
sensor_stale busy internal`. Every line is printable ASCII, at most 127 characters plus LF.

### Input rules and error precedence

* Limit: **64 bytes per line** excluding the terminator. A longer line is discarded up to
  its LF and answered with one `ERR oversized_input`; no prefix of it is ever executed.
* A single write may carry at most 512 bytes (longer: ATT error 0x0D, nothing executed).
* Precedence at the end of a line: oversized, then invalid character (anything outside
  printable ASCII, TAB, CR: `ERR invalid_argument`), then unknown command, then extra
  arguments (`ERR invalid_argument cmd=<name>`).
* Names are case-sensitive and the set is closed to the five above: `help`, `ifconfig`,
  `wifiscan`, `wifi_connect`, `ping`, `fwupgrade` and `hw_rev_set` are **not** available over
  BLE (`ERR unknown_command`).
* Empty or whitespace-only lines produce no output. A line without a terminator waits for
  more bytes. Commands in one write run in order.
* Responses longer than ATT_MTU - 3 are split into several notifications; concatenating
  them reproduces the line exactly. Only one `accel_xyz` session exists system-wide: a
  second request gets `ERR busy cmd=accel_xyz`.
* More than 4 commands queued at once: the rest of the burst is dropped and one
  `ERR busy reason=input_overflow` is sent (order is never violated).

## 4. Security and pairing

* The CLI requires an encrypted **and bonded** link. Unbonded writes are rejected (ATT
  0x0F Insufficient Encryption, or 0x05 when encrypted but not bonded) and no response
  bytes are ever sent to an insecure or unsubscribed peer.
* Pairing is **Just Works** (no PIN, no MITM protection). Residual risks, accepted at
  Gate 1: any nearby phone can bond and run diagnostics or `reboot`; four other phones
  bonding in turn evict the oldest bond (round-robin, 4 bonds; forget and re-pair);
  a bonded peer can send `reboot` repeatedly; a bonded but idle peer holds the single
  connection slot. Two more (R-10, ADV-IMPL-14): a peer that spoofs the identity address
  of a bonded phone triggers the repeat-pairing handler, which **deletes the real bond**
  of that identity (the owner must re-pair); and a peer that reconnects right after the
  60 s unsecured-link timeout can keep the single slot occupied again and again.
* One connection at a time. A second connection attempt is terminated.
* A link that is not encrypted and bonded is terminated **60 s after connecting**
  (`BLECLI_SEC_TIMEOUT`); advertising resumes at once. It is a fixed bound: a pairing that
  starts after about 55 s can be cut. `CONFIG_BLE_HOST_SECURE_DEADLINE_S` (10..120) is the lever.
* If `CONFIG_BLE_HOST_INITIATE_SECURITY=n` were ever chosen (fallback for a pairing
  collision with the OTA app), pairing starts on the first CLI write instead of on connect
  and the "no manual bond step" criterion is not met; that deviation would need to be
  recorded and accepted. The default is `y`.

## 5. OTA (fwupgrade) with the shared BLE host

BLE is up from boot. The OTA service is present but **unusable until armed**:

* `fwupgrade start` arms OTA (advertised UUID switches to the OTA service within 1 s, the
  CLI UUID moves to the scan response); `fwupgrade stop` disarms it (back within 1 s). The
  radio, the host and an engaged CLI link stay up.
* OTA writes while disarmed are rejected with ATT 0x03 Write Not Permitted
  (`FWUPG_RX_REJECT reason=not_armed`).
* `stop` terminates the link only if it is OTA-only; a CLI-engaged link is kept, and a link
  where nothing was engaged is kept.
* After a disconnect while armed, OTA stays armed for `CONFIG_BLE_OTA_READVERTISE_WINDOW_S`
  (default 300 s), then disarms; advertising never stops.
* `fwupgrade start` while the running image is PENDING_VERIFY is refused with the existing
  text; the BLE CLI stays available (a documented change from NETSHELL32-2).
* A CLI peer and the OTA app can share the same phone link (same-phone path); if the
  existing OTA app insists on its own connection, disconnect the CLI first (limitation R-04).

`fwupgrade status` fields: `ble: up|starting|down`, `armed: yes|no|arming|disarming|drain_stuck`,
`adv mode: armed|disarmed`, `advertising`, `connected`, `bonded` (now describing the shared
link), `peer_connected`, `cli_engaged`, `ota_registered`, plus the existing transfer lines.

**`drain_stuck`.** If a flash operation does not finish within the drain timeout when OTA is
stopped, the worker task is deliberately left running and the state is `drain_stuck`
(`fwupgrade start` is refused). It recovers automatically within about 6 s after the
operation finishes. Otherwise: **reset the device: run 'reboot' (serial or BLE CLI) or press EN**.
`reboot` is accepted in this state (but never while the image is PENDING_VERIFY).

`reboot` is refused with `ERR busy cmd=reboot reason=ota_active` while an OTA transfer is in
flight and `reason=pending_verify` while the new image is still being verified. It restarts
within 1.5 s of the command otherwise. Bonds, Wi-Fi configuration and the hardware revision
persist.

During an OTA transfer the host task can stall for up to about 350 ms at transfer begin, so
`accel_xyz` timing is not specified during OTA. Flash erase/write also disables the cache, so
an I2C transaction overlapping it can time out: that is counted like any failure (3 strikes
fall back to the simulator) and self-heals within one probe interval (5 s).

## 6. Hardware revision (`hw_version`) and provisioning

The revision lives in NVS (`devinfo/hw_rev`), **nothing is compiled in**: a freshly erased
board answers `ERR unavailable cmd=hw_version`. Provision each DevKit once, **over serial
only**:

```text
esp32_net> hw_rev_set Alpha_1.0.0_HW
OK hw_rev_set=Alpha_1.0.0_HW
```

Format `[A-Za-z0-9._-]`, 1 to 32 characters. `hw_rev_set` is not reachable over BLE.
After `idf.py erase-flash` repeat the provisioning (and the "forget device" step on the phone).

## 7. Firmware version (`fw_version`) and release procedure

The version comes only from `version.txt` at the repository root (single line, LF, 1 to 31
characters from `[A-Za-z0-9._+-]`, not `1`). It is currently `1.0.0`. The configure step fails
with a clear message if the file is missing/empty/`1`/too long/has bad characters, or if
`PROJECT_VER` is set on the command line (`-DPROJECT_VER=...`) or is in the CMake cache
(an environment variable of that name is not checked). At runtime
`fw_version` reports the running image's own descriptor, so it is correct after an OTA.

Two-image recipe for OTA tests (build into separate directories):

```text
# image A (1.0.0)
idf.py -B build-a build
# image B: change version.txt to 1.0.1-test, then
idf.py -B build-b build
```

OTA A to B, wait for `FWUPG_VALIDATED`, run `fw_version` (`1.0.1-test`); repeat B to A.

## 8. ADXL345 sensor

Wiring (all PENDING hardware validation, see `docs/hw_validation_checklist.md`):

| ADXL345 | DevKit-C | Note |
| :--- | :--- | :--- |
| VCC | 3V3 | |
| GND | GND | |
| SDA | GPIO21 | internal pull-up on; add external 4.7 kOhm pull-ups for a reliable bus |
| SCL | GPIO22 | same (other pins: `menuconfig`; the build refuses GPIO 0/1/2/3/5/6-11/12/15 and input-only pins 34-39) |
| CS | VDD_IO (3V3) | selects I2C |
| SDO | GND = address 0x53, 3V3 = address 0x1D | both addresses are probed |

Attach or detach the sensor only with the board unpowered. The firmware probes every 5 s
while no sensor is active and switches from simulated to hardware data without a reflash.

Configuration: range +/-2 g, FULL_RES (3.9 mg/LSB), 100 Hz output rate, bypass FIFO, no
interrupts, measure mode. Axes are the sensor's own X, Y, Z. A board lying flat reads about
+1 g on Z (PENDING validation).

**Freshness limit:** a hardware sample older than 100 ms (`CONFIG_ACCEL_MAX_AGE_MS`) at
reporting time is reported as `ERR sensor_stale cmd=accel_xyz sample=<n>`; age exactly at the
limit is accepted. A failed hardware read is `ERR sensor_error ...`; three consecutive
failures fall back to simulated data for the following samples (labelled `source=simulated`).
Every sample is one coherent 13-byte read of registers 0x2C..0x38 that also checks that the
part still holds its configuration (BW_RATE, POWER_CTL measure set and sleep clear,
DATA_FORMAT, FIFO_CTL bypass); the sensor only becomes active 20 ms after measure mode is
set, so the first sample is a real conversion: a part
that reset or went to standby counts as a failed read (never as `source=hardware` with
zeros) and is reconfigured by the probe task. A device that ACKs at 0x53 or 0x1D but is not an
ADXL345 (wrong DEVID) is reported once as `SENSOR_FAIL`, the other address is still tried,
and probing repeats at most every 5 s.

**Simulation:** with no sensor the data is generated, labelled `source=simulated`, uniform
integer milli-g with X and Y in [-0.250, +0.250] g and Z in [+0.750, +1.250] g. The seed is
random each boot and is not reachable from any command.

Timing: 15 samples at absolute 1 s deadlines (jitter below 50 ms on the DevKit; to be
measured). Disconnecting cancels the session.

## 9. Limits and notes

* Console failure: if the console or REPL cannot start, the BLE host is not started either
  (the BLE CLI is then unavailable until reset).
* `fwupgrade start` right after the prompt may report "BLE host is still starting, retry in a
  few seconds".
* Advertising never stops, so the device stays discoverable (accepted at the requirement
  level, REQ-HW-10).
* Logging: `BLECLI_*`, `ACCEL_*`, `SENSOR_*`, `REBOOT_*`, `FWUPG_*`, `HEAP_STAT`, `STACK_HWM`
  lines on the serial log (never response contents). The resource dump after a BLE
  disconnect is emitted only for a link that had been secure and at most every 10 s.
* If NVS had to be erased at boot (corrupt or full), one warning is logged: the hardware
  revision and all BLE bonds are gone; run `hw_rev_set <rev>` and forget the device on the phone.

## 10. Physical hardware validation checklist

The authoritative list is `docs/hw_validation_checklist.md`. Summary: every ADXL345 item is
**PENDING - not validated**. No ADXL345 has been attached; the simulator and the mock-bus
unit tests do not close any of them.

## 11. Running the host unit tests

```text
cmake -S test/host -B test/host/build && cmake --build test/host/build && ctest --test-dir test/host/build --output-on-failure
```

Requires host `gcc`, `cmake` and `IDF_PATH` set (Unity is taken from ESP-IDF). The tests
verify decision logic against mocks; they are not hardware validation.
