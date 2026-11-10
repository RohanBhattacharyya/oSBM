#!/usr/bin/env python3
"""Check distributable assets without extracting archives or needing game assets."""
import argparse
from pathlib import Path, PurePosixPath
import plistlib
import zipfile


def check_names(names):
    for name in names:
        path = PurePosixPath(name)
        if path.is_absolute() or '..' in path.parts or '\\' in name:
            raise ValueError(f'Unsafe package path: {name}')
        if path.name.lower() == 'packed.pak':
            raise ValueError(f'User-owned game archive in package: {name}')


def require_tree(names, prefix):
    if not any(name.startswith(prefix) and not name.endswith('/') for name in names):
        raise ValueError(f'Missing bundled assets: {prefix}')


def check(kind, path):
    path = Path(path)
    if kind in ('desktop', 'switch'):
        names = [p.relative_to(path).as_posix() for p in path.rglob('*') if p.is_file()]
        check_names(names)
        prefix = 'assets/' if kind == 'desktop' else 'bundled_assets/'
        for tree in ('opensb/', 'osbm/'):
            require_tree(names, prefix + tree)
        if kind == 'switch':
            require_tree(names, prefix + 'lang/')
        if kind == 'desktop' and not any((path / name).is_file() and (path / name).stat().st_size
                                         for name in ('starbound', 'starbound.exe')):
            raise ValueError('Missing desktop executable')
        return
    with zipfile.ZipFile(path) as archive:
        names = archive.namelist()
        check_names(names)
        if kind == 'apk':
            for tree in ('assets/opensb/', 'assets/osbm/', 'assets/lang/'):
                require_tree(names, tree)
            abis = {PurePosixPath(n).parts[1] for n in names
                    if n.startswith('lib/') and n.endswith('/libmain.so') and archive.getinfo(n).file_size}
            supported = {'arm64-v8a', 'armeabi-v7a', 'x86_64'}
            if not abis or not abis <= supported:
                raise ValueError(f'Invalid native ABI contents: {abis}')
            if '-universal' in path.name:
                expected = supported
            else:
                expected = {abi for abi in supported if f'-{abi}' in path.name}
            if not expected or abis != expected:
                raise ValueError(f'APK name and native ABIs disagree: {path.name}: {abis}')
        else:
            plists = [n for n in names if n.startswith('Payload/') and n.endswith('.app/Info.plist')]
            if len(plists) != 1:
                raise ValueError('Expected exactly one IPA application')
            prefix = plists[0][:-len('Info.plist')]
            info = plistlib.loads(archive.read(plists[0]))
            executable = prefix + info['CFBundleExecutable']
            if archive.getinfo(executable).file_size < 500000:
                raise ValueError('IPA executable is unexpectedly small')
            for tree in ('opensb/', 'osbm/', 'lang/'):
                require_tree(names, prefix + 'assets/' + tree)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('kind', choices=('apk', 'ipa', 'desktop', 'switch'))
    parser.add_argument('paths', nargs='+')
    args = parser.parse_args()
    for package in args.paths:
        check(args.kind, package)
        print(f'PASS {args.kind}: {package}')
