# Provisioning and Deploying to Satlab for Dagwood Testing

This guide provides step-by-step instructions for provisioning a new **Satlab**
test station in UFS/Swarming, deploying and persisting the `ec-builder`
container, and running automated tests on **Dagwood** fixtures with ChromeOS EC
Add-In Cards (AICs).

*   For general `ec-builder` usage, see [`README.md`](./README.md).
*   For running Dagwood & EC-AIC tests, see [`DAGWOOD.md`](./DAGWOOD.md).

> **Note**: In the commands below, replace `mysatlab` with the real Satlab
> hostname or IP address and `<SATLAB_ID>` with the lowercase host identifier
> returned by `/usr/local/bin/get_host_identifier` (e.g. `0wgatfqi22088091`).

---

## 1. Architecture & Hardware Topology

*   **Satlab Host**: Chromebox running ChromeOS, Docker, the Swarming `drone`
    agent, and `satlab_remote_access`.
*   **Dagwood Fixture**: STM32H7 USB device (`18d1:5214`) providing MECC
    200-pin slot power control, I2C muxing/emulation, power-sequencing
    emulation, and UART console/programming interfaces.
*   **EC-AIC**: The Add-In Card under test (`it8xxx2/it82002aw`,
    `realtek/rts5912`, `npcx9/npcx9m7f`).
*   **`ec-builder` Container**: Standalone container on the Satlab host Docker
    daemon (`192.168.231.1:2376`) containing `zmake`, Zephyr Twister, Coreboot
    SDK toolchains, and host flasher utilities (`itecomdbgr`, `rtkupdate`,
    `uartupdatetool`, `dfu-util`, `openocd`).

When connecting via SSH (`ssh mysatlab`), the session logs in as user `moblab`
inside an isolated `remote-access` container. The host's Docker daemon socket
(`/var/run/docker.sock`) is mounted directly inside the container with
read/write permissions (`0666`), allowing the `docker` CLI inside the SSH
session to interact directly with the host's Docker daemon.

### Serial Interfaces (`/dev/ttyACM*`)

When a single Dagwood fixture is connected to the Satlab Chromebox over USB, it
enumerates three CDC-ACM serial ports:

| Node | Name | Speed | Role |
| :--- | :--- | :--- | :--- |
| `/dev/ttyACM0` | **Dagwood Shell** | 115200 8N1 | Controls MECC slot power (`ec power on/off`), I2C muxes, and Dagwood test emulators. |
| `/dev/ttyACM1` | **EC Console** | 115200 8N1 | Passthrough UART console for the EC Add-In Card (used by Twister `--device-serial`). |
| `/dev/ttyACM2` | **EC Programmer** | Platform | Flashing UART port for AIC bootloaders (`npcx_boot`, `rtkupdate`). |

---

## 2. Registering the Satlab & Dagwood in UFS / Swarming

Because the Dagwood fixture is an EC testbed attached over USB rather than a
full Chromebook DUT with a Servo V4, use `/usr/local/bin/shivas` inside the
Satlab SSH session to register the asset and DUT in UFS with `-board dagwood`
and `-model dagwood`.

### Step 1: Discover the `SATLAB_ID`
SSH to the Satlab host:
```bash
ssh mysatlab
```
Run:
```bash
/usr/local/bin/get_host_identifier
```
*Example output:* `0WGATFQI22088091` (use the lowercase form `0wgatfqi22088091`
for `<SATLAB_ID>` in rack, host, and pool names below).

### Step 2: Register the Asset in UFS
Create the underlying asset entity in UFS using the Satlab service account.
Asset names in UFS (`-namespace os`) are globally unique and bound 1-to-1 to a
single DUT, so include `<SATLAB_ID>` in the asset name (e.g.
`dagwood-<SATLAB_ID>-host1`) to prevent collisions across Satlab stations:

```bash
/usr/local/bin/shivas add asset \
  -namespace os \
  -service-account-json /home/satlab/keys/pubsub-key-do-not-delete.json \
  -name dagwood-<SATLAB_ID>-host1 \
  -board dagwood \
  -model dagwood \
  -type dut \
  -zone satlab \
  -rack satlab-<SATLAB_ID>-rack
```

*(If updating an existing asset's board or model to `dagwood`, run
`/usr/local/bin/shivas update asset -namespace os -service-account-json
/home/satlab/keys/pubsub-key-do-not-delete.json -name <ASSET_NAME> -board
dagwood -model dagwood`.)*

### Step 3: Register the DUT & Assign the `ec-dagwood` Pool
Create the DUT endpoint and assign it to the `ec-dagwood` pool:

