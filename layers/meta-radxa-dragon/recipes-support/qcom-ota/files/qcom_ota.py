#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Manual signed OSTree deployment manager. No daemon, polling, or implicit reboot."""
import argparse
import ast
import contextlib
import fcntl
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import time
from urllib.parse import urlsplit

IDENTITY = Path('/usr/share/qcom-ota/identity.json')
REPO = Path('/sysroot/ostree/repo')
STATE = Path('/var/lib/qcom-ota')
BOOT = Path('/boot')
REMOTE = 'qcom'
BOOT_COUNT_PATH = Path('/sys/firmware/efi/efivars/LoaderBootCountPath-4a67b082-0a4c-41cf-b6c7-440b29bb8c4f')


class OTAError(RuntimeError):
    pass


def run(*args):
    result = subprocess.run([str(x) for x in args], text=True, stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE, env={**os.environ, 'LC_ALL': 'C'})
    if result.returncode:
        raise OTAError(f'{args[0]} failed ({result.returncode}): {result.stderr.strip()}')
    return result.stdout.strip()


def atomic_write(path, content, mode=0o600):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, temp = tempfile.mkstemp(prefix='.' + path.name, dir=path.parent)
    try:
        with os.fdopen(fd, 'w') as stream:
            os.fchmod(stream.fileno(), mode)
            stream.write(content)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temp, path)
        directory = os.open(path.parent, os.O_DIRECTORY)
        try:
            os.fsync(directory)
        finally:
            os.close(directory)
    finally:
        if os.path.exists(temp):
            os.unlink(temp)


def write_json(path, value):
    atomic_write(path, json.dumps(value, sort_keys=True, indent=2) + '\n')


def read_json(path):
    return json.loads(Path(path).read_text())


def version(value):
    if not isinstance(value, str) or not re.fullmatch(r'(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)', value):
        raise OTAError('Version must be MAJOR.MINOR.PATCH')
    return tuple(map(int, value.split('.')))


def validate_identity(value):
    if value.get('schema') != 1:
        raise OTAError('Unsupported identity schema')
    for field in ('machine', 'image', 'channel'):
        if not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9._-]*', value.get(field, '')):
            raise OTAError(f'Invalid {field}')
    if value.get('ref') != '/'.join(value[k] for k in ('machine', 'image', 'channel')):
        raise OTAError('Ref and compatibility identity disagree')
    if value.get('backend') not in ('systemd-boot', 'embloader', 'fixed'):
        raise OTAError('Unknown boot backend')
    if not re.fullmatch('[0-9a-f]{64}', value.get('kernel_fingerprint', '')):
        raise OTAError('Missing kernel compatibility fingerprint')
    version(value.get('version'))
    for field in ('installed_bytes', 'boot_bytes', 'min_free_kib'):
        if not isinstance(value.get(field), int) or value[field] < 0:
            raise OTAError(f'Invalid {field}')
    return value


def compatible(current, target, allow_rollback=False):
    validate_identity(current)
    validate_identity(target)
    for field in ('machine', 'image', 'channel', 'ref', 'backend'):
        if current[field] != target[field]:
            raise OTAError(f'Incompatible {field}: {target[field]}')
    if current['backend'] == 'fixed' and current['kernel_fingerprint'] != target['kernel_fingerprint']:
        raise OTAError('Update changes the fixed boot payload, boot parameters, or modules; reflash required')
    if not allow_rollback and version(target['version']) < version(current['version']):
        raise OTAError('Downgrade rejected; use rollback for a retained deployment')


def commit_id(value):
    if not re.fullmatch('[0-9a-f]{64}', value):
        raise OTAError('Expected an OSTree commit checksum')
    return value


def ostree(*args):
    return run('ostree', f'--repo={REPO}', *args)


def metadata(checksum):
    # OSTree show prints a GVariant string, which is a quoted Python-compatible
    # string for the deliberately restricted, JSON-encoded metadata used here.
    raw = ostree('show', '--print-metadata-key=qcom.ota', commit_id(checksum))
    return validate_identity(json.loads(ast.literal_eval(raw)))


def deployments():
    result = json.loads(run('ostree', 'admin', 'status', '--json'))['deployments']
    return [x for x in result if x['stateroot'] == 'qcom']


def booted():
    for item in deployments():
        if item['booted']:
            return item
    raise OTAError('Not running an OSTree deployment; install the factory OTA image first')


def record(state, **fields):
    value = dict(state=state, time=int(time.time()), **fields)
    write_json(STATE / 'state.json', value)
    print(json.dumps(value), flush=True)


