# Upstream Overrides

[TOC]

`upstream_overrides/` holds temporary patches and file replacements applied at
build time by `zephyr/zmake/zmake/repository_prep.py` (invoked automatically by
`zmake`, `twister`, `chromeos-zephyr` / `chromeos-ec` ebuilds, and
`make PREPARE_WORKSPACE=1`). During normal `zmake` and `twister` runs, changes
are applied inside a temporary shadow checkout so your working tree stays clean.

## Directory Layout

*   `patches/<checkout-relative-repo-root>/*.patch`: Unified diff files ending
    in `.patch`, applied in lexicographical filename order (e.g.,
    `0001-foo.patch`, `0002-bar.patch`, or `pr-125536.patch`).
*   `src-override/<checkout-relative-file-path>`: Full replacement files copied
    into the target checkout before patches are applied.

## Creating a Patch from a GitHub Pull Request

`repository_prep.py` applies and reverts each `.patch` file in a single
`git apply` / `git apply -R` invocation. Always download GitHub's `.diff` URL
rather than `.patch`:

*   **`.diff`** squashes multi-commit PRs into a single net diff where each
    file appears once, so `git apply` and `git apply -R` handle multi-commit
    additions, edits, and renames cleanly.
*   **`.patch`** concatenates separate commits (`git format-patch`), which
    breaks `git apply` or `git apply -R` when a multi-commit PR creates and
    later modifies or renames the same file.

Save the output with a `.patch` extension so `repository_prep.py` discovers it.
`git apply` ignores text before the first `diff --git` header, so prepending an
`Upstream-PR:` line keeps a link to the source PR inside the file.

### Upstream Zephyr RTOS (`zephyrproject-rtos/zephyr`)

ChromeOS mirrors upstream Zephyr in the `zephyr/` subdirectory of the flattened
`src/third_party/zephyrproject` git repository. Because `git apply` resolves
`diff --git` paths relative to the git repository root, place the file under
`patches/src/third_party/zephyrproject/` and prefix paths with `zephyr/`.

From `src/platform/ec/`:

```bash
PR=125539
{
  echo "Upstream-PR: https://github.com/zephyrproject-rtos/zephyr/pull/${PR}"
  echo ""
  curl -fL "https://github.com/zephyrproject-rtos/zephyr/pull/${PR}.diff" | \
    sed -E \
      -e 's#^diff --git a/(.*) b/(.*)$#diff --git a/zephyr/\1 b/zephyr/\2#' \
      -e 's#^(---|[+]{3}) ([ab])/#\1 \2/zephyr/#' \
      -e 's#^(rename|copy) (from|to) #\1 \2 zephyr/#'
} > "upstream_overrides/patches/src/third_party/zephyrproject/pr-${PR}.patch"
```

### Flattened Zephyr Modules (HALs and Libraries)

Upstream Zephyr modules mirrored under `src/third_party/zephyrproject/modules/`
(such as `zephyrproject-rtos/hal_stm32` at `modules/hal/stm32/` or `cmsis_6` at
`modules/hal/cmsis_6/`) also live in the `src/third_party/zephyrproject` git
repository and use their module subdirectory prefix:

```bash
REPO=zephyrproject-rtos/hal_stm32
SUBDIR=modules/hal/stm32
PR=123
{
  echo "Upstream-PR: https://github.com/${REPO}/pull/${PR}"
  echo ""
  curl -fL "https://github.com/${REPO}/pull/${PR}.diff" | \
    sed -E \
      -e "s#^diff --git a/(.*) b/(.*)\$#diff --git a/${SUBDIR}/\\1 b/${SUBDIR}/\\2#" \
      -e "s#^(---|[+]{3}) ([ab])/#\\1 \\2/${SUBDIR}/#" \
      -e "s#^(rename|copy) (from|to) #\\1 \\2 ${SUBDIR}/#"
} > "upstream_overrides/patches/src/third_party/zephyrproject/hal_stm32-pr-${PR}.patch"
```

### Standalone Repositories

For repositories whose ChromeOS git root matches the upstream repository root
(e.g., `src/third_party/pigweed`, `src/third_party/boringssl`,
`src/third_party/cryptoc`, `src/third_party/tpm2`, or `src/platform/ec`), no
path prefixing is needed:

```bash
REPO=owner/repo
TARGET_REPO=src/third_party/boringssl
PR=456
mkdir -p "upstream_overrides/patches/${TARGET_REPO}"
{
  echo "Upstream-PR: https://github.com/${REPO}/pull/${PR}"
  echo ""
  curl -fL "https://github.com/${REPO}/pull/${PR}.diff"
} > "upstream_overrides/patches/${TARGET_REPO}/pr-${PR}.patch"
```

## Removing Merged Overrides

Once an upstream PR merges and rolls into ChromeOS, delete its `.patch` file
from `upstream_overrides/patches/`. If the upstream roll lands first,
`repository_prep.py` detects that the reverse patch applies cleanly
(`git apply -R --check`) and skips the already-applied patch automatically.
