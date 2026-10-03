"""Exercise release transfers and replacement with a local fake R2 bucket."""
import hashlib
import os
from pathlib import Path
import plistlib
import shutil
import subprocess
import sys
import tarfile
import tempfile
import unittest


SCRIPT = Path(__file__).resolve().parents[3] / "packaging/macos/release.sh"
MOCK = r'''
import hashlib, os, pathlib, shutil, sys
name = pathlib.Path(sys.argv[0]).name
args = sys.argv[1:]
if name == 'uname':
    print('Darwin' if args == ['-s'] else 'arm64')
elif name == 'sw_vers':
    print('26.0')
elif name == 'pgrep':
    sys.exit(1)
elif name == 'codesign':
    if os.environ.get('FAIL_SIGNATURE'):
        sys.exit(1)
elif name == 'open':
    pass
elif name == 'ditto':
    shutil.copytree(*args, symlinks=True)
elif name == 'mv':
    if os.environ.get('FAIL_INSTALL') and '/.CHA-update.' in args[0]:
        sys.exit(1)
    os.rename(*args)
elif name == 'curl':
    if args == ['--help', 'all']:
        print('--aws-sigv4')
        sys.exit(0)
    key = args[-1].split('/bucket/')[1]
    path = pathlib.Path(os.environ['FAKE_BUCKET']) / key
    if '--upload-file' in args:
        if os.environ.get('FAIL_UPLOAD') and key.endswith('.tar.gz'):
            sys.exit(22)
        source = pathlib.Path(args[args.index('--upload-file') + 1])
        digest = hashlib.sha256(source.read_bytes()).hexdigest()
        assert 'x-amz-content-sha256: ' + digest in args
        path.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, path)
    else:
        shutil.copyfile(path, args[args.index('--output') + 1])
else:
    raise AssertionError(name)
'''


@unittest.skipUnless(sys.platform == "darwin", "uses macOS PlistBuddy")
class ReleaseTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.bucket = self.root / "bucket"
        self.installs = self.root / "Applications"
        self.installs.mkdir()
        binaries = self.root / "bin"
        binaries.mkdir()
        for command in ("uname", "sw_vers", "pgrep", "codesign", "open", "ditto", "mv", "curl"):
            mock = binaries / command
            mock.write_text(f"#!{sys.executable}\n" + MOCK)
            mock.chmod(0o700)
        config = self.root / "config"
        config.write_text("CHA_RELEASE_BUCKET_URL='https://example.com/bucket'\n"
                          "CHA_RELEASE_ACCESS_KEY_ID='0123456789abcdef0123456789abcdef'\n"
                          "CHA_RELEASE_SECRET_KEY='test'\n")
        self.env = dict(os.environ, PATH=f"{binaries}:{os.environ['PATH']}",
                        CHA_RELEASE_CONFIG=str(config), FAKE_BUCKET=str(self.bucket),
                        CHA_INSTALL_DIR=str(self.installs))
        self.archive = self.root / "CHA-macos-1.0.tar.gz"
        contents = self.root / "CHA.app/Contents"
        contents.mkdir(parents=True)
        with (contents / "Info.plist").open("wb") as file:
            plistlib.dump({"CFBundleIdentifier": "com.michaelpopov.cha",
                          "CFBundleVersion": "1.0", "LSMinimumSystemVersion": "13.3"}, file)
        with tarfile.open(self.archive, "w:gz") as file:
            file.add(contents.parent, arcname="CHA.app")

    def run_script(self, *args, success=True, **env):
        result = subprocess.run(["sh", str(SCRIPT), *args], env=dict(self.env, **env),
                                capture_output=True, text=True)
        self.assertEqual(result.returncode == 0, success, result.stdout + result.stderr)
        return result

    def publish(self):
        self.run_script("upload", "1.0", str(self.archive))

    def old_app(self):
        app = self.installs / "CHA.app"
        app.mkdir()
        (app / "old").write_text("previous app")
        return app

    def test_upload_and_install(self):
        self.run_script("check")
        self.publish()
        latest = (self.bucket / "packages/macos/latest.txt").read_text().splitlines()
        self.assertEqual(latest, ["1.0", hashlib.sha256(self.archive.read_bytes()).hexdigest(), "13.3"])
        self.old_app()
        self.run_script()
        self.assertTrue((self.installs / "CHA.app/Contents/Info.plist").is_file())
        self.assertEqual(list(self.installs.iterdir()), [self.installs / "CHA.app"])

    def test_short_access_key_fails_before_transfer(self):
        config = self.root / "config"
        config.write_text(config.read_text().replace(
            "0123456789abcdef0123456789abcdef", "0123456789abcdef0123456789abcde"))
        result = self.run_script("check", success=False)
        self.assertIn("must be 32 characters (found 31)", result.stderr)
        self.assertFalse(self.bucket.exists())

    def test_failed_upload_preserves_latest(self):
        self.publish()
        latest = self.bucket / "packages/macos/latest.txt"
        original = latest.read_bytes()
        self.run_script("upload", "1.0", str(self.archive), success=False, FAIL_UPLOAD="1")
        self.assertEqual(latest.read_bytes(), original)

    def test_corrupt_download_preserves_app(self):
        self.publish()
        next(self.bucket.rglob("*.tar.gz")).write_bytes(b"corrupt")
        app = self.old_app()
        result = self.run_script(success=False)
        self.assertIn("checksum mismatch", result.stderr)
        self.assertTrue((app / "old").is_file())

    def test_newer_macos_preserves_app(self):
        self.publish()
        latest = self.bucket / "packages/macos/latest.txt"
        latest.write_text(latest.read_text().replace("13.3", "99.0"))
        app = self.old_app()
        result = self.run_script(success=False)
        self.assertIn("requires macOS 99.0", result.stderr)
        self.assertTrue((app / "old").is_file())

    def test_failed_replacement_restores_app(self):
        self.publish()
        app = self.old_app()
        self.run_script(success=False, FAIL_INSTALL="1")
        self.assertTrue((app / "old").is_file())
        self.assertEqual(list(self.installs.iterdir()), [app])

    def test_failed_signature_preserves_app(self):
        self.publish()
        app = self.old_app()
        self.run_script(success=False, FAIL_SIGNATURE="1")
        self.assertTrue((app / "old").is_file())

    def test_archive_parent_path_preserves_app(self):
        import io
        with tarfile.open(self.archive, "w:gz") as file:
            file.add(self.root / "CHA.app", arcname="CHA.app")
            info = tarfile.TarInfo("CHA.app/../outside")
            info.size = 3
            file.addfile(info, io.BytesIO(b"bad"))
        self.publish()
        app = self.old_app()
        result = self.run_script(success=False)
        self.assertIn("parent path", result.stderr)
        self.assertTrue((app / "old").is_file())


if __name__ == "__main__":
    unittest.main()
