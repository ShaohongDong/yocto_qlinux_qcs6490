# SDK host tools

`buildtools/` is a locally installed Qualcomm Linux 2.0 host-tool bundle.
It contains prebuilt executables, libraries, certificates and an x86_64
sysroot. Git tracks the source metadata and artifact identity, and ignores
this installed directory. The SDK environment still depends on these tools.

## Pinned recovery artifact

[buildtools.lock.json](buildtools.lock.json) records the SDK version, source
commit/tree, archive SHA-256, byte size and file count. The complete original
3,378-file snapshot is preserved outside the checkout at:

```text
/data/qcom-sdk-artifacts/qcom-buildtools-2.0-74abaadcac77.tar.gz
```

This archive was generated from the original Git snapshot, including files
already missing from the current installation. It is not a backup of the
incomplete working directory. A matching `.sha256` sidecar is beside it;
the checksum committed in the lock file is the authoritative identity.
No remote artifact upload has been performed. The original vendor installer
URL/checksum has not been established and is recorded as `null`.

The archived environment script retains the installation prefix
`/home/dsh/Desktop/qcom_sdk`. On this host that path points to
`/data/qcom_sdk`. This snapshot supports recovery of that installation;
use the matching vendor SDK installer for installation at a different prefix
so its relocation steps are applied.

## Recover missing local tools

From the SDK root, verify the archive and extract into a new temporary
directory. Merge only missing paths to preserve existing local tools:

```sh
set -eu
artifact=/data/qcom-sdk-artifacts/qcom-buildtools-2.0-74abaadcac77.tar.gz
printf '9cb0441f9df4a26ae9117b08d61fdca61da85b1d84a085999a2d9d97e269cf48  %s\n' "$artifact" | sha256sum --check -
recovery_dir=$(mktemp -d /data/qcom-sdk-artifacts/buildtools-restore.XXXXXX)
tar -xzf "$artifact" -C "$recovery_dir"
mkdir -p buildtools
rsync -a --ignore-existing "$recovery_dir/buildtools/" buildtools/
```

Keep the temporary directory until the SDK has been checked. In a fresh
shell at the original SDK location, run:

```sh
source ./environment-setup-armv8a-qcom-linux
devtool --help
```

The repository cleanup preserves all existing local paths and leaves their
previously missing files missing. Archive validation does not establish that
the current local SDK initializes successfully. Removing tracking also does
not remove these binaries from older Git commits.

## Recreate the archive from existing Git history

With the recorded source commit available, create a new output path outside
the checkout. Do not overwrite the preserved artifact:

```bash
set -euo pipefail
set -C
git archive --format=tar 74abaadcac77ff94e13405aa076c689189b24ecc -- buildtools \
    | gzip -n > /data/qcom-sdk-artifacts/qcom-buildtools-recreated.tar.gz
sha256sum /data/qcom-sdk-artifacts/qcom-buildtools-recreated.tar.gz
```

Compare the output checksum with the lock file before using it. The source
commit/tree identifies the contents independently of archive-tool versions.
Future SDK updates should retain the matching installer/archive in artifact
storage and update the source/version/checksum metadata in Git.
