# Deploying `ec-builder` to Satlab

When changes are made to the `ec-builder` Dockerfile or dependencies locally,
use the following procedure to package and deploy the updated image to a Satlab
test station.

*   For general `ec-builder` usage, see [`README.md`](./README.md).
*   For running Dagwood & EC-AIC tests, see [`DAGWOOD.md`](./DAGWOOD.md).

> **Note**: In the commands below, replace `mysatlab` with the real Satlab
> hostname found in the **`ec-dagwood`** swarming pool (e.g. from the ChromeOS
> Swarming bot list for `pool:ec-dagwood` or UFS inventory).

---

## Architecture Overview

When connecting via SSH (`ssh mysatlab`), the session logs in as user `moblab`
inside an isolated `remote-access` container. The host's Docker daemon socket
(`/var/run/docker.sock`) is mounted directly inside the container with
read/write permissions (0666). This allows the standard Linux `docker` CLI
inside the session to interact directly with the host's Docker daemon without
requiring root SSH access to the base host OS.

---

## Automated Deployment Helper (Recommended)

Use `deploy_satlab.py` to package, transfer, load, and configure persistence
automatically:

```bash
# Deploy local image to remote Satlab station:
./deploy_satlab.py mysatlab

# Or pull the released image directly from Artifact Registry on Satlab:
./deploy_satlab.py --pull mysatlab
```

---

## Manual Deployment Procedure

### 1. Save and Compress the Image Locally
From your local workstation:
```bash
docker save ec-builder:latest | gzip > /tmp/ec-builder_image.tar.gz
```

### 2. Transfer to Satlab
Copy the tarball to Satlab's `/tmp`:
```bash
scp /tmp/ec-builder_image.tar.gz mysatlab:/tmp/ec-builder_image.tar.gz
```

### 3. Load Image into Docker on Satlab
Load the image into the Docker daemon:
```bash
ssh mysatlab 'PATH=/usr/local/bin:$PATH docker load -i /tmp/ec-builder_image.tar.gz'
```

### 4. Configure Reboot Persistence (Critical)

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

### 5. Verify the New Image and Sentinel Container
```bash
ssh mysatlab '
export PATH=/usr/local/bin:$PATH
docker images | grep ec-builder
docker ps -a --filter "name=ec-builder-keepalive"
'
```

---

### Automated CI Publishing and Distribution (Artifact Registry & Watchtower)

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

## Running Tests on Satlab

Once the image is deployed, connect to the Satlab unit and launch the container
with `--privileged` access to interact with the Dagwood hardware:

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
   ./dagwood -p it8xxx2/it82002aw
   # or: ./dagwood -p realtek/rts5912
   ```
