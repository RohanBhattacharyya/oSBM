import importlib.util
from pathlib import Path
import plistlib
import tempfile
import unittest
import zipfile

spec = importlib.util.spec_from_file_location('check_package', Path(__file__).with_name('check-package.py'))
checker = importlib.util.module_from_spec(spec)
spec.loader.exec_module(checker)


class PackageChecks(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)

    def archive(self, name, files):
        path = self.root / name
        with zipfile.ZipFile(path, 'w') as archive:
            for filename, contents in files.items():
                archive.writestr(filename, contents)
        return path

    def apk_files(self, abis):
        files = {f'lib/{abi}/libmain.so': b'native code' for abi in abis}
        files.update({f'assets/{tree}/test': b'asset' for tree in ('opensb', 'osbm', 'lang')})
        return files

    def test_universal_requires_all_abis(self):
        name = 'oSBM-1.1-android-universal.apk'
        files = self.apk_files(('arm64-v8a', 'armeabi-v7a', 'x86_64'))
        checker.check('apk', self.archive(name, files))
        del files['lib/armeabi-v7a/libmain.so']
        with self.assertRaisesRegex(ValueError, 'ABIs disagree'):
            checker.check('apk', self.archive(name, files))

    def test_apk_rejects_game_assets_and_wrong_abi(self):
        files = self.apk_files(('arm64-v8a',))
        path = self.archive('oSBM-1.1-android-arm64-v8a.apk', files)
        checker.check('apk', path)
        files['assets/packed.pak'] = b'user owned'
        with self.assertRaisesRegex(ValueError, 'User-owned'):
            checker.check('apk', self.archive(path.name, files))
        del files['assets/packed.pak']
        with self.assertRaisesRegex(ValueError, 'ABIs disagree'):
            checker.check('apk', self.archive('oSBM-1.1-android-x86_64.apk', files))

    def test_ipa_bundle_and_executable(self):
        prefix = 'Payload/OpenStarbound.app/'
        files = {prefix + 'Info.plist': plistlib.dumps({'CFBundleExecutable': 'OpenStarbound'}, fmt=plistlib.FMT_BINARY),
                 prefix + 'OpenStarbound': b'code' * 125000}
        files.update({prefix + f'assets/{tree}/test': b'asset' for tree in ('opensb', 'osbm', 'lang')})
        checker.check('ipa', self.archive('test.ipa', files))
        files[prefix + 'OpenStarbound'] = b''
        with self.assertRaisesRegex(ValueError, 'unexpectedly small'):
            checker.check('ipa', self.archive('test.ipa', files))
        files[prefix + 'OpenStarbound'] = b'code' * 125000
        del files[prefix + 'assets/osbm/test']
        with self.assertRaisesRegex(ValueError, 'Missing bundled'):
            checker.check('ipa', self.archive('test.ipa', files))

    def test_desktop_and_switch_contents(self):
        for tree in ('opensb', 'osbm'):
            directory = self.root / 'assets' / tree
            directory.mkdir(parents=True)
            (directory / 'test').write_bytes(b'asset')
        (self.root / 'starbound').write_bytes(b'code')
        checker.check('desktop', self.root)
        (self.root / 'assets' / 'PACKED.PAK').write_bytes(b'user owned')
        with self.assertRaisesRegex(ValueError, 'User-owned'):
            checker.check('desktop', self.root)
        (self.root / 'assets' / 'PACKED.PAK').unlink()
        (self.root / 'assets').rename(self.root / 'bundled_assets')
        (self.root / 'bundled_assets' / 'lang').mkdir()
        (self.root / 'bundled_assets' / 'lang' / 'en_US.lang').write_text('key=value')
        checker.check('switch', self.root)

    def test_unsafe_paths(self):
        for name in ('../packed.pak', '/absolute', 'assets\\packed.pak'):
            with self.assertRaises(ValueError):
                checker.check_names([name])


if __name__ == '__main__':
    unittest.main()
