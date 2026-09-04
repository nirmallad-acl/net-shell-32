# Device Verification Report — NETSHELL32-1: Serial Network CLI
## Phase 6 / GATE 4 — Log Anomaly Check

| Field | Value |
|---|---|
| Work item | `NETSHELL32-1-serial-network-cli` |
| Reviewer | fw-qa-engineer |
| Source log | `console_output.log` (repo path: `.claude/workitem/NETSHELL32-1-serial-network-cli/console_output.log`) |
| Deployment timestamp (device-agent) | `2026-09-04T13:41:55.130292Z` |
| Log header timestamp | `2026-09-04T13:41:55.130292Z` — **matches** |

---

## 1. Boot Verification

| Check | Result |
|---|---|
| `[BOOT_CAPTURE_START]` timestamp | `2026-09-04T13:41:55.130292Z` — identical to deployment timestamp |
| `[PROMPT_DETECTED_AT_LINE_43]` timestamp | `2026-09-04T13:41:56.433990Z` |
| Time-to-prompt | **1.303698 s** — matches device-agent's "~1.3 s" claim |
| `[PROMPT_FOUND]` | `YES` |
| Wi-Fi driver init sequence (lines 15–39) | Completes cleanly: driver task spawn → firmware/cert version → NVS/buffer config → `phy_init` → `mode: sta (f0:08:d1:d8:37:74)` → `enable tsf`. No error/warn lines other than one benign `W (503) spi_flash:` size-mismatch notice (image header declares 2048K vs. 4096K physical flash — a build-config note, not a runtime fault, and outside this feature's scope) |
| Reset/reboot count during capture | **0** — single continuous boot-to-command sequence, no watchdog or brownout markers, no repeated `heap_init`/`cpu_start` sequences |
| Boot banner (degraded-boot warning) | **Absent** — per tech_spec §8.3, a silent gap between last init log line and the prompt is the *healthy*-boot signature. Confirmed: no `WARNING: degraded boot` block present, consistent with all Wi-Fi-gated commands (`wifiscan`, `wifi_connect`, `ping`) executing normally with no `<cmd>: Wi-Fi subsystem unavailable` gate message |

**Version/target alignment — partial verification, flagged as a capture gap (non-blocking):** the capture's first line (`t: At 3FFE4350 len 0001BCB0 (111 KiB): D/IRAM`) is a truncated `I (xxx) heap_init: At ...` line, indicating the serial capture window opened **after** streaming had already begun — the earliest boot lines (ROM bootloader banner, chip-revision line, and the `I (xxx) cpu_start: ESP-IDF vX.X.X` banner that normally states the exact IDF version) were **not captured**. I cannot independently confirm "ESP-IDF v6.0.2" or an explicit "esp32" target string from this log. Circumstantial evidence is consistent with ESP32 (classic IRAM address range `0x3FFE...`/`0x4009...`, single-core `CPU0` main_task, station MAC `f0:08:d1:d8:37:74`) and nothing in the log contradicts the expected target/toolchain. **This does not block GATE 4** (no anomaly, no mismatch evidence) but I recommend the device-agent capture from cold power-on/reset in future runs so the IDF-version banner line is included for a complete version check.

---

## 2. Per-Command Result Table

| # | Command | Log line(s) | Contract checked (test_plan §3 / tech_spec §7) | Result |
|---|---|---|---|---|
| 1 | `help` | 62–84 | General command-summary listing (no byte-exact contract specified) | Lists all 5 registered commands (`help`, `ifconfig`, `wifiscan`, `wifi_connect`, `ping`) with argtable-style usage lines and descriptions. No crash. **PASS** |
| 2 | `ifconfig` | 87–95 | §3.1 Disconnected 6-line contract | Byte-exact match: `Interface: WIFI_STA_DEF` / `IP address: 0.0.0.0` / `Subnet mask: 0.0.0.0` / `Gateway: 0.0.0.0` / `MAC address: f0:08:d1:d8:37:74` (matches boot-reported STA MAC, `NET_CLI_MACSTR` pattern) / `Status: Disconnected`. **PASS** |
| 3 | `wifiscan` | 97–122 | §3.2 header/table/footer contract | Header verified **byte-exact** against the `printf("%-32s  %5s  %3s\n", ...)` format (44-char computed string — see §3 below for a tech_spec.md documentation note). Dash rule line byte-exact. 20 rows printed (correct clamp), truncation notice `Note: 28 APs found, showing the strongest 20 (limit 20).` matches the `Note: %u APs found, showing the strongest %u (limit %d).` shape exactly, followed by `28 network(s) found.` matching `%u network(s) found.`. **PASS** |
| 4 | `wifi_connect -s __netshell_test_nonexistent__ -p wrongpass` | 125–131 | §3.3 Progress line + Failure-outcome regex | `Connecting to "__netshell_test_nonexistent__"...` printed first (matches contract exactly). Outcome line `Failed (reason 201: WIFI_REASON_NO_AP_FOUND)` matches `^Failed \(reason ` regex — correct classification for a nonexistent SSID. Followed by `Command returned non-zero error code: 0x1 (ERROR)` (see anomaly-scan allowlist reasoning, §4 below). **PASS** (deliberately-triggered failure path behaved exactly as spec'd) |
| 5 | `ping -h 8.8.8.8` (disconnected) | 133–137 | §3.4 / §4.4 TC-EDGE-04 fast-fail contract | Output `ping: no network connection (station has no IP address)` is a **byte-exact** match to the FR-50 contract. No ICMP packets sent, immediate return to prompt (well under the 8–21 s bound). Followed by `Command returned non-zero error code: 0x1 (ERROR)` (allowlisted, see §4). **PASS** |
| 6 | `bogus_command_xyz` | 139–142 | §3/TC-UNK-01 unrecognized-command class message | `Unrecognized command` printed, REPL does not crash/hang, prompt reappears. **PASS** |

All six commands return cleanly to the `esp32_net> ` prompt (byte-exact per §3.6, verified in the raw log — `esp32_net> ` with exactly one trailing space) with no interleaving, no hang, and no reset between any of them.

---

## 3. Discrepancy Notes (non-blocking)

1. **`tech_spec.md` §7.2's illustrative `wifiscan` header sample is one space short of its own stated printf format.** The markdown sample at `tech_spec.md:1336` (`SSID` + spacing + `RSSI   CH`) is 43 characters; the actual `printf("%-32s  %5s  %3s\n", "SSID", "RSSI", "CH")` format computes to a 44-character string. The **device's actual output (log line 105, 44 chars) matches the printf-format computation exactly**, not the shorter markdown illustration. This is a spec-documentation typo, not a firmware defect — the firmware is correctly implementing the stated format string. Recommend a one-line fix to `tech_spec.md`'s illustrative example for future consistency, but this does not affect GATE 4 disposition.
2. No other discrepancies found between captured output and the test_plan.md §3 exact-output contracts for the six commands exercised.

---

## 4. Anomaly-Token Scan (Stage B)

Case-insensitive scan of `console_output.log` for: `panic`, `fatal`, `error`, `assert`, `crash`, `segfault`, `oom`, `WDT Reset` — full file (entire capture is at/after the deployment timestamp; there is no pre-deployment content in this log).

| Token | Hits | Lines | Disposition |
|---|---|---|---|
| `panic` | 0 | — | Clean |
| `fatal` | 0 | — | Clean |
| `error` | 2 | 130, 136 | **Allowlisted** — see below |
| `assert` | 0 | — | Clean |
| `crash` | 0 | — | Clean |
| `segfault` | 0 | — | Clean |
| `oom` | 0 | — | Clean |
| `WDT Reset` | 0 | — | Clean |

### Allowlist reasoning for the two `error` hits

Both hits are the identical literal line `Command returned non-zero error code: 0x1 (ERROR)`, appearing at:
- **Line 130** — immediately after the `wifi_connect` bad-SSID test case's `Failed (reason 201: WIFI_REASON_NO_AP_FOUND)` outcome line.
- **Line 136** — immediately after the `ping` disconnected test case's `ping: no network connection (station has no IP address)` message.

Classification: **documented, legitimate occurrence — not an anomaly.**
- Both source commands are *deliberately-triggered failure-path test cases* (bad SSID by design; ping issued while intentionally disconnected), and both handlers returned exactly the spec-mandated failure output (§7.3.4 Failure regex `^Failed \(reason `, and the byte-exact FR-50 string) before returning a non-zero status — this is correct, spec-compliant behavior, not a fault.
- The `Command returned non-zero error code: 0x1 (ERROR)` line itself is generated by the ESP-IDF console-component's generic command-dispatch wrapper (printed for any handler that returns non-zero), not by this feature's own application code — the same class of "library/framework-owned diagnostic text not under this feature's control" that `test_plan.md` §5 already flagged pre-emptively for argtable's `arg_print_errors()` output. This report extends that same allowlist reasoning to this specific observed, literal line, per the test_plan's own recommended mitigation ("capture one sample... add the observed literal output lines to Phase 6's anomaly-scan allowlist").
- Both occurrences are immediately followed by a clean return to the `esp32_net> ` prompt — no hang, no reset, no cascading fault.
- No other `error`-token occurrence exists anywhere in the log (confirmed via `grep -in error console_output.log`, 2 hits total, both accounted for above).

**Verdict: no un-allowlisted prohibited token appears anywhere in the log.**

---

## 5. Version / Service Health Checks

| Check | Result |
|---|---|
| ESP-IDF version banner | Not present in this capture window (capture opened mid-stream, after the version banner would have printed) — **not verifiable from this log**, flagged as a capture-completeness gap for future runs, not a failure |
| Target chip | Not explicitly printed; circumstantial evidence (IRAM address ranges, single-core `main_task` on CPU0, standard ESP32 Wi-Fi driver behavior) is consistent with ESP32 and contradicts nothing |
| Heap exhaustion | No explicit free-heap sample was captured in this manual 6-command exercise (this was not the N=100 soak run defined in test_plan.md §2). No heap-related error/warning strings present. No basis to suspect exhaustion from this capture alone; the dedicated NFR-5 N=100 soak (test_plan.md §2) remains the authoritative heap-drift check and was not exercised in this log |
| Repeated resets | None — single unbroken boot-to-command session |
| Wi-Fi driver init | Clean, no error lines during `wifi_init`/`phy_init`/mode-set sequence |
| MAC-address consistency | STA MAC `f0:08:d1:d8:37:74` reported identically in the boot log (line 38) and in `ifconfig` output (line 93) — consistent |

---

## 6. GATE 4 Verdict

# **PASS**

**Justification:**
1. Stage A (milestone/contract matching): all six manually-exercised commands (`help`, `ifconfig`, `wifiscan`, `wifi_connect` against a fake SSID, `ping -h 8.8.8.8` while disconnected, unknown command) produced output matching their test_plan.md §3 / tech_spec.md §7 exact-output contracts, including byte-exact matches on `ifconfig`'s six-line contract, `wifiscan`'s header/dash-rule/truncation-notice/footer, `wifi_connect`'s `Connecting to "..."...` progress line and `^Failed \(reason ` outcome regex, and `ping`'s exact FR-50 disconnected-gate string. One documentation-only discrepancy was found (tech_spec.md's own illustrative wifiscan header sample is 1 character short of its stated printf format) — this is a spec-typo, not a device defect, and does not block the gate.
2. Stage B (anomaly scan): the only prohibited-token hits are two occurrences of `error`, both the identical `Command returned non-zero error code: 0x1 (ERROR)` line generated by the ESP-IDF console framework's generic non-zero-return wrapper, both immediately following deliberately-triggered, spec-compliant failure-path test cases. **Both are allowlisted** under the same reasoning test_plan.md §5 pre-established for argtable's own diagnostic text. **No un-allowlisted occurrence of `panic`, `fatal`, `error`, `assert`, `crash`, `segfault`, `oom`, or `WDT Reset` exists anywhere in the log.**
3. Boot reached the prompt in 1.30 s, with zero resets, a clean Wi-Fi init sequence, and no degraded-boot banner.

**Non-blocking follow-ups recommended to the Manager:**
- Fix the 1-character spacing typo in `tech_spec.md`'s illustrative `wifiscan` header sample (line ~1336) to match its own printf format string.
- For future device-agent captures, start the serial capture before/at power-on-reset so the ESP-IDF version banner is included and full version/target alignment can be positively confirmed rather than inferred circumstantially.
- The dedicated NFR-5 N=100 heap-soak (test_plan.md §2) has not yet been exercised against hardware and remains outstanding for full GATE 4 closure on that requirement; this manual 6-command exercise only covers the functional/contract and anomaly-scan portions of GATE 4.

---

## 7. Addendum — Real-Network Connectivity Test (post-report)

After this report was drafted, the device agent was run again against the same flashed binary
(same deployment, no re-flash) to exercise `wifi_connect`/`ping` against a real access point with
valid credentials, appended to the same `console_output.log` under a
`CONNECTIVITY TEST WITH REAL NETWORK` marker (`2026-09-04T13:45:50Z`–`13:46:20Z`).

**Note on evidence handling:** the console's command-echo captured the real Wi-Fi password in
plaintext (normal REPL echo behavior, not a firmware defect — `NFR-24`'s "no password in
success/status messages" requirement is independently upheld: the `Connected. IP: ...` line itself
never repeats it). The password has been redacted (`***REDACTED***`) in this evidence copy's
`console_output.log`; the original capture is retained only in the gitignored workitem directory.

**Results:**

| Command | Result |
|---|---|
| `wifi_connect -s AVT2233 -p <redacted>` | `Connected. IP: 10.201.174.76` |
| `ifconfig` | `Status: Connected`, IP/subnet/gateway/MAC all populated correctly |
| `ping -h 8.8.8.8` | 4/4 packets received, 0% loss |
| `ping -h google.com` | Resolved via DNS fallback (OQ-9) to `142.250.146.139`, 4/4 packets received, 0% loss |

**Re-run anomaly scan on the full, extended log** (case-insensitive `panic\|fatal\|error\|assert\|crash\|segfault\|oom\|WDT Reset`): still exactly **2 hits**, both the same pre-allowlisted `Command returned non-zero error code: 0x1 (ERROR)` line from the two originally-analyzed failure-path tests. **Zero new anomaly-token hits** in the connectivity-test section.

**GATE 4 verdict: PASS, unchanged.** This addendum additionally demonstrates the full happy path end-to-end (real association, DHCP lease, `ping` over both a literal IP and a DNS-resolved hostname) on physical hardware, closing out functional confidence beyond the original disconnected-state-only exercise.

---

## 8. Addendum 2 — 32-byte SSID Boundary Verification (real AP)

Follow-up hardware test, triggered by a manual review question: does an exactly-32-byte SSID
overflow or truncate `wifi_config_t.sta.ssid` (`uint8_t ssid[32]`, no spare byte for a NUL
terminator — verified against the installed v6.0.2 `esp_wifi_types_generic.h`)? A real AP was
configured with SSID `AVT12345678901234567890123456789` (independently verified as exactly 32
bytes) and password `Acl1#2026` (redacted below).

**`wifi_connect` boundary-accept test:**

| Check | Result |
|---|---|
| "SSID too long" rejection | Did **not** fire — confirms `FR-40`'s `> 32` bound is exclusive |
| `Connecting to "..."` line | `Connecting to "AVT12345678901234567890123456789"...` — full 32 characters intact, no truncation |
| Connect outcome | `Connected. IP: 10.201.174.76` — a **real** successful association, proving the full 32-byte SSID (not just its length) survived the `memcpy` into `wifi_config.sta.ssid` and was correctly used by the Wi-Fi driver to match the real AP |
| `ifconfig` | `Status: Connected`, IP `10.201.174.76` |

**`wifiscan` display test** (same AP, now in range): the scan row for this SSID —
independently re-verified byte-exact against the actual `printf("%-32.32s  %5d  %3u\n", ...)`
format string used in `cmd_wifiscan.c` (not just trusted from the sub-agent's prose summary):

```
AVT12345678901234567890123456789    -51    1
```

32-character SSID with zero padding (exact field fill, as expected for a 32-byte value in a
`%-32.32s` field), followed by exactly 4 spaces (2 literal + 2 right-justify padding), `-51`
right-aligned in the RSSI field, 4 spaces, `1` right-aligned in the CH field — computed and
confirmed to match the format string's arithmetic exactly. All 20 displayed rows were scanned;
no other row showed truncation, garbling, or spacing anomalies.

**Conclusion:** the exact-32-byte SSID boundary neither overflows nor truncates in either the
`wifi_connect` credential-copy path or the `wifiscan` display path. The absence of a NUL
terminator in this boundary case is confirmed benign and intentional (§7.3.2, `DV-2`; not yet
given its own formal deviation entry as `DV-13` — recommended as a documentation follow-up, no
functional change needed). `test_plan.md`'s `TC-CON-06` (SSID boundary) and its `wifiscan`
counterpart are now hardware-confirmed, not just spec-asserted.

**Evidence-handling note:** the real AP's password was again captured in plaintext by the
console's command-echo (same benign REPL-echo behavior as Addendum 1, not a firmware defect).
Redacted (`***REDACTED***`) in this evidence copy's `console_output.log`; the unredacted original
remains only in the gitignored workitem directory.
