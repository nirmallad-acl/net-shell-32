# Physical hardware validation checklist (NETSHELL32-5, REQ-DOC-02)

No ADXL345 has been attached to any DevKit so far. **Every item below is PENDING - not
validated.** Simulated output (`source=simulated`), the host unit tests (mock I2C bus, mock
clock) and QEMU do not close any item. Record the date, board, wiring and log excerpt when an
item is finally run.

| ID | Item | Requirement | Mock/UT coverage (not validation) | Status |
| :--- | :--- | :--- | :--- | :--- |
| HW-01 | DEVID (0xE5) read on a real part at 0x53 | REQ-FUNC-50 | `test_adxl345` | PENDING - not validated |
| HW-02 | Real I2C electrical operation on GPIO21/22, internal plus external pull-ups, 100 kHz | REQ-HW-03, REQ-HW-05 | none | PENDING - not validated |
| HW-03 | Both addresses 0x53 and 0x1D (SDO low/high) | REQ-HW-04 | `test_accel_select` | PENDING - not validated |
| HW-04 | `source=hardware` appears only for fresh real reads | REQ-FUNC-44 | `test_accel_select`, `test_accel_sched` | PENDING - not validated |
| HW-05 | Real read failure and stale data give per-slot `ERR sensor_error` / `ERR sensor_stale` | REQ-FUNC-45, 56 | `test_accel_sched`, `test_accel_select` | PENDING - not validated |
| HW-06 | Same syntax and structure for hardware as for simulated data | REQ-FUNC-46 | `test_format` | PENDING - not validated |
| HW-07 | Attach with power off, power on: simulation switches to hardware with the same image | REQ-FUNC-57, 59 | `test_accel_select` | PENDING - not validated |
| HW-08 | Failure and recovery with a real part (3 failures, fallback, re-probe, recovery) | REQ-FUNC-58 | `test_accel_select` | PENDING - not validated |
| HW-09 | Orientation-dependent readings (about +1 g on the axis facing up at rest), axis convention | REQ-HW-06 | none | PENDING - not validated |
| HW-10 | Wiring check (3V3, GND, CS to VDD_IO, SDO), bus stuck low detection and recovery | REQ-HW-05, 08 | `test_accel_select` | PENDING - not validated |
| HW-11 | Real-world sample age (<= 100 ms) | REQ-FUNC-56 | mock clock only | PENDING - not validated |
| HW-12 | `accel_xyz` timing with a real sensor in the loop (15 slots, 1000 +/- 50 ms) | REQ-FUNC-41, REQ-PERF-02 | simulated timing only | PENDING - not validated |
| HW-13 | `accel_xyz` during an OTA transfer (I2C timeouts while flash erase/write disables the cache; fallback and self-heal within one probe interval) | REQ-HW-08, R-09 | none | PENDING - not validated |
| HW-15 | A non-ADXL345 device at 0x53 or 0x1D (wrong DEVID), alone and together with a real ADXL345 at the other address: one `SENSOR_PROBE`/`SENSOR_FAIL` per 5 s at most, no log flood, no I2C hammering, the real part is still found; two wrong parts (one at each address) are each reported once, not on every probe cycle | REQ-FUNC-57, 58, REQ-PERF-05 | `test_accel_select` (wrong-device, probe-loop spacing) | PENDING - not validated |
| HW-16 | Sensor reset or power glitch while running (POWER_CTL back to 0, data 0): `ERR sensor_error` for 3 slots, then simulated samples, then automatic recovery within one probe interval; never `source=hardware` with zeros. Also: the first sample after (re)configuration is not all-zero (the probe task waits 20 ms after setting measure mode before the sensor becomes active); a part left in sleep mode (POWER_CTL bit 2) or in FIFO stream/trigger mode is rejected as a config error | REQ-FUNC-44, 58 | `test_adxl345`, `test_accel_select` | PENDING - not validated |
| HW-17 | After an NVS erase at boot (corrupt/full NVS): the single warning appears, `hw_version` answers `ERR unavailable` until `hw_rev_set`, the phone must forget the device | REQ-FUNC-22 | none | PENDING - not validated |
| HW-14 | NVS and heap behaviour with a real sensor attached for 24 h (no leak, no WDT) | REQ-MEM-01 | none | PENDING - not validated |
