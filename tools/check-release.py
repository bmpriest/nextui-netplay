#!/usr/bin/env python3
"""Check Complete archive layout, executable modes, architecture and payloads."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import zipfile

ROOT = Path(__file__).resolve().parents[1]
COMPAT = ('fceumm', 'picodrive', 'snes9x', 'mednafen_supafaust', 'pcsx_rearmed', 'gambatte', 'gpsp')


def check(path, spruce):
    prefix = 'App/Netplay/' if spruce else 'Netplay.pak/'
    platforms = ('tg5040', 'tg5050', 'my355', 'h700') if spruce else ('tg5040', 'tg5050', 'my282', 'my355', 'h700')
    with zipfile.ZipFile(path) as archive:
        assert archive.testzip() is None, 'ZIP CRC failure'
        names = set(archive.namelist())
        assert all(name.startswith(prefix) or name in ('App/',) for name in names), 'incorrect extraction layout'
        assert len(names) == len(archive.infolist()), 'duplicate ZIP entries'
        for name in names:
            assert '..' not in Path(name).parts, name
            bundled_bios = name.startswith(prefix + 'cores/nds/base/NDS.pak/drastic/system/') and Path(name).name in ('drastic_bios_arm7.bin', 'drastic_bios_arm9.bin', 'nds_bios_arm7.bin', 'nds_bios_arm9.bin', 'nds_firmware.bin')
            assert bundled_bios or not name.endswith(('.gba', '.gb', '.gbc', '.nds', '.sfc', '.nes', '.cue', '.bin')), f'unexpected ROM payload: {name}'
            assert '__pycache__' not in name and not name.endswith(('.pyc', '.log')), f'development artifact: {name}'
            assert Path(name).name not in ('session', 'settings', 'rig.conf', 'rig.json'), f'mutable fixture state: {name}'
        def require(relative, executable=False):
            name = prefix + relative
            assert name in names, f'missing {name}'
            if executable:
                assert (archive.getinfo(name).external_attr >> 16) & 0o111, f'not executable: {name}'
            return archive.read(name)
        def elf(relative, platform):
            data = require(relative, True)
            assert data[:4] == b'\x7fELF' and data[5] == 1, relative
            assert data[4] == (1 if platform == 'my282' else 2), relative
            assert struct.unpack_from('<H', data, 18)[0] == (40 if platform == 'my282' else 183), relative
            return data
        require('launch.sh', True)
        require('README.md')
        commit = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT).strip()
        assert require('SOURCE_COMMIT').strip() == commit, 'archive source commit differs from checkout'
        require('docs/release.md')
        require('launcher/invite.sh', True)
        require('pak_onboarding.json')
        for platform in platforms:
            for name in ('netplay.elf', 'netplay-broker.elf', 'netplay-wifips.elf', 'netplay_shim.so'):
                relative = f'bin/{platform}/{name}'
                data = elf(relative, platform)
                if spruce and platform == 'my355' and name == 'netplay.elf':
                    assert b'libmali_hook.so' not in data, 'RK3566 backend depends on the Flip-only graphics hook'
                local = ROOT/'bin'/('spruce' if spruce and name.endswith('.elf') else '')/platform/name
                assert hashlib.sha256(data).digest() == hashlib.sha256(local.read_bytes()).digest(), f'stale binary: {relative}'
            for core in ('gambatte', 'gpsp', 'gambatte_dual', 'mgba_dual'):
                elf(f'cores/override/{platform}/{core}_libretro.so', platform)
            if not spruce:
                elf(f'cores/mgba/{platform}/MGBA.pak/mgba_libretro.so', platform)
        for arch in (('aarch64',) if spruce else ('armv7', 'aarch64')):
            for core in COMPAT:
                require(f'cores/compatibility/{arch}/{core}_libretro.so')
        if spruce:
            require('spruce_ui/CHEEVOS-LICENSE')
            require('spruce_ui/footer.py')
            config = json.loads(require('config.json'))
            assert set(config['devices']) == {'TRIMUI_A133P', 'TRIMUI_SMART_PRO_S', 'MIYOO_FLIP', 'ANBERNIC_RGXX', 'RGB30', 'RGB20SX', 'MINILOONG_POCKET1'}
            for platform in platforms:
                elf(f'bin/{platform}/libdrastic_wifi.so', platform)
                for name in ('libsamplerate.so.0', 'libzip.so.5'):
                    require(f'bin/{platform}/{name}')
        else:
            require('cores/nds/base/NDS.pak/drastic/drastic', True)
            for platform in ('tg5040', 'tg5050', 'h700'):
                require(f'cores/dsp/{platform}/DSP.pak/{platform}/dsperate', True)
                require(f'cores/nds/{platform}/NDS.pak/drastic/libs/libdrastic_wifi.so')
    digest = hashlib.sha256(path.read_bytes()).hexdigest()
    print(f'{path.name}: PASS ({path.stat().st_size:,} bytes) SHA256 {digest}')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', nargs='?', type=Path, default=ROOT/'dist')
    args = parser.parse_args()
    check(args.directory/'NextUI-Netplay-Complete.zip', False)
    check(args.directory/'Spruce-Netplay-Complete.zip', True)
