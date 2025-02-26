#!/usr/bin/env python3

import contextlib
import json
import os
import pathlib
import random
import re
import subprocess
import sysconfig
from typing import Optional


ROOT = pathlib.Path.cwd()
assert (ROOT / '.git').exists(), "You need to call this script from the root of the repository."

SRC = ROOT / 'src'
LIB_DIR = ROOT / 'install' / 'lib'
ICD_DIR = ROOT / 'install' / 'share' / 'vulkan' / 'icd.d'
ARCH = sysconfig.get_platform().split('-')[-1].lower()


@contextlib.contextmanager
def section(title: str):
    section_slug = re.sub(r'[^a-zA-Z0-9-_]', '', title)
    section_id = f'{random.randrange(999)}-{section_slug}'
    print(f'\x1b[0Ksection_start:{section_id}\r\x1b[0K{title}\x1b[0m')
    try:
        yield
    finally:
        print(f'\x1b[0Ksection_end:{section_id}:\r\x1b[0K', end='')


def vulkaninfo(
    env_for_test: dict[str, str],
    output_path: pathlib.Path,
) -> None:
    cmd = [
        'vulkaninfo',
        '--json',
        '--show-formats',
        '--output', '/dev/stdout',
    ]
    data = subprocess.check_output(cmd, env=env_for_test)
    json_dict = json.loads(data)

    props = json_dict['capabilities']['device']['properties']

    # Remove Mesa version number
    if 'VkPhysicalDeviceDriverProperties' in props:
        props['VkPhysicalDeviceDriverProperties']['driverInfo'] = 'Mesa'
    if 'VkPhysicalDeviceVulkan12Properties' in props:
        props['VkPhysicalDeviceVulkan12Properties']['driverInfo'] = 'Mesa'

    # Zero-out hashes depending on the version & git commit
    def zero_uuid(ext_name: str, prop: str) -> None:
        if ext_name in props:
            for i, _ in enumerate(props[ext_name][prop]):
                props[ext_name][prop][i] = 0

    zero_uuid('VkPhysicalDeviceHostImageCopyPropertiesEXT',
              'optimalTilingLayoutUUID')
    zero_uuid('VkPhysicalDeviceIDProperties',
              'deviceUUID')
    zero_uuid('VkPhysicalDeviceIDProperties',
              'driverUUID')
    zero_uuid('VkPhysicalDeviceProperties',
              'pipelineCacheUUID')
    zero_uuid('VkPhysicalDeviceShaderModuleIdentifierPropertiesEXT',
              'shaderModuleIdentifierAlgorithmUUID')
    zero_uuid('VkPhysicalDeviceShaderObjectPropertiesEXT',
              'shaderBinaryUUID')
    zero_uuid('VkPhysicalDeviceVulkan11Properties',
              'deviceUUID')
    zero_uuid('VkPhysicalDeviceVulkan11Properties',
              'driverUUID')

    # Drop this entirely because it doesn't contain any useful information and
    # is full of non-constant stuff.
    del json_dict['profiles']

    with output_path.open('w') as json_file:
        json.dump(json_dict, json_file, indent=2, sort_keys=True)


def test_driver(
    *,
    driver: str,
    folder: pathlib.Path,
    shim_name: Optional[str] = None,
    hw_envvar: Optional[str] = None,
    hw_variants: list[str] = [],
    extra_env: dict[str, str] = {},
) -> None:
    # Neither or both
    assert (hw_envvar is None) == (hw_variants == [])

    driver_icd = ICD_DIR / f'{driver}_icd.{ARCH}.json'

    env_for_test = {
        # Load the driver we want to test
        'VK_ICD_FILENAMES': driver_icd.as_posix(),
        'LD_LIBRARY_PATH': LIB_DIR.as_posix(),
        'XDG_RUNTIME_DIR': os.environ['XDG_RUNTIME_DIR'],
        **extra_env,
    }

    # Optional, to make it easier for devs to run the script locally
    if 'WAYLAND_DISPLAY' in os.environ:
        env_for_test['WAYLAND_DISPLAY'] = os.environ['WAYLAND_DISPLAY']
    if 'DISPLAY' in os.environ:
        env_for_test['DISPLAY'] = os.environ['DISPLAY']

    if shim_name is not None:
        shim_so = LIB_DIR / f'lib{shim_name}_noop_drm_shim.so'
        env_for_test['LD_PRELOAD'] = shim_so.as_posix()
        env_for_test['DRM_SHIM_DEBUG'] = 'true'

    with section(f'Running `vulkaninfo` for {driver}'):
        if hw_envvar is None:
            output_file = folder / 'vulkaninfo.json'
            vulkaninfo(env_for_test, output_file)
        else:
            for hw in hw_variants:
                env_for_test[hw_envvar] = hw
                with section(f'Running `vulkaninfo` for {driver} on {hw}'):
                    output_file = folder / f'vulkaninfo-{hw}.json'
                    vulkaninfo(env_for_test, output_file)


def main():
    test_driver(
        driver='freedreno',
        folder=SRC / 'freedreno' / 'vulkan',
        shim_name='freedreno',
    )

    test_driver(
        driver='intel',
        folder=SRC / 'intel' / 'vulkan',
        shim_name='intel',
        hw_envvar='INTEL_STUB_GPU_PLATFORM',
        hw_variants=[
            'tgl', 'rkl', 'adl', 'rpl', 'dg1', 'sg1',
            'dg2',
        ],
    )

    test_driver(
        driver='intel_hasvk',
        folder=SRC / 'intel' / 'vulkan_hasvk',
        shim_name='intel',
        hw_envvar='INTEL_STUB_GPU_PLATFORM',
        hw_variants=[
            'ivb', 'byt',
            'hsw',
            'bdw', 'chv',
            # 'skl', 'kbl', 'aml', 'cml', 'whl', 'bxt', 'glk',
            # 'icl', 'ehl', 'jsl',
        ],
    )

    test_driver(
        driver='lvp',
        folder=SRC / 'gallium' / 'frontends' / 'lavapipe',
    )

    test_driver(
        driver='nouveau',
        folder=SRC / 'nouveau' / 'vulkan',
        shim_name='nouveau',
        hw_envvar='NOUVEAU_CHIPSET',
        hw_variants=[
            '162',
        ],
    )

    test_driver(
        driver='radeon',
        folder=SRC / 'amd' / 'vulkan',
        shim_name='amdgpu',
    )

if __name__ == '__main__':
    main()
