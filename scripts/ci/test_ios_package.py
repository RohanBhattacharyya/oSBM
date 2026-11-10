from pathlib import Path
import plistlib
import subprocess
import tempfile
import unittest
import zipfile

from test_check_package import checker


class IosPackaging(unittest.TestCase):
    def test_clean_and_repeated_builds(self):
        # Exercise the production CMake target without an Apple compiler or
        # game assets. Spaces also check VERBATIM handling in generated rules.
        with tempfile.TemporaryDirectory(prefix='osbm ipa ') as temporary:
            root = Path(temporary)
            source = root / 'project'
            source.mkdir()
            bundle = source / 'test bundle' / 'OpenStarbound.app'
            bundle.mkdir(parents=True)
            (bundle / 'Info.plist').write_bytes(plistlib.dumps({'CFBundleExecutable': 'OpenStarbound'}))
            executable = bundle / 'OpenStarbound'
            executable.write_bytes(b'code' * 125000)
            executable.chmod(0o755)
            for tree in ('opensb', 'osbm', 'lang'):
                path = bundle / 'assets' / tree
                path.mkdir(parents=True)
                (path / 'test').write_text('asset')
            (bundle / 'old-asset').write_text('old')
            module = Path(__file__).resolve().parents[2] / 'cmake' / 'IosPackaging.cmake'
            (source / 'CMakeLists.txt').write_text(
                'cmake_minimum_required(VERSION 3.23)\n'
                'project(IpaPackaging NONE)\n'
                f'include("{module.as_posix()}")\n'
                'add_custom_target(starbound)\n'
                'star_add_ios_ipa_target(starbound "${CMAKE_CURRENT_SOURCE_DIR}/test bundle/OpenStarbound.app")\n')
            build = root / 'build'
            self.run_command('cmake', '-S', str(source), '-B', str(build), '-G', 'Ninja')
            self.assertFalse((build / 'ipa').exists())
            self.run_command('cmake', '--build', str(build), '--target', 'ipa')
            archive = build / 'ipa' / 'OpenStarbound-iOS-Release-unsigned.ipa'
            checker.check('ipa', archive)
            with zipfile.ZipFile(archive) as result:
                info = result.getinfo('Payload/OpenStarbound.app/OpenStarbound')
                self.assertTrue((info.external_attr >> 16) & 0o111)
                self.assertIn('Payload/OpenStarbound.app/old-asset', result.namelist())

            stale = build / 'ipa' / 'Payload' / 'Stale.app'
            stale.mkdir()
            (stale / 'unused').write_text('stale')
            (bundle / 'old-asset').unlink()
            executable.write_bytes(b'new!' * 125000)
            self.run_command('cmake', '--build', str(build), '--target', 'ipa')
            checker.check('ipa', archive)
            with zipfile.ZipFile(archive) as result:
                self.assertNotIn('Payload/OpenStarbound.app/old-asset', result.namelist())
                self.assertFalse(any('Stale.app' in name for name in result.namelist()))
                self.assertEqual(executable.read_bytes(), result.read('Payload/OpenStarbound.app/OpenStarbound'))

    def run_command(self, *command):
        result = subprocess.run(command, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        self.assertEqual(result.returncode, 0, result.stdout)


if __name__ == '__main__':
    unittest.main()
