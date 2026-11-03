#!/usr/bin/env python3
# Copyright 2026 OpenDroidOS
# SPDX-License-Identifier: MIT
"""Build native Turnip from explicit OpenHarmony product dependencies."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import struct
import subprocess
import tempfile


def write_changed(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    if not path.exists() or path.read_text() != value:
        path.write_text(value)


def source_version(root, name):
    if name == 'zlib':
        text = (root / 'third_party/zlib/zlib.h').read_text()
        return re.search(r'#define ZLIB_VERSION "([^"]+)"', text).group(1)
    if name == 'expat':
        text = (root / 'third_party/expat/expat/lib/expat.h').read_text()
        return '.'.join(re.search(r'#define XML_' + part + r'_VERSION (\d+)', text).group(1)
                        for part in ('MAJOR', 'MINOR', 'MICRO'))
    bundles = {
        'libhilog': 'base/hiviewdfx/hilog/bundle.json',
        'libhitrace': 'base/hiviewdfx/hitrace/bundle.json',
        'libbegetutil': 'base/startup/init/bundle.json',
        'libqos': 'foundation/resourceschedule/qos_manager/bundle.json',
        'libdrm': 'third_party/libdrm/bundle.json',
    }
    return json.loads((root / bundles[name]).read_text())['version']


def validate_library(path):
    with path.open('rb') as stream:
        header = stream.read(64)
    if header.startswith(b'!<arch>\n'):
        return
    if not (header[:6] == b'\x7fELF\x02\x01' and struct.unpack_from('<H', header, 18)[0] == 183):
        raise ValueError(f'Expected a native AArch64 library or archive: {path}')


def validate_cache(build, compiler, sysroot):
    database = build / 'compile_commands.json'
    if not database.is_file():
        raise ValueError(f'Configured build has no compiler database: {build}')
    entry = next(x for x in json.loads(database.read_text()) if x['file'].endswith('/tu_ohos.cc'))
    command = shlex.split(entry['command'])
    roots = [Path(x.split('=', 1)[1]) for x in command if x.startswith('--sysroot=')]
    if (not compiler.samefile(command[0]) or '--target=aarch64-linux-ohos' not in command or
            len(roots) != 1 or not sysroot.samefile(roots[0])):
        raise ValueError('Existing Turnip cache uses a different compiler, target or sysroot')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-root', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--metadata-archive', type=Path, required=True)
    parser.add_argument('--library', action='append', default=[], metavar='NAME=PATH')
    parser.add_argument('--kernel-driver', choices=('kgsl', 'msm'), required=True)
    parser.add_argument('--native-buffer', action='store_true')
    parser.add_argument('--build-type', choices=('debug', 'debugoptimized', 'release'), required=True)
    parser.add_argument('--sysroot', type=Path, required=True)
    parser.add_argument('--llvm', type=Path, required=True)
    parser.add_argument('--python', default='python3')
    parser.add_argument('--ninja', default='ninja')
    parser.add_argument('--glslang', default='glslangValidator')
    parser.add_argument('--build-dir', type=Path)
    parser.add_argument('--jobs', type=int, default=16, choices=range(1, 17))
    args = parser.parse_args()
    root, output = args.source_root.resolve(), args.output.resolve()
    source = root / 'third_party/mesa3d'
    llvm, sysroot = args.llvm.resolve(), args.sysroot.resolve()
    compiler = llvm / 'bin/clang'
    build = args.build_dir.resolve() if args.build_dir else output / 'build'
    python = shutil.which(args.python)
    ninja = shutil.which(args.ninja)
    shader_compiler = shutil.which(args.glslang)
    if not (python and ninja and shader_compiler):
        raise ValueError('Select working host Python/Meson, Ninja and glslangValidator tools')
    # Preserve a virtual environment's executable path while making tool paths
    # independent of Meson's subprocess working directories.
    python, ninja, shader_compiler = [os.path.abspath(x) for x in (python, ninja, shader_compiler)]
    if not (compiler.is_file() and (sysroot / 'usr/include').is_dir()):
        raise ValueError('Select the native LLVM toolchain and built product SDK sysroot')

    libraries = dict(item.split('=', 1) for item in args.library)
    required = {'zlib', 'expat', 'libhilog', 'libhitrace', 'libbegetutil', 'libqos'}
    if args.kernel_driver == 'msm':
        required.add('libdrm')
    if set(libraries) != required:
        raise ValueError(f'Expected exactly these product dependencies: {sorted(required)}')
    headers = {
        'zlib': 'third_party/zlib',
        'expat': 'third_party/expat/expat/lib',
        'libhilog': 'base/hiviewdfx/hilog/interfaces/native/innerkits/include',
        'libhitrace': 'base/hiviewdfx/hitrace/interfaces/native/innerkits/include',
        'libbegetutil': 'base/startup/init/interfaces/innerkits/include',
        'libqos': 'foundation/resourceschedule/qos_manager/interfaces/kits/c',
        'libdrm': 'third_party/libdrm/include/drm',
    }
    pkgconfig = output / 'pkgconfig'
    for name, value in libraries.items():
        library = Path(value).resolve()
        validate_library(library)
        include = root / headers[name]
        if not include.is_dir():
            raise ValueError(f'Missing dependency headers: {include}')
        write_changed(pkgconfig / (name + '.pc'),
                      f'Name: {name}\nDescription: Native product {name}\nVersion: {source_version(root, name)}\n'
                      f'Libs: {shlex.quote(str(library))}\nCflags: -I{shlex.quote(str(include))}\n')
    metadata = args.metadata_archive.resolve()
    validate_library(metadata)
    native_headers = [
        'device/soc/qualcomm/common/include', 'drivers/hdf_core/interfaces/inner_api/hdi/base',
        'interface/sdk_c/graphic/graphic_2d', 'third_party/libdrm/include/drm',
        'drivers/peripheral/display/interfaces/include',
    ]
    write_changed(pkgconfig / 'qcom-native-buffer.pc',
                  'Name: qcom-native-buffer\nDescription: Native managed buffer ownership and layout\nVersion: 1\n'
                  f'Libs: {shlex.quote(str(metadata))}\nCflags: ' +
                  ' '.join('-I' + shlex.quote(str(root / p)) for p in native_headers) + '\n')

    cross = '[binaries]\n'
    for key, value in [('c', compiler), ('cpp', llvm / 'bin/clang++'), ('ar', llvm / 'bin/llvm-ar'),
                       ('strip', llvm / 'bin/llvm-strip'), ('pkg-config', '/usr/bin/pkg-config')]:
        cross += f'{key} = {str(value)!r}\n'
    cross += '\n[properties]\nneeds_exe_wrapper = true\n\n[built-in options]\n'
    native_args = ['--target=aarch64-linux-ohos', '--sysroot=' + str(sysroot), '-fPIC']
    for key in ('c_args', 'cpp_args', 'c_link_args', 'cpp_link_args'):
        flags = native_args + (['-L' + str(llvm / 'lib/aarch64-linux-ohos')] if 'link' in key else [])
        cross += key + ' = ' + repr(flags) + '\n'
    cross += "\n[host_machine]\nsystem = 'linux'\ncpu_family = 'aarch64'\ncpu = 'armv8-a'\nendian = 'little'\n"
    cross_file = output / 'cross.ini'
    write_changed(cross_file, cross)
    host_bin = output / 'host-bin'
    host_bin.mkdir(parents=True, exist_ok=True)
    validator = host_bin / 'glslangValidator'
    if validator.is_symlink() and validator.resolve() != Path(shader_compiler).resolve():
        validator.unlink()
    if not validator.exists():
        validator.symlink_to(Path(shader_compiler).resolve())
    if validator.resolve() != Path(shader_compiler).resolve():
        raise ValueError('Existing host shader compiler differs from the selected tool')
    environment = dict(os.environ, PATH=str(host_bin) + ':' + str(Path(python).parent) + ':' + os.environ['PATH'],
                       PKG_CONFIG_PATH='', PKG_CONFIG_LIBDIR=str(pkgconfig), PKG_CONFIG_SYSROOT_DIR='',
                       PYTHONNOUSERSITE='1', PYTHONDONTWRITEBYTECODE='1', NINJA=ninja)
    command = [python, '-m', 'mesonbuild.mesonmain', 'setup', str(build), str(source), '--wrap-mode=nofallback',
               '-Dplatforms=ohos', '-Dvulkan-drivers=freedreno', '-Dfreedreno-kmds=' + args.kernel_driver,
               '-Dgallium-drivers=', '-Dopengl=false', '-Dgles1=disabled', '-Dgles2=disabled',
               '-Degl=disabled', '-Dglx=disabled', '-Dglvnd=disabled', '-Dbuildtype=' + args.build_type,
               '-Dfreedreno-ohos-native-buffer=' + str(args.native_buffer).lower(),
               '--prefix=/vendor', '--libdir=lib64', '--datadir=etc']
    if (build / 'meson-private/coredata.dat').is_file():
        validate_cache(build, compiler, sysroot)
        command += ['--reconfigure', '--clearcache']
    else:
        command += ['--cross-file=' + str(cross_file)]
    subprocess.run(command, env=environment, check=True)
    driver_name = 'src/freedreno/vulkan/libvulkan_freedreno.so'
    icd_name = 'src/freedreno/vulkan/freedreno_icd.armv8-a.json'
    subprocess.run([ninja, '-C', str(build), '-j' + str(args.jobs), driver_name, icd_name],
                   env=environment, check=True)
    driver = build / driver_name
    validate_library(driver)
    icd = build / icd_name
    if json.loads(icd.read_text())['ICD']['library_path'] != '/vendor/lib64/libvulkan_freedreno.so':
        raise ValueError('Generated ICD does not name the selected vendor driver installation')
    # Meson's install step removes build-directory RPATHs. Copying the linked
    # build target directly would embed paths to the host's product libraries.
    with tempfile.TemporaryDirectory(prefix='install-', dir=output) as staging:
        subprocess.run([python, '-m', 'mesonbuild.mesonmain', 'install', '-C', str(build),
                        '--no-rebuild', '--tags=runtime', '--destdir=' + staging],
                       env=environment, check=True)
        installed = Path(staging) / 'vendor'
        installed_driver = installed / 'lib64/libvulkan_freedreno.so'
        validate_library(installed_driver)
        dynamic = subprocess.check_output([str(llvm / 'bin/llvm-readelf'), '--dynamic',
                                           str(installed_driver)], text=True)
        if '(RPATH)' in dynamic or '(RUNPATH)' in dynamic:
            raise ValueError('Installed driver retains a runtime library search path')
        installed_icd = installed / 'etc/vulkan/icd.d' / Path(icd_name).name
        if json.loads(installed_icd.read_text()) != json.loads(icd.read_text()):
            raise ValueError('Installed ICD differs from the generated driver description')
        for original, filename in [(installed_driver, 'libvulkan_freedreno.so'),
                                   (installed_icd, 'freedreno_icd.json')]:
            original.replace(output / filename)
    driver = output / 'libvulkan_freedreno.so'
    write_changed(output / 'build-result.json', json.dumps({
        'kernel_driver': args.kernel_driver, 'jobs': args.jobs, 'build_type': args.build_type,
        'native_buffer': args.native_buffer,
        'driver_sha256': hashlib.sha256(driver.read_bytes()).hexdigest(),
        'icd_library_path': '/vendor/lib64/libvulkan_freedreno.so',
    }, indent=2) + '\n')


if __name__ == '__main__':
    main()
