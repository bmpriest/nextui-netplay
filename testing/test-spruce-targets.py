#!/usr/bin/env python3
"""Launch the packaged shell entry point against a fake card for each RC model."""
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
TARGETS = {
    'Brick': ('tg5040', 'TRIMUI_BRICK'),
    'BrickPro': ('tg5040', 'TRIMUI_BRICK_PRO'),
    'SmartPro': ('tg5040', 'TRIMUI_SMART_PRO'),
    'SmartProS': ('tg5050', 'TRIMUI_SMART_PRO_S'),
    'Flip': ('my355', 'MIYOO_FLIP'),
    'AnbernicXX640480': ('h700', 'ANBERNIC_RGXX640480'),
    'AnbernicXX720480NoStick': ('h700', 'ANBERNIC_RGXX720480'),
    'AnbernicRG28XX': ('h700', 'ANBERNIC_RG28XX'),
    'AnbernicRGCubeXX': ('h700', 'ANBERNIC_RGCUBEXX'),
    'RGB30': ('my355', 'RGB30'),
    'RGB20SX': ('my355', 'RGB20SX'),
    'Miniloong': ('my355', 'MINILOONG_POCKET1'),
}

class SpruceTargets(unittest.TestCase):
    def test_menu_advertises_only_rc_families(self):
        devices = json.loads((ROOT/'launcher/spruce/config.json').read_text())['devices']
        self.assertEqual(set(devices), {'TRIMUI_A133P', 'TRIMUI_SMART_PRO_S', 'MIYOO_FLIP',
                                       'ANBERNIC_RGXX', 'RGB30', 'RGB20SX', 'MINILOONG_POCKET1'})

    def test_real_launcher_routes_each_model_and_runtime(self):
        with tempfile.TemporaryDirectory() as directory:
            card = Path(directory)
            app = card/'App/Netplay'
            def write(relative, text, executable=False):
                path = card/relative
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(text)
                if executable: path.chmod(0o755)
            write('App/Netplay/launcher/firmware.sh', (ROOT/'launcher/spruce/firmware.sh').read_text())
            write('App/Netplay/launch.sh', (ROOT/'launcher/spruce/launch.sh').read_text().replace('/mnt/SDCARD', str(card)))
            write('spruce/scripts/helperFunctions.sh', 'PLATFORM=$TEST_MODEL\nget_python_path() { echo /usr/bin/python3; }\n')
            for library in ('libSDL2-2.0', 'libSDL2_image-2.0', 'libSDL2_ttf-2.0'):
                write(f'App/PyUI/dll/{library}.so', 'fake SDL runtime')
            write('spruce/flip/bin/MainUI', '#!/bin/sh\nprintf "%s\\n" "$PLATFORM" "$NETPLAY_PYUI_DEVICE" "$PYSDL2_DLL_PATH" "${SDL_VIDEODRIVER:-}" "${SDL_AUDIODRIVER:-}" "$NETPLAY_ENGINE_LD_LIBRARY_PATH" "$*" > "$TEST_RESULT"\n', True)
            for build in ('tg5040', 'tg5050', 'my355', 'h700'):
                write(f'App/Netplay/bin/{build}/netplay.elf', '#!/bin/sh\nexit 0\n', True)
            for model, (build, identity) in TARGETS.items():
                with self.subTest(model=model):
                    output = card/'result'
                    env = {**os.environ, 'SDCARD_PATH': str(card), 'TEST_MODEL': model,
                           'TEST_RESULT': str(output), 'NETPLAY_RUNTIME_ROOT': str(card/'runtime')}
                    env.pop('SDL_VIDEODRIVER', None)
                    env.pop('SDL_AUDIODRIVER', None)
                    subprocess.run(['sh', str(app/'launch.sh')], env=env, check=True,
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE)
                    values = output.read_text().splitlines()
                    self.assertEqual(values[:2], [build, identity])
                    self.assertEqual(values[-1], '-m spruce_ui')
                    if build == 'my355':
                        alias = card/'runtime/spruce-libs/libSDL2-2.0.so.0'
                        self.assertEqual(alias.resolve(), card/'App/PyUI/dll/libSDL2-2.0.so')
                        self.assertTrue(values[5].startswith(str(card/'runtime/spruce-libs') + ':'))
                    if model in ('RGB30', 'RGB20SX', 'Miniloong'):
                        self.assertEqual(values[3:5], ['kmsdrm', 'alsa'])

    def test_missing_rk3566_sdl_fails_instead_of_loading_fbdev(self):
        with tempfile.TemporaryDirectory() as directory:
            env = {**os.environ, 'SDCARD_PATH': directory, 'PLATFORM': 'my355',
                   'NETPLAY_RUNTIME_ROOT': str(Path(directory)/'runtime')}
            result = subprocess.run(['sh', '-c', '. "$1"; np_spruce_engine_libraries',
                                     'test', str(ROOT/'launcher/spruce/firmware.sh')],
                                    env=env, capture_output=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(result.stdout, b'')

    def test_other_nightly_models_are_not_mapped(self):
        script = '. "$1"; np_spruce_platform "$2"'
        for model in ('A30', 'MiyooMini', 'RG353M', 'Zero40', 'unknown'):
            result = subprocess.run(['sh', '-c', script, 'test',
                                     str(ROOT/'launcher/spruce/firmware.sh'), model], capture_output=True)
            self.assertNotEqual(result.returncode, 0, model)

if __name__ == '__main__':
    unittest.main()
