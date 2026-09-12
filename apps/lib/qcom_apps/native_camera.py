# SPDX-License-Identifier: MIT
"""Native Q6A image contract; CamX integration is a separate capability."""
from pathlib import Path
import subprocess


def validate_manifest(spec):
    if 'kernel/config/native-camera.cfg' not in spec.kernel['config']:
        raise ValueError('native camera kernel configuration is missing')
    if 'kernel/diagnostics/0001-cci-initialize-all-completions.patch' not in spec.kernel['patches']:
        raise ValueError('native CCI completion fix is missing')
    if 'kernel-module-i2c-qcom-cci' not in spec.dependencies['runtime']:
        raise ValueError('native CCI runtime module is missing')
    targets = {entry['target'] for entry in spec.kernel['device_tree']['files']}
    if 'arch/arm64/boot/dts/qcom/q6a-imx708-native.dtsi' not in targets or not spec.kernel['device_tree']['patches']:
        raise ValueError('native camera device tree integration is missing')
    services = {Path(entry['source']).name for entry in spec.services if entry['enable']}
    if not {'imx708-native-drivers.service', 'imx708-native-preview.service'} <= services:
        raise ValueError('native camera startup services are missing')


def audit_dtb(dtb, fdtget='fdtget'):
    def get(node, prop, kind='s'):
        return subprocess.check_output([str(fdtget), '-t', kind, str(dtb), node, prop], text=True).strip()
    if 'radxa,dragon-q6a' not in get('/', 'compatible').split():
        raise ValueError('native camera DTB is not Q6A')
    labels = ('cci0', 'cci1', 'cci1_i2c0', 'cci1_i2c1', 'cam3_raw_sensor',
              'cci3_default', 'cci3_sleep', 'camss', 'tlmm', 'vreg_l10c_0p88', 'vreg_l6b_1p2')
    paths = {label: get('/__symbols__', label) for label in labels}
    for label in ('cci0', 'cci1', 'cci1_i2c0', 'cci1_i2c1', 'cam3_raw_sensor', 'camss'):
        expected = 'disabled' if label in ('cci0', 'cci1_i2c0') else 'okay'
        if get(paths[label], 'status') != expected:
            raise ValueError(f'unexpected {label} status')
    bus, sensor = paths['cci1_i2c1'], paths['cam3_raw_sensor']
    if str(Path(bus).parent) != paths['cci1'] or str(Path(sensor).parent) != bus:
        raise ValueError('sensor is not on CCI1/master1')
    if 'qcom,sc7280-cci' not in get(paths['cci1'], 'compatible').split():
        raise ValueError('native CCI driver binding missing')
    if get(bus, 'reg', 'u') != '1' or get(bus, 'clock-frequency', 'u') != '100000':
        raise ValueError('wrong CCI master or rate')
    if get('/aliases', 'i2c19') != bus:
        raise ValueError('wrong CCI alias')
    if get(sensor, 'compatible') != 'sony,imx708' or get(sensor, 'reg', 'u') != '26':
        raise ValueError('wrong sensor binding/address')
    for prop, label in [('pinctrl-0', 'cci3_default'), ('pinctrl-1', 'cci3_sleep')]:
        if get(paths['cci1'], prop, 'u') != get(paths[label], 'phandle', 'u'):
            raise ValueError('CCI does not exclusively select CAM3 pins')
    for label in ('cci3_default', 'cci3_sleep'):
        if get(paths[label], 'pins').split() != ['gpio75', 'gpio76']:
            raise ValueError('wrong CCI pins')
    children = subprocess.check_output([str(fdtget), '-l', str(dtb), '/'], text=True).split()
    if 'i2c-cam3-gpio' in children and get('/i2c-cam3-gpio', 'status') != 'disabled':
        raise ValueError('GPIO I2C conflicts with CCI')
    tlmm = get(paths['tlmm'], 'phandle', 'u')
    if get(sensor, 'reset-gpios', 'u') != f'{tlmm} 78 0':
        raise ValueError('wrong sensor enable GPIO')
    for prop, label in [('vdda-phy-supply','vreg_l10c_0p88'),('vdda-pll-supply','vreg_l6b_1p2')]:
        if get(paths['camss'],prop,'u') != get(paths[label],'phandle','u'):
            raise ValueError(f'wrong {prop}')
    tx, rx = sensor+'/port/endpoint', paths['camss']+'/ports/port@3/endpoint'
    for node, clock, lanes in [(tx,'0','1 2'),(rx,'7','0 1')]:
        if get(node,'clock-lanes','u') != clock or get(node,'data-lanes','u') != lanes:
            raise ValueError('wrong CSI lane mapping')
    if get(tx,'remote-endpoint','u') != get(rx,'phandle','u') or get(rx,'remote-endpoint','u') != get(tx,'phandle','u'):
        raise ValueError('CSI endpoint link is not reciprocal')


def audit_rootfs(root):
    root = Path(root)
    for name in ('imx708-camera','imx708-camera-launch','media-ctl','python3'):
        if not (root/'usr/bin'/name).exists():
            raise ValueError(f'missing runtime executable {name}')
    units = root/'usr/lib/systemd/system'
    for unit, target in [('imx708-native-drivers.service','multi-user.target'),('imx708-native-preview.service','graphical.target')]:
        if not (units/unit).is_file():
            raise ValueError(f'missing service {unit}')
        links = [root/etc/'systemd/system'/f'{target}.wants'/unit for etc in ('etc','usr/etc')]
        if not any(p.is_symlink() for p in links):
            raise ValueError(f'service not enabled: {unit}')
    for module in ('imx708','qcom-camss','i2c-qcom-cci'):
        if not any(root.glob(f'**/lib/modules/**/{module}.ko*')):
            raise ValueError(f'missing module {module}')
    driver_unit = (units/'imx708-native-drivers.service').read_text()
    starts = [line for line in driver_unit.splitlines() if line.startswith('ExecStart=')]
    if starts != ['ExecStart=/sbin/modprobe '+name for name in ('i2c-qcom-cci', 'imx708', 'qcom-camss')]:
        raise ValueError('incorrect native driver load order')
    configs = [root/etc/'modprobe.d/imx708-native.conf' for etc in ('etc','usr/etc')]
    text = next((p.read_text() for p in configs if p.is_file()), '')
    if not all(f'blacklist {module}' in text for module in ('imx708','qcom_camss','i2c_qcom_cci','i2c_gpio')):
        raise ValueError('ordered probe configuration missing')
    weston = next((p.read_text() for p in (root/'etc/xdg/weston/weston.ini', root/'usr/etc/xdg/weston/weston.ini') if p.is_file()), '')
    if 'path=/usr/bin/imx708-camera-launch' not in weston:
        raise ValueError('Weston camera launcher missing')
    icon = root/'usr/share/weston/imx708-camera.png'
    if not icon.is_file() or not icon.read_bytes().startswith(b'\x89PNG\r\n\x1a\n'):
        raise ValueError('Weston camera icon missing')
