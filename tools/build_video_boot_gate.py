#!/usr/bin/env python3
"""Build our exact-profile boot gate; never extract, distribute or install vendor code."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
NATIVE_SHA256 = 'f942639021769407028aa41e1064d794dd86f07ffab1442a91112c2aa678c799'
LIBRARIES = {
    'libpthread-2.19-2014.04.so': 'd2e902158966b0dd39e94a8ed3fc790602e0317b6e6a37b2fa1bd93eac46a4e0',
    'libdl-2.19-2014.04.so': '3b12b2ed8dff1efd545ce92207a3c9848168c9bd59c76e27cb832222ae422097',
    'librt-2.19-2014.04.so': 'c7ebb9c4c0ed4cd71eb5d4b4639b1fa399c9f5f9252e2204653fd5c266b1d109',
    'libc-2.19-2014.04.so': '12bc1219cb83303f31265c4aa9677f107b7b3131fdf06ac61de806d6dc7bab11',
}


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def verified_inputs(native, directory):
    if not native.is_file() or digest(native) != NATIVE_SHA256:
        raise ValueError('Native camera image differs from the qualified profile')
    files = []
    for name, expected in LIBRARIES.items():
        path = directory / name
        if not path.is_file() or digest(path) != expected:
            raise ValueError('Native runtime differs from the qualified profile: ' + name)
        data = path.read_bytes()[:20]
        if len(data) != 20 or data[:6] != b'\x7fELF\x01\x01' or data[18:20] != b'\x28\x00':
            raise ValueError('Expected a little-endian ELF32 ARM runtime: ' + name)
        files.append(path.resolve())
    return files


def build(zig, native, libraries, out):
    files = verified_inputs(native, libraries)
    version = subprocess.check_output([str(zig), 'version'], text=True).strip()
    lock = json.loads((ROOT / 'build/inputs.lock.json').read_text())
    if version != lock['tools']['zig']:
        raise ValueError('Zig version differs from the pinned toolchain')
    target = out / 'video-boot-gate.so'
    manifest = out / 'video-boot-gate.json'
    if target.exists() or manifest.exists():
        raise ValueError('Use a fresh output directory; existing artifacts are never overwritten')
    compiler = json.loads(subprocess.check_output([str(zig), 'env'], text=True))
    headers = Path(compiler['lib_dir']) / 'libc/include'
    out.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, ZIG_GLOBAL_CACHE_DIR=str(out / 'zig-cache'), ZIG_LOCAL_CACHE_DIR=str(out / 'zig-local-cache'))
    command = [str(zig), 'cc', '-target', 'arm-linux-gnueabihf.2.19', '-mcpu=cortex_a7', '-mthumb',
        '-O2', '-g0', '-Wall', '-Wextra', '-Werror', '-fno-stack-protector', '-shared', '-fPIC', '-nostdlib']
    for folder in ('arm-linux-gnueabihf', 'generic-glibc', 'arm-linux-any', 'any-linux-any'):
        command += ['-isystem', str(headers / folder)]
    command += ['-Wl,--allow-shlib-undefined,--gc-sections,--version-script=' + str(ROOT / 'build/video-boot/exports.map')
        + ',-T,' + str(ROOT / 'build/video-boot/old-loader.ld') + ',-s,--no-as-needed',
        str(ROOT / 'app/src/g2h_video_boot_gate.c'), *map(str, files), '-o', str(target)]
    subprocess.run(command, check=True, env=env)
    record = {'format': 'g2h-video-boot-build-v1', 'vendor_firmware_included': False,
        'native_profile_sha256': NATIVE_SHA256, 'runtime_inputs_sha256': LIBRARIES, 'zig': version,
        'source_sha256': {name: digest(ROOT / name) for name in ('app/src/g2h_video_boot_gate.c',
            'app/src/g2h_video_boot_tasks.h', 'build/video-boot/exports.map', 'build/video-boot/old-loader.ld')},
        'artifact': {'path': target.name, 'bytes': target.stat().st_size, 'sha256': digest(target)},
        'installed': False}
    manifest.write_text(json.dumps(record, indent=2) + '\n')
    return record


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--zig', type=Path, required=True)
    parser.add_argument('--native-image', type=Path, required=True, help='Your own exact qualified camera image')
    parser.add_argument('--native-libraries', type=Path, required=True, help='Your own extracted native runtime, never included here')
    parser.add_argument('--out', type=Path, required=True, help='Fresh directory for our gate and its manifest')
    args = parser.parse_args()
    result = build(args.zig.resolve(), args.native_image.resolve(), args.native_libraries.resolve(), args.out.resolve())
    print('Own boot gate built, not installed:', result['artifact']['sha256'])


if __name__ == '__main__':
    main()
