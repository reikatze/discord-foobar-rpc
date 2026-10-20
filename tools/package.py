"""Combine verified native CI inputs into one installable component package."""
import argparse
from pathlib import Path
import shutil
import stat
import subprocess
import tarfile
import tempfile
import zipfile

from run import inspect_pe, WINDOWS_DLL, MAC_EXECUTABLE, MAC_BUNDLE

NAME = 'discord-foobar-rpc'


def command(args):
    subprocess.run(list(map(str, args)), check=True)


def package(inputs, destination):
    def input_file(platform, arch, filename):
        path = inputs / f'native-input-{NAME}-{platform}-{arch}-Release' / filename
        if not path.is_file():
            raise RuntimeError(f'Missing native input: {path}')
        return path

    destination.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory() as temporary:
        work = Path(temporary)
        payload = work / 'payload'
        payload.mkdir()
        for arch in ['x86', 'x64']:
            entry = ('x64/' if arch == 'x64' else '') + WINDOWS_DLL
            source = input_file('windows', arch, NAME + '.fb2k-component')
            with zipfile.ZipFile(source) as archive:
                if archive.namelist() != [entry] or archive.testzip() is not None:
                    raise RuntimeError(f'Unexpected Windows package contents: {source}')
                target = payload / entry
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(archive.read(entry))
            inspect_pe(target, arch)

        bundles = []
        for arch in ['x64', 'arm64']:
            source = input_file('macos', arch, MAC_BUNDLE + '.tar.gz')
            root = work / arch
            root.mkdir()
            with tarfile.open(source, 'r:gz') as archive:
                for member in archive.getmembers():
                    path = Path(member.name)
                    if (path.is_absolute() or '..' in path.parts
                            or not path.parts or path.parts[0] != MAC_BUNDLE
                            or not (member.isfile() or member.isdir())):
                        raise RuntimeError('Unexpected macOS bundle archive entry')
                archive.extractall(root, filter='data')
            bundle = root / (MAC_BUNDLE)
            if not (bundle / 'Contents/MacOS' / MAC_EXECUTABLE).is_file():
                raise RuntimeError(f'Missing macOS executable: {source}')
            bundles.append(bundle)

        bundle = payload / 'mac' / (MAC_BUNDLE)
        shutil.copytree(bundles[0], bundle)
        binary = bundle / 'Contents/MacOS' / MAC_EXECUTABLE
        # Changing Mach-O slices invalidates the old bundle signature.
        signature = bundle / 'Contents/_CodeSignature'
        if signature.exists():
            shutil.rmtree(signature)
        command(['lipo', '-create',
                 bundles[0] / 'Contents/MacOS' / MAC_EXECUTABLE,
                 bundles[1] / 'Contents/MacOS' / MAC_EXECUTABLE,
                 '-output', binary])
        binary.chmod(0o755)
        command(['lipo', binary, '-verify_arch', 'x86_64', 'arm64'])
        command(['codesign', '--force', '--sign', '-', bundle])
        command(['codesign', '--verify', '--strict', bundle])

        staged = work / (NAME + '.fb2k-component')
        with zipfile.ZipFile(staged, 'w', compression=zipfile.ZIP_DEFLATED) as archive:
            for file in sorted(payload.rglob('*')):
                if file.is_file():
                    entry = zipfile.ZipInfo.from_file(file, file.relative_to(payload).as_posix())
                    if file == binary:
                        # Windows chmod/stat cannot represent Unix executable bits.
                        entry.create_system = 3
                        entry.external_attr = (stat.S_IFREG | 0o755) << 16
                    archive.writestr(entry, file.read_bytes(), compress_type=zipfile.ZIP_DEFLATED)
        with zipfile.ZipFile(staged) as archive:
            if archive.testzip() is not None:
                raise RuntimeError('Component package integrity check failed')
            mode = archive.getinfo('mac/' + MAC_BUNDLE + '/Contents/MacOS/' + MAC_EXECUTABLE).external_attr >> 16
            if not stat.S_IMODE(mode) & 0o111:
                raise RuntimeError('Packaged macOS executable lost its executable mode')
        shutil.copy2(staged, destination)
    return destination


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--inputs', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    package(args.inputs.resolve(), args.output.resolve())