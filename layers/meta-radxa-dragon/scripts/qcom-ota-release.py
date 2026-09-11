#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Create an explicitly signed, self-hostable OSTree repository generation."""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

module_path = Path(__file__).resolve().parents[1] / 'recipes-support/qcom-ota/files/qcom_ota.py'
spec = importlib.util.spec_from_file_location('qcom_ota', module_path)
ota = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ota)


def publish(source, manifest, output, key, homedir, previous=None):
    source, output = Path(source).resolve(), Path(output).resolve()
    release = ota.read_json(manifest)
    checksum = ota.commit_id(release.pop('commit'))
    ota.validate_identity(release)
    embedded = json.loads(ota.run('ostree', f'--repo={source}', 'cat', checksum,
                                 '/usr/share/qcom-ota/identity.json'))
    if embedded != release:
        raise ota.OTAError('Manifest differs from the commit contents')
    encoded = ota.run('ostree', f'--repo={source}', 'show', '--print-metadata-key=qcom.ota', checksum)
    if json.loads(ota.ast.literal_eval(encoded)) != release:
        raise ota.OTAError('Commit metadata differs from its filesystem identity')
    if previous:
        old = ota.read_json(Path(previous) / 'release.json')
        ota.compatible(old, release)
        if ota.version(release['version']) <= ota.version(old['version']):
            raise ota.OTAError('Publication must increase the release version')
    if output.exists():
        raise ota.OTAError('Output already exists; publish into a new generation directory')
    if not ota.re.fullmatch(r'[0-9A-Fa-f]{40,64}', key):
        raise ota.OTAError('Specify the complete signing-key fingerprint')
    output.parent.mkdir(parents=True, exist_ok=True)
    staging = Path(tempfile.mkdtemp(prefix='.qcom-ota-release-', dir=output.parent))
    try:
        repo = staging / 'repo'
        ota.run('ostree', f'--repo={repo}', 'init', '--mode=archive-z2')
        ota.run('ostree', f'--repo={repo}', 'pull-local', source, checksum)
        ota.run('ostree', f'--repo={repo}', 'refs', f'--create={release["ref"]}', checksum)
        ota.run('ostree', f'--repo={repo}', 'gpg-sign', f'--gpg-homedir={homedir}', checksum, key)
        ota.run('ostree', f'--repo={repo}', 'summary', '--update', f'--gpg-sign={key}',
                f'--gpg-homedir={homedir}')
        ota.run('ostree', f'--repo={repo}', 'fsck')
        public_key = ota.run('gpg', '--homedir', homedir, '--armor', '--export', key)
        if 'BEGIN PGP PUBLIC KEY BLOCK' not in public_key:
            raise ota.OTAError('Signing public key could not be exported')
        (staging / 'release-key.asc').write_text(public_key + '\n')
        ota.write_json(staging / 'release.json', dict(release, commit=checksum))
        (staging / 'release.json').chmod(0o644)
        # Validate the exact published bytes with a fresh verifying consumer.
        verification = staging / '.verify'
        ota.run('ostree', f'--repo={verification}', 'init', '--mode=bare-user')
        ota.run('ostree', f'--repo={verification}', 'remote', 'add', '--set=gpg-verify=true',
                '--set=gpg-verify-summary=true', f'--gpg-import={staging / "release-key.asc"}',
                'release', repo.as_uri())
        ota.run('ostree', f'--repo={verification}', 'pull', 'release', release['ref'])
        if ota.run('ostree', f'--repo={verification}', 'rev-parse', f'release:{release["ref"]}') != checksum:
            raise ota.OTAError('Published ref verification failed')
        shutil.rmtree(verification)
        (staging / 'README.txt').write_text(
            'Serve repo/ over HTTP(S). Provision release-key.asc through a trusted channel.\n'
            'qcom-ota configure --url https://HOST/PATH/repo --key release-key.asc\n'
            'qcom-ota check\nqcom-ota install\nqcom-ota reboot\n'
            'This is a software-validated release, not a hardware acceptance certificate.\n')
        staging.chmod(0o755)
        os.sync()
        os.rename(staging, output)
        directory = os.open(output.parent, os.O_DIRECTORY)
        try:
            os.fsync(directory)
        finally:
            os.close(directory)
        return dict(output=str(output), commit=checksum, version=release['version'], signed=True)
    finally:
        if staging.exists():
            shutil.rmtree(staging)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--repo', required=True, type=Path)
    parser.add_argument('--manifest', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--key', required=True, help='full GPG signing-key fingerprint')
    parser.add_argument('--gnupghome', required=True, type=Path)
    parser.add_argument('--previous', type=Path, help='previous release generation for version/ABI validation')
    args = parser.parse_args()
    try:
        print(json.dumps(publish(args.repo, args.manifest, args.output, args.key,
                                 args.gnupghome, args.previous), indent=2))
        return 0
    except (ota.OTAError, OSError, ValueError, KeyError, SyntaxError) as error:
        print(f'qcom-ota-release: {error}', file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
