# Dagwood Fixture & EC Add-In Card (AIC) Testing

This guide covers building and flashing the Dagwood fixture firmware and running
hardware-in-the-loop (HIL) tests on attached ChromeOS EC Add-In Cards (AICs)
inside the `ec-builder` container.

*   For general `ec-builder` setup and host builds, see [`README.md`](./README.md).
*   For provisioning and deploying to a Satlab test station, see
    [`SATLAB.md`](./SATLAB.md).

---

## Real Device Testing on Dagwood

To run tests against a Dagwood board and EC Add-In Card (AIC) connected to the
host, use `./dagwood` (in `/workspace/src/platform/ec`) or the
`run_dagwood_tests.py` helper script. It automatically handles device forwarding
and configures Twister with the required parameters (toolchain, flash command,
etc.).

Test results are written to `workspace/src/platform/ec/twister-out/`.

### Realtek (`realtek/rts5912`) Example
For Realtek boards, the container automatically builds and pre-installs the
`rtkupdate` utility and the `rts5915_flash_upload.bin` monitor binary on
startup.

To run all EC-AIC tests on `realtek/rts5912`:
```bash
./run_dagwood_tests.py -p realtek/rts5912
```

### Nuvoton (`npcx9/npcx9m7f`) Example
For Nuvoton boards, the container automatically builds and pre-installs the
`uartupdatetool` utility and the `npcx_monitor.bin` monitor binary on startup.

To run all EC-AIC tests on `npcx9/npcx9m7f`:
```bash
./run_dagwood_tests.py -p npcx9/npcx9m7f
```

### ITE (`it8xxx2/it82002aw`) Example
For ITE boards, the container pre-installs `itecomdbgr` on image build.

To run all EC-AIC tests on `it8xxx2/it82002aw`:
```bash
./run_dagwood_tests.py -p it8xxx2/it82002aw
```

### Customizing the Run
You can override the defaults using the script options:

*   **Specify one or more test directories**:
    ```bash
    ./run_dagwood_tests.py -p realtek/rts5912 -T zephyr/test/ec-aic -T zephyr/test/another-test
    ```
*   **Run one or more specific test scenarios**:
    ```bash
    ./run_dagwood_tests.py -p realtek/rts5912 -s aic.i2c -s another.scenario
    ```
*   **Specify a different serial port**:
    ```bash
    ./run_dagwood_tests.py -p realtek/rts5912 -d /dev/ttyACM0
    ```

For help on options:
```bash
./run_dagwood_tests.py -h
```

---

## Building and Flashing Dagwood Fixture Firmware

The container includes `openocd`, `dfu-util`, and the `hal_stm32` Zephyr module
to build and flash the Dagwood fixture MCU (STM32H7) firmware directly.

1.  **Inside the container**:
    ```bash
    cd /workspace/src/platform/dagwood

    # Build and flash over USB DFU (no ST-Link required once DFU-capable firmware is installed):
    ./build_from_chroot.py -f --flasher dfu

    # Build and flash via OpenOCD & ST-Link (required for initial bootstrap):
    ./build_from_chroot.py -f --flasher openocd

    # Or flash existing build without rebuilding:
    ./build_from_chroot.py --flash-only
    ```

2.  **Direct from host via `run_docker.sh`**:
    ```bash
    ./run_docker.sh bash -c "cd /workspace/src/platform/dagwood && ./build_from_chroot.py -f --flasher dfu"
    ```