```bash
/usr/local/bin/shivas add dut \
  -namespace os \
  -service-account-json /home/satlab/keys/pubsub-key-do-not-delete.json \
  -name satlab-<SATLAB_ID>-host1 \
  -board dagwood \
  -model dagwood \
  -asset dagwood-<SATLAB_ID>-host1 \
  -pools ec-dagwood,satlab-<SATLAB_ID>
```

### Step 4: Set the DUT State to `ready`
New Satlab DUTs default to `dut_state: needs_deploy`. Set the state to `ready`:

```bash
/usr/local/bin/shivas update dut-state \
  -namespace os \
  -service-account-json /home/satlab/keys/pubsub-key-do-not-delete.json \
  -dut-name=satlab-<SATLAB_ID>-host1 \
  -dut-state=ready
```

### Step 5: Verify the Bot in Swarming
Satlab's `drone` container synchronizes with UFS within 2–5 minutes and launches
a Swarming bot named `crossk-satlab-<SATLAB_ID>-host1`. Verify its status on the
ChromeOS Swarming dashboard:

*   **Bot Page**:
    `https://chromeos-swarming.appspot.com/bot?id=crossk-satlab-<SATLAB_ID>-host1`
*   **Pool Bot List (`label-pool:ec-dagwood`)**:
    `https://chromeos-swarming.appspot.com/botlist?f=pool%3AChromeOSSkylab&f=label-pool%3Aec-dagwood`

---

## 3. Deploying `ec-builder` to Satlab

### Automated Deployment Helper (Recommended)

Use `deploy_satlab.py` from your workstation to package, transfer, load, and
configure reboot persistence automatically:

```bash
# Deploy local image to remote Satlab station:
./deploy_satlab.py mysatlab

# Or pull the released image directly from Artifact Registry on Satlab:
./deploy_satlab.py --pull mysatlab
```

### Manual Deployment Procedure

#### 1. Save and Compress the Image Locally
From your local workstation:
```bash
docker save ec-builder:latest | gzip > /tmp/ec-builder_image.tar.gz
```

#### 2. Transfer to Satlab
Copy the tarball to Satlab's `/tmp`:
```bash
scp /tmp/ec-builder_image.tar.gz mysatlab:/tmp/ec-builder_image.tar.gz
```

#### 3. Load Image into Docker on Satlab
Load the image into the Docker daemon:
```bash
ssh mysatlab 'PATH=/usr/local/bin:$PATH docker load -i /tmp/ec-builder_image.tar.gz'
```

#### 4. Configure Reboot Persistence (Critical)

> **Why this step is required**:
> On Satlab boot, the startup script (`/compose_startup.sh`) executes:
> ```bash
> docker system prune --filter "label!=skip.while.pruning.docker.system=yes" -f
> docker image prune -a -f
> ```
> `docker image prune -a -f` deletes **all unused images** (any image without an
> associated running or stopped container). Because `ec-builder` is typically
> run interactively with `--rm`, no container exists on reboot, causing the
> image to be deleted during boot up.

To protect `ec-builder` from being pruned across reboots, create a permanent
sentinel container bearing Satlab's prune-exemption label:

```bash
ssh mysatlab '
export PATH=/usr/local/bin:$PATH
docker rm -f ec-builder-keepalive 2>/dev/null || true
docker create \
  --name ec-builder-keepalive \
  --label "skip.while.pruning.docker.system=yes" \
  ec-builder:latest
'
```

#### 5. Verify the New Image and Sentinel Container
```bash
ssh mysatlab '
export PATH=/usr/local/bin:$PATH
docker images | grep ec-builder
docker ps -a --filter "name=ec-builder-keepalive"
'
```

---

## 4. Automated CI Publishing and Distribution (Artifact Registry & Watchtower)

For fleet-wide deployment across Satlab bots without manual `docker load` steps:

1. **Publish to Google Artifact Registry**:
   Use `publish_image.py` (or CI CloudBuild/Kokoro) to build and push to the
   partner Satlab registry:
   ```bash
   ./publish_image.py --build
   # Pushes to:
   #   us-docker.pkg.dev/chromeos-partner-moblab/satlab/ec-builder:release
   ```

2. **Satlab Auto-Updates via Watchtower**:
   On Satlab, Watchtower periodically scans registered containers for image
   updates from Artifact Registry (`WATCHTOWER_POLL_INTERVAL=3600`).
   Once `ec-builder` is launched from the remote registry:
   ```bash
   REGISTRY_IMG="us-docker.pkg.dev/chromeos-partner-moblab/satlab/ec-builder:release"
   docker pull "${REGISTRY_IMG}"
   docker tag "${REGISTRY_IMG}" ec-builder:latest
   docker create \
     --name ec-builder-keepalive \
     --label "skip.while.pruning.docker.system=yes" \
     "${REGISTRY_IMG}"
   ```
   Watchtower will automatically detect new releases pushed to the registry,
   download the updated layer blobs, and keep the image up to date.