@contextlib.contextmanager
def locked():
    STATE.mkdir(parents=True, exist_ok=True)
    with (STATE / 'lock').open('a') as stream:
        try:
            fcntl.flock(stream, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError as error:
            raise OTAError('Another OTA operation is active') from error
        yield


def configure(url, key):
    parts = urlsplit(url)
    if parts.scheme not in ('http', 'https') or not parts.hostname or parts.username or parts.password or parts.query or parts.fragment:
        raise OTAError('Use an HTTP(S) repository URL without embedded credentials, query or fragment')
    key = Path(key).resolve(strict=True)
    # Provisioning is explicit and root-only. Never auto-import keys from the server.
    ostree('remote', 'add', '--force', '--set=gpg-verify=true', '--set=gpg-verify-summary=true',
           f'--gpg-import={key}', REMOTE, url.rstrip('/'))
    record('configured', url=url.rstrip('/'))


def space_check(current, target, include_download=True):
    reserve = max(current['min_free_kib'], target['min_free_kib']) * 1024
    needed = target['installed_bytes'] * (2 if include_download else 1) + reserve
    if shutil.disk_usage(REPO).free < needed:
        raise OTAError(f'Insufficient root filesystem space: need at least {needed} bytes free')
    if current['backend'] != 'fixed':
        run('mountpoint', '-q', BOOT)
        if shutil.disk_usage(BOOT).free < target['boot_bytes'] * 2 + 16 * 1024 * 1024:
            raise OTAError('Insufficient ESP space for new boot payload and fallback')


def check(current):
    # Enforce policy on every operation, even if remote settings were changed.
    for setting in ('gpg-verify', 'gpg-verify-summary'):
        if ostree('config', 'get', f'remote "{REMOTE}".{setting}') != 'true':
            raise OTAError(f'Remote must enable {setting}; run configure')
    ostree('pull', '--commit-metadata-only', REMOTE, current['ref'])
    checksum = commit_id(ostree('rev-parse', f'{REMOTE}:{current["ref"]}'))
    target = metadata(checksum)
    compatible(current, target)
    active = booted()['checksum']
    if checksum != active and version(target['version']) == version(current['version']):
        raise OTAError('A different commit reuses the installed release version')
    record('available' if checksum != active else 'up-to-date', commit=checksum, version=target['version'])
    return checksum, target


def fixed_select(deployment):
    checksum = commit_id(deployment['checksum'])
    serial = int(deployment['serial'])
    target = f'deploy/qcom/deploy/{checksum}.{serial}'
    link = REPO.parent / 'qcom-active'
    temp = link.with_name('.qcom-active.tmp')
    if not (REPO.parent / target).is_dir():
        raise OTAError('Selected deployment is missing')
    temp.unlink(missing_ok=True)
    temp.symlink_to(target)
    os.replace(temp, link)
    os.sync()


def install(current):
    active = booted()
    if any(d['pending'] for d in deployments()):
        raise OTAError('A deployment is already pending; reboot or roll it back first')
    checksum, target = check(current)
    if checksum == active['checksum']:
        return
    space_check(current, target)
    record('downloading', commit=checksum)
    # Fetch exactly the verified checksum, not a moving ref.
    ostree('pull', REMOTE, checksum)
    if metadata(checksum) != target:
        raise OTAError('Commit metadata changed during download')
    space_check(current, target, include_download=False)
    record('installing', commit=checksum, previous=active['checksum'])
    if not active.get('pinned'):
        manage_pin(active['checksum'])
    run('ostree', 'admin', 'pin', str(active['index']))
    # Retain the running deployment; the OSTree backend writes new boot files
    # before switching deployment references. Never overwrite the active kernel.
    run('ostree', 'admin', 'deploy', '--os=qcom',
        f'--origin-file={origin_file(current)}', checksum)
    if current['backend'] == 'fixed':
        fixed_select(next(x for x in deployments() if x['checksum'] == checksum))
    record('reboot-pending', commit=checksum, previous=active['checksum'])


def origin_file(current):
    path = STATE / 'deployment.origin'
    atomic_write(path, f'[origin]\nrefspec={REMOTE}:{current["ref"]}\n')
    return path


def manage_pin(checksum):
    path = STATE / 'managed-pins.json'
    managed = set(read_json(path) if path.exists() else [])
    managed.add(commit_id(checksum))
    write_json(path, sorted(managed))


def retain_successful_pair(active):
    """Unpin only this client's older deployments, never administrator pins."""
    items = deployments()
    previous = next((item for item in items if item['rollback']), None)
    keep = {active['checksum']}
    if previous:
        keep.add(previous['checksum'])
    path = STATE / 'managed-pins.json'
    managed = set(read_json(path) if path.exists() else [])
    for item in sorted(items, key=lambda item: item['index'], reverse=True):
        if item['checksum'] in managed - keep and not item['booted'] and not item['pending']:
            if item['pinned']:
                run('ostree', 'admin', 'pin', '--unpin', str(item['index']))
            run('ostree', 'admin', 'undeploy', str(item['index']))
    write_json(path, sorted(managed & keep))


def rollback(current):
    items = deployments()
    active = booted()
    # A pending update can be cancelled by choosing the running deployment.
    target = active if any(x['pending'] for x in items) else next((x for x in items if x['rollback']), None)
    if target is None:
        raise OTAError('No retained deployment to roll back to')
    compatible(current, metadata(target['checksum']), allow_rollback=True)
    run('ostree', 'admin', 'set-default', str(target['index']))
    if current['backend'] == 'fixed':
        fixed_select(target)
    record('reboot-pending', commit=target['checksum'], rollback=True)


def bless_embloader(active):
    # Match the BLS deployment argument to the running deployment, including
    # its serial. The EFI variable must identify that exact entry.
    expected = REPO.parent / 'deploy/qcom/deploy' / f'{commit_id(active["checksum"])}.{int(active["serial"])}'
    def matches(entry):
        for line in entry.read_text().splitlines():
            if line.startswith('options '):
                for arg in line.split()[1:]:
                    if arg.startswith('ostree='):
                        value = arg.removeprefix('ostree=')
                        if not value.startswith('/ostree/') or '..' in Path(value).parts:
                            raise OTAError('Invalid BLS deployment path')
                        return (REPO.parent.parent / value.lstrip('/')).resolve() == expected.resolve()
        return False

    if not BOOT_COUNT_PATH.exists():
        entries = [entry for entry in (BOOT / 'loader/entries').glob('*.conf') if matches(entry)]
        if not entries or any('+' in entry.name for entry in entries):
            raise OTAError('Missing boot-count EFI variable for the running deployment')
        return  # Initial factory deployment or a previously confirmed fallback.
    value = BOOT_COUNT_PATH.read_bytes()[4:].decode('utf-16-le').rstrip('\0').replace('\\', '/')
    if not re.fullmatch(r'/loader/entries/[A-Za-z0-9._-]+\+[0-9]+-[0-9]+\.conf', value):
        raise OTAError('Invalid boot-count path from embloader')
    entry = BOOT / value.lstrip('/')
    good = entry.with_name(re.sub(r'\+[0-9]+-[0-9]+(?=\.conf$)', '', entry.name))
    if not entry.is_file():
        if good.is_file() and matches(good):
            return  # Idempotent confirmation in the same boot.
        # Pruning older deployments renumbers the already blessed BLS entries.
        # Accept only an uncounted entry for this exact running deployment.
        entries = [candidate for candidate in (BOOT / 'loader/entries').glob('*.conf')
                   if matches(candidate)]
        if entries and all('+' not in candidate.name for candidate in entries):
            return
        raise OTAError('Booted BLS entry is missing')
    if not matches(entry):
        raise OTAError('Boot-count entry does not match the running deployment')
    os.replace(entry, good)
    os.sync()


def confirm(current):
    active = booted()
    health = read_json('/etc/qcom-ota/health.json')
    for mount in health.get('mounts', ['/sysroot']):
        run('mountpoint', '-q', mount)
    if current['backend'] != 'fixed':
        run('mountpoint', '-q', BOOT)
    services = health.get('services', [])
    for service in services:
        if not re.fullmatch(r'[A-Za-z0-9_.@:-]+\.service', service):
            raise OTAError('Invalid health-check service name')
    deadline = time.monotonic() + 120
    while True:
        try:
            for unit in ['multi-user.target', *services]:
                run('systemctl', 'is-active', '--quiet', unit)
            break
        except OTAError:
            if time.monotonic() >= deadline:
                raise OTAError('Boot health checks did not pass within 120 seconds')
            time.sleep(2)
    if current['backend'] == 'systemd-boot':
        run('/usr/lib/systemd/systemd-bless-boot', 'good')
        atomic_write('/run/qcom-ota-health-passed', active['checksum'] + '\n')
    elif current['backend'] == 'embloader':
        bless_embloader(active)
    if not active.get('pinned'):
        manage_pin(active['checksum'])
    run('ostree', 'admin', 'pin', str(active['index']))
    retain_successful_pair(active)
    record('confirmed', commit=active['checksum'], version=current['version'])


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest='command', required=True)
    config = commands.add_parser('configure')
    config.add_argument('--url', required=True)
    config.add_argument('--key', required=True)
    for name in ('status', 'check', 'install', 'rollback', 'reboot', 'confirm'):
        commands.add_parser(name)
    args = parser.parse_args(argv)
    try:
        current = validate_identity(read_json(IDENTITY))
        if args.command == 'status':
            try:
                last = read_json(STATE / 'state.json')
            except (FileNotFoundError, PermissionError):
                last = None
            print(json.dumps(dict(identity=current, deployments=deployments(), last=last), indent=2))
            return 0
        if os.geteuid() != 0:
            raise OTAError('This operation requires root')
        with locked():
            try:
                if args.command == 'configure':
                    configure(args.url, args.key)
                elif args.command == 'check':
                    check(current)
                elif args.command == 'install':
                    install(current)
                elif args.command == 'rollback':
                    rollback(current)
                elif args.command == 'confirm':
                    confirm(current)
                elif args.command == 'reboot':
                    run('systemctl', 'reboot')
            except (OTAError, OSError, ValueError, KeyError, SyntaxError) as error:
                record('failed', error=str(error))
                raise
        return 0
    except (OTAError, OSError, ValueError, KeyError, SyntaxError) as error:
        print(f'qcom-ota: {error}', file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
