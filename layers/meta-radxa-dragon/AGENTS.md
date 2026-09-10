# Agent Guide for meta-qcom (wrynose / LTS branch)

This file guides automation agents to run builds / checks the same way CI does:

- use **kas-container** (isolated from host),
- keep `DL_DIR` and `SSTATE_DIR` outside the repo so caches are shared,
- run `yocto-patchreview` and `oe-selftest` routinely, and run
  `yocto-check-layer` before opening/updating a PR, via the CI helper scripts.

> **This is the `wrynose` LTS branch.** It is not the primary development
> branch. Changes are expected to land on **master** first and then be
> **backported** here with `git cherry-pick -x`, unless they are specific to
> `wrynose`. See [section 6](#6-pull-request--contribution-workflow-wrynose-lts)
> for the full workflow.

## Project Overview

meta-qcom is an OpenEmbedded / Yocto Project hardware enablement layer for Qualcomm based platforms.

`wrynose` is the **LTS Stable branch**, focused on long term support and
compatibility with the most recent Yocto Project LTS release. `master` is the
primary development branch.

## 1) Prerequisites

1. `kas-container` available on PATH, or set `KAS_CONTAINER=/abs/path/to/kas-container`
   (from [kas-container](https://github.com/siemens/kas/blob/master/kas-container)).
2. Container runtime access (Docker/Podman backend used by `kas-container`).
3. Work directories outside the repository for build outputs and shared caches.

### Container runtime smoke test (required order)

Run Docker first:

```sh
docker run --rm hello-world
```

Then check Podman:

```sh
if command -v podman >/dev/null 2>&1; then
  podman run --rm hello-world
else
  echo "podman not installed; continue with Docker backend"
fi
```

Notes:

- Do not use `sudo` unless the host setup explicitly requires it.
- Do not create or modify user groups as part of this workflow.
- If Podman is unavailable, Docker-only operation is acceptable.

## 2) Recommended environment

If `KAS_WORK_DIR`, `DL_DIR`, and `SSTATE_DIR` are already set in the environment, use them
directly — do not override them. Only set defaults when they are absent:

```sh
export REPO_DIR="$(pwd)"                               # meta-qcom checkout
export KAS_WORK_DIR="${KAS_WORK_DIR:-/path/to/kas-work}"      # outside repo to avoid polling the checkout
export DL_DIR="${DL_DIR:-/path/to/shared-cache/downloads}"
export SSTATE_DIR="${SSTATE_DIR:-/path/to/shared-cache/sstate-cache}"
mkdir -p "${DL_DIR}" "${SSTATE_DIR}" "${KAS_WORK_DIR}"
```

## 3) Build with kas-container (CI style)

CI build composition pattern:
`:ci/<machine>.yml[:distro.yml][:kernel.yml]`

Example:

```sh
export KAS_YAMLS="ci/rb3gen2-core-kit.yml:ci/qcom-distro.yml"
"${KAS_CONTAINER:-kas-container}" build "${KAS_YAMLS}"
```

## 4) Run routine checks via CI helper scripts

For routine local validation, run:

```sh
ci/kas-container-shell-helper.sh ci/yocto-patchreview.sh
ci/kas-container-shell-helper.sh ci/oe-selftest.sh
```

Run `yocto-check-layer` only before opening/updating a pull request:

```sh
ci/kas-container-shell-helper.sh ci/yocto-check-layer.sh
```

### oe-selftest details

- Script: `ci/oe-selftest.sh`
- Auto-discovers tests in `lib/oeqa/selftest/cases/` when no test list is given.
- Honors `DL_DIR` and `SSTATE_DIR` from environment (recommended for shared cache).

Run a subset:

```sh
"${KAS_CONTAINER:-kas-container}" shell ci/base.yml \
  --command "/repo/ci/oe-selftest.sh /repo /work qcom_fitimage.QcomFitImageMatrixTests"
```

If passing explicit tests directly (without helper), call:

```sh
ci/oe-selftest.sh "$REPO_DIR" "$KAS_WORK_DIR" qcom_fitimage.QcomFitImageMatrixTests
```

## 5) Direct kas shell alternative (no helper wrapper)

For one-off commands:

```sh
kas-container shell --skip repos_checkout ci/rb3gen2-core-kit.yml -c "bitbake <target>"
kas-container shell --skip repos_checkout ci/rb3gen2-core-kit.yml -c "oe-selftest --run-tests qcom_fitimage"
```

Use the helper scripts for CI parity whenever possible.

## 6) Pull request / contribution workflow (wrynose LTS)

`wrynose` is the LTS branch. **Propose changes against `master` first.**
We expect every change to be backported to `wrynose` unless it is specific to
`wrynose` (e.g. it does not apply to `master`, or `master` has diverged in a way
that makes the change meaningless there).

The full backport workflow — the default `git cherry-pick -x` path from
`master`, the exception for wrynose-only changes, the CI-equivalent checks to
run before opening a PR, and the `[Backport wrynose]` commit message
conventions — is documented in [BACKPORTING.md](BACKPORTING.md).

If the change **cannot** be submitted to `master` (it is specific to
`wrynose`), then submit it directly against `wrynose`, and **explain in the
commit body and PR description why it is wrynose-only** and not a backport.
The general contribution workflow and the commit subject and message
requirements are documented in [CONTRIBUTING.md](CONTRIBUTING.md).

When committing programmatically, take the `Signed-off-by` identity from the
local git configuration and append the trailer explicitly:

```text
Signed-off-by: $(git config user.name) <$(git config user.email)>
```

Never fabricate a name or email; always read them from `git config`.

Fixups within the same patch series are not allowed; changes should be
corrected in the patch where they are introduced.
