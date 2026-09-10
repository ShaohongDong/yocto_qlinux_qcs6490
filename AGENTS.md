# Repository Guidelines

## Project Structure & Module Organization

This checkout is a Qualcomm Yocto extensible SDK. `conf/` selects the active
layers, distro, and machine (`radxa-dragon-q6a` by default). Source metadata is
under `layers/`; board work belongs mainly in `layers/meta-radxa-dragon/`, with
machine files in `conf/machine/`, recipes in `recipes-*`, CI definitions in
`ci/`, self-tests in `lib/oeqa/selftest/cases/`, and offline validators in
`scripts/`. Read that layer's own `AGENTS.md` before editing it. `workspace/` is
managed by `devtool`. Treat `tmp/`, `cache/`, `downloads/`, `sstate-cache/`, and
BitBake daemon files as generated state, not source.

## Build, Test, and Development Commands

Start from a fresh shell at the repository root:

```sh
source ./environment-setup-armv8a-qcom-linux
devtool build-image qcom-minimal-efi-sd-image
layers/oe-core/bitbake/bin/bitbake radxa-dragon-q6a-flash-bundle
```

The first command activates the eSDK; the next two build an image and an
auditable board bundle. Artifacts appear under
`tmp/deploy/images/<machine>/`. Use `devtool modify <recipe>` for recipe source
work and `devtool reset <recipe>` only after preserving wanted changes.

For CI-equivalent layer checks, enter `layers/meta-radxa-dragon/` and run
`ci/kas-container-shell-helper.sh ci/yocto-patchreview.sh` and
`ci/kas-container-shell-helper.sh ci/oe-selftest.sh`. Run the heavier
`ci/yocto-check-layer.sh` wrapper before a pull request.

## Coding Style & Naming Conventions

Follow surrounding OpenEmbedded style. Name recipes `<component>_<version>.bb`,
appends `<component>_%.bbappend`, and machine/CI files with lowercase kebab-case.
Use four spaces in Python, `snake_case` functions, `test_*` methods, and SPDX
headers. Keep BitBake variable names uppercase and align multiline assignments
with nearby recipes. Prefer POSIX shell unless Bash features are required.

## Testing Guidelines

There is no repository-wide coverage threshold. Match validation to the change:
parse/build affected recipes, run relevant `oe-selftest` cases (for example
`qcom_fitimage.QcomFitImageMatrixTests`), and run the matching
`scripts/audit-radxa-dragon-*.py` against completed deploy artifacts. Report
offline checks separately from physical-board boot or peripheral testing.

## Commit & Pull Request Guidelines

History currently contains a signed-off initial commit; layer policy is more
specific. Make each commit atomic, use `<component>: <imperative summary>`, and
add `Signed-off-by` with `git commit -s`. PRs should explain the problem and
solution, identify affected machines/recipes, link issues, and list exact test
commands and results. Include hardware logs when claiming board behavior; do
not flash media as part of routine validation.
