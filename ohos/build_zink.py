#!/usr/bin/env python3
# Copyright 2026 OpenDroidOS
# SPDX-License-Identifier: MIT
"""Build and install native OHOS EGL/GLES from explicit product dependencies."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile

from build_turnip import source_version, validate_library, write_changed


def validate_cache(build, compiler, sysroot):
    database = build / 'compile_commands.json'
    if not database.is_file():
        raise ValueError(f'Configured build has no compiler database: {build}')
    entry = next(x for x in json.loads(database.read_text())
                 if x['file'].endswith('/platform_ohos.c'))
    command = shlex.split(entry['command'])
    roots = [Path(x.split('=', 1)[1]) for x in command if x.startswith('--sysroot=')]
    if (not compiler.samefile(command[0]) or '--target=aarch64-linux-ohos' not in command or
            len(roots) != 1 or not sysroot.samefile(roots[0])):
        raise ValueError('Existing EGL cache uses a different compiler, target or sysroot')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('source-root', 'output', 'metadata-archive', 'sysroot', 'llvm'):
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--library', action='append', default=[], metavar='NAME=PATH')
    parser.add_argument('--build-type', choices=('debug', 'debugoptimized', 'release'), required=True)
    parser.add_argument('--python', default='python3')
    parser.add_argument('--ninja', default='ninja')
    parser.add_argument('--build-dir', type=Path)
    parser.add_argument('--jobs', type=int, default=16, choices=range(1, 17))
    args = parser.parse_args()
    root, output = args.source_root.resolve(), args.output.resolve()
    llvm, sysroot = args.llvm.resolve(), args.sysroot.resolve()
    compiler = llvm / 'bin/clang'
    build = args.build_dir.resolve() if args.build_dir else output / 'build'
    python, ninja = shutil.which(args.python), shutil.which(args.ninja)
    if not (python and ninja and compiler.is_file() and (sysroot / 'usr/include').is_dir()):
        raise ValueError('Select native LLVM/product SDK, host Python/Meson and Ninja tools')
    python, ninja = [os.path.abspath(x) for x in (python, ninja)]
    libraries = {}
    for item in args.library:
        name, value = item.split('=', 1)
        if name in libraries:
            raise ValueError(f'Duplicate product dependency: {name}')
        libraries[name] = Path(value).resolve()
    required = {'zlib', 'expat', 'libhilog', 'libhitrace', 'libbegetutil', 'libqos',
                'libdrm', 'libsurface'}
    if set(libraries) != required:
        raise ValueError(f'Expected exactly these product dependencies: {sorted(required)}')
    headers = {
        'zlib': ['third_party/zlib'],
        'expat': ['third_party/expat/expat/lib'],
        'libhilog': ['base/hiviewdfx/hilog/interfaces/native/innerkits/include'],
        'libhitrace': ['base/hiviewdfx/hitrace/interfaces/native/innerkits/include'],
        'libbegetutil': ['base/startup/init/interfaces/innerkits/include'],
        'libqos': ['foundation/resourceschedule/qos_manager/interfaces/kits/c'],
        'libdrm': ['third_party/libdrm/include/drm', 'third_party/libdrm'],
        'libsurface': ['foundation/graphic/graphic_surface/interfaces/inner_api/surface',
                       'foundation/graphic/graphic_surface/interfaces/inner_api/buffer_handle',
                       'foundation/graphic/graphic_surface/interfaces/inner_api/common',
                       'drivers/peripheral/display/interfaces/include',
                       'drivers/interface/display/composer/common/include',
                       'drivers/hdf_core/interfaces/inner_api/hdi/base'],
    }
    pkgconfig = output / 'pkgconfig'
    for name, library in libraries.items():
        validate_library(library)
        includes = [root / p for p in headers[name]]
        if not all(p.is_dir() for p in includes):
            raise ValueError(f'Missing native dependency headers: {name}')
        version = (json.loads((root / 'foundation/graphic/graphic_surface/bundle.json').read_text())['version']
                   if name == 'libsurface' else source_version(root, name))
        write_changed(pkgconfig / (name + '.pc'),
                      f'Name: {name}\nDescription: Native product {name}\nVersion: {version}\n'
                      f'Libs: {shlex.quote(str(library))}\nCflags: ' +
                      ' '.join('-I' + shlex.quote(str(p)) for p in includes) + '\n')
    metadata = args.metadata_archive.resolve()
    validate_library(metadata)
    includes = ['device/soc/qualcomm/common/include', 'drivers/hdf_core/interfaces/inner_api/hdi/base',
                'interface/sdk_c/graphic/graphic_2d', 'third_party/libdrm/include/drm',
                'drivers/peripheral/display/interfaces/include']
    write_changed(pkgconfig / 'qcom-native-buffer.pc',
                  'Name: qcom-native-buffer\nDescription: Native managed buffer ownership and layout\nVersion: 1\n'
                  f'Libs: {shlex.quote(str(metadata))}\nCflags: ' +
                  ' '.join('-I' + shlex.quote(str(root / p)) for p in includes) + '\n')
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
    environment = dict(os.environ, PATH=str(Path(python).parent) + ':' + os.environ['PATH'],
                       PKG_CONFIG_PATH='', PKG_CONFIG_LIBDIR=str(pkgconfig), PKG_CONFIG_SYSROOT_DIR='',
                       PYTHONNOUSERSITE='1', PYTHONDONTWRITEBYTECODE='1', NINJA=ninja)
    command = [python, '-m', 'mesonbuild.mesonmain', 'setup', str(build),
               str(root / 'third_party/mesa3d'), '--wrap-mode=nofallback',
               '-Dplatforms=ohos', '-Degl-native-platform=ohos', '-Dgallium-drivers=zink',
               '-Dvulkan-drivers=', '-Degl=enabled', '-Degl-ohos-native-buffer=true',
               '-Dgles1=enabled', '-Dgles2=enabled', '-Dopengl=true', '-Dglx=disabled',
               '-Dglvnd=disabled', '-Dshared-glapi=enabled', '-Dllvm=disabled', '-Dgbm=disabled',
               '-Dtools=', '-Dbuildtype=' + args.build_type, '-Degl-lib-suffix=_impl',
               '-Dgles-lib-suffix=_impl', '-Dunversion-libgallium=true', '--prefix=/vendor',
               '--libdir=lib64', '--datadir=etc']
    if (build / 'meson-private/coredata.dat').is_file():
        validate_cache(build, compiler, sysroot)
        command += ['--reconfigure']
    else:
        command += ['--cross-file=' + str(cross_file)]
    subprocess.run(command, env=environment, check=True)
    targets = json.loads(subprocess.check_output(
        [python, '-m', 'mesonbuild.mesonmain', 'introspect', '--targets', str(build)],
        env=environment, text=True))
    names = {'EGL_impl', 'GLESv1_CM_impl', 'GLESv2_impl', 'gallium_dri'}
    selected = {t['name']: t for t in targets if t['name'] in names}
    if set(selected) != names:
        raise ValueError('Configured cache does not contain the native EGL/GLES renderer targets')
    outputs = [str(Path(selected[n]['filename'][0]).relative_to(build)) for n in sorted(names)]
    subprocess.run([ninja, '-C', str(build), '-j' + str(args.jobs), *outputs],
                   env=environment, check=True)
    # Native wrappers request these API-specific names. GLES2's actual Mesa
    # client also exports GLES3; both names refer to that genuine implementation.
    installed_names = {'EGL_impl': 'libEGL_impl.so', 'GLESv1_CM_impl': 'libGLESv1_impl.so',
                       'GLESv2_impl': 'libGLESv2_impl.so', 'gallium_dri': 'libgallium_dri.so'}
    with tempfile.TemporaryDirectory(prefix='install-', dir=output) as staging:
        subprocess.run([python, '-m', 'mesonbuild.mesonmain', 'install', '-C', str(build),
                        '--no-rebuild', '--tags=runtime', '--destdir=' + staging],
                       env=environment, check=True)
        for name, filename in installed_names.items():
            installed = Path(staging) / 'vendor/lib64' / Path(selected[name]['filename'][0]).name
            validate_library(installed)
            dynamic = subprocess.check_output([str(llvm / 'bin/llvm-readelf'), '--dynamic',
                                               str(installed)], text=True)
            if '(RPATH)' in dynamic or '(RUNPATH)' in dynamic:
                raise ValueError('Installed EGL/GLES renderer retains a runtime library search path')
            if name == 'GLESv2_impl':
                symbols = subprocess.check_output([str(llvm / 'bin/llvm-readelf'), '--dyn-syms', '--wide',
                                                  str(installed)], text=True)
                if not all(symbol in symbols for symbol in (' glDispatchCompute', ' glTexStorage3D')):
                    raise ValueError('Selected GLES client does not export the GLES3 API')
            installed.replace(output / filename)
        defaults = Path(staging) / 'vendor/etc/drirc.d/00-mesa-defaults.conf'
        defaults.replace(output / '00-mesa-defaults.conf')
    alias = output / 'libGLESv3_impl.so'
    if alias.exists() or alias.is_symlink():
        alias.unlink()
    os.link(output / 'libGLESv2_impl.so', alias)
    files = [*installed_names.values(), alias.name, '00-mesa-defaults.conf']
    write_changed(output / 'build-result.json', json.dumps({
        'jobs': args.jobs, 'build_type': args.build_type,
        'native_buffer': True,
        'files': {f: hashlib.sha256((output / f).read_bytes()).hexdigest() for f in files},
        'gles3_provider': 'Mesa GLES2+3 client; shared inode with GLES2 deployment name',
    }, indent=2) + '\n')


if __name__ == '__main__':
    main()