---

## 5. Initial Dagwood DFU Firmware Bootstrap

The `test_dagwood_firmware` LUCI recipe automatically checks and updates the
Dagwood fixture firmware over USB DFU (`./build_from_chroot.py -f --flasher dfu`)
before each test run without needing an ST-Link attached.

However, a brand-new Dagwood board (or one running older firmware prior to
software-triggered DFU support) must be flashed **once** using an ST-Link and
OpenOCD (`--flasher openocd`) so it can enter STM32 USB DFU mode (`0483:df11`)
from software on subsequent runs:

```bash
ssh mysatlab
docker run -it --rm \
  --privileged \
  --net=host \
  -v /usr/local/ec-workspace:/workspace \
  -v /dev:/dev \
  -v /run/udev:/run/udev:ro \
  ec-builder:latest \
  --update \
  bash -c "cd /workspace/src/platform/dagwood && ./build_from_chroot.py -f --flasher openocd"
```

---

## 6. Running & Scheduling Tests on Satlab

### Interactive Container Execution on Satlab

1. **Connect to Satlab**:
   ```bash
   ssh mysatlab
   ```

2. **Launch `ec-builder`**: Include docker run options to include a bindmount
to the satlab stateful partition and the options needed to access the Dagwood
TTY and USB endpoints. Because the `remote-access` SSH container does not
expose `/dev/ttyACM*` in its own `/dev`, query `/sys/class/tty/ttyACM*` to
dynamically bind-mount only the `/dev/ttyACM` devices currently present on the
Satlab host:
   ```bash
   docker run -it --rm \
     --privileged \
     -v /usr/local/ec-workspace:/workspace \
     -v /dev/bus/usb:/dev/bus/usb \
     $(for d in /sys/class/tty/ttyACM*; do [ -e "$d" ] && echo "-v /dev/${d##*/}:/dev/${d##*/}"; done) \
     ec-builder:latest \
     --update \
     bash
   ```

3. **Verify endpoints and execute tests**: Inside the running container:
   ```bash
   # Verify Dagwood TTY nodes are present
   ls -la /dev/ttyACM*

    # Run all the Dagwood tests on the connected EC (e.g. ITE or Realtek)
    cd /workspace/src/platform/ec
    ./dagwood -p it8xxx2/it82002aw --sram
    # or: ./dagwood -p realtek/rts5912 --sram
    ```

### Triggering and Monitoring LUCI Builds (`ec-dagwood-nightly`)

*   **LUCI Builder Page**:
    `https://ci.chromium.org/ui/p/chromeos/builders/test_runner/ec-dagwood-nightly`
*   **Testhaus Results (`board:dagwood`)**:
    `https://cros-test-analytics.appspot.com/p/chromeos/search?rowAxes=suite&rowAxes=test&columnAxes=model&filters=board%3Adagwood&exclude_acts=false&exclude_non_prod_builders=false&exclude_non_release=false&exclude_non_account_results=false`
    *(If searching manually in the Testhaus UI, open **Options** and clear
    **Exclude ACTS**, **Exclude non-prod builders**, **Exclude non-release
    builds**, and **Exclude non-account results**.)*
*   **Manually Trigger a Build via `bb` CLI**:
    ```bash
    bb add chromeos/test_runner/ec-dagwood-nightly
    ```
*   **Lease the Bot via `crosfleet` for Interactive Debugging**:
    To prevent scheduled LUCI builds from running on the bot while debugging
    over SSH:
    ```bash
    crosfleet dut lease -host satlab-<SATLAB_ID>-host1 -minutes 60
    ```

---

## 7. Troubleshooting & Recovery

| Issue | Cause | Fix |
| :--- | :--- | :--- |
| `Entity not found Name:dagwood-<SATLAB_ID>-host1` | `shivas add dut` run before `shivas add asset`. | Run `shivas add asset` first with `-type dut -zone satlab`. |
| `Timed out waiting for Dagwood to enter DFU mode (0483:df11)` | Missing `--net=host` / `-v /run/udev:/run/udev:ro` on container, or Dagwood is running pre-DFU firmware. | Pass `--net=host -v /dev:/dev -v /run/udev:/run/udev:ro` to `docker run`; if firmware is older than DFU support, flash once via `--flasher openocd`. |
| `Permission denied /dev/ttyACM*` | Container missing device cgroup permissions. | Pass `--privileged` or `--device-cgroup-rule="c *:* rmw"` with `-v /dev:/dev`. |
| `0 devices found on i2c4` | MECC slot power is off or mux unconfigured. | On `/dev/ttyACM0`: run `ec power on` and `i2c_mux i2c-mux-a 0`. |
