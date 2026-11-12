#!/usr/bin/env python3
"""Build the production macOS host with isolated test hooks and restart it.

Requires webapp/dist, cha_macos_runtime, and cha_prepare_test_vault.
Run outside a restricted sandbox: AppKit needs the macOS GUI session.
No provider requests or production application data are used.
"""
import json
import os
from pathlib import Path
import plistlib
import shutil
import select
import subprocess
import sys
import tempfile

REPOSITORY = Path(__file__).resolve().parents[3]
DEFAULTS = dict(split=False, sidebar=True, pictures=False, toolbar=True,
                chat_toolbar=True, notes_prompt=True, russian=False, notes_font_size=14)


def menu_state(state):
    return {key: value for key, value in state.items() if key != 'notes_font_size'}


def main():
    if sys.platform != 'darwin':
        raise SystemExit('This check requires macOS')
    with tempfile.TemporaryDirectory(prefix='cha-persistence-') as temporary:
        root = Path(temporary)
        contents = root / 'CHAPersistenceTest.app/Contents'
        executable = contents / 'MacOS/CHAPersistenceTest'
        executable.parent.mkdir(parents=True)
        (contents / 'Frameworks').mkdir()
        resources = contents / 'Resources'
        resources.mkdir()
        shutil.copytree(REPOSITORY / 'webapp/dist', resources / 'web')
        runtime = REPOSITORY / 'build/ninja/libChaRuntime.dylib'
        shutil.copy2(runtime, contents / 'Frameworks' / runtime.name)
        with (contents / 'Info.plist').open('wb') as file:
            plistlib.dump(dict(CFBundleExecutable=executable.name,
                              CFBundleIdentifier='com.michaelpopov.cha.persistencetest',
                              CFBundleName='CHAPersistenceTest', CFBundlePackageType='APPL',
                              LSUIElement=True), file)
        subprocess.run(['xcrun', 'swiftc', '-swift-version', '5', '-parse-as-library', '-O',
                        '-D', 'CHA_NATIVE_INSTRUMENTATION', '-module-cache-path', str(root / 'modules'),
                        '-framework', 'AppKit', '-framework', 'WebKit',
                        '-import-objc-header', str(REPOSITORY / 'packaging/host/runtime_bridge.h'),
                        '-L', str(runtime.parent), '-lChaRuntime', '-Xlinker', '-rpath',
                        '-Xlinker', '@executable_path/../Frameworks',
                        *[str(REPOSITORY / path) for path in [
                            'packaging/macos/main.swift', 'packaging/macos/feasibility.swift',
                            'packaging/macos/native_bridge.swift', 'tests/native/macos/persistence_probe.swift']],
                        '-o', str(executable)], check=True)
        config = root / 'config'
        subprocess.run([str(REPOSITORY / 'build/ninja/cha_prepare_test_vault'), '--folder-stores', str(config)], check=True)
        preferences = config / 'view.json'

        def launch(mode, expected, final=None):
            env = dict(os.environ, CHA_PERSISTENCE_CONFIG=str(config),
                       CHA_PERSISTENCE_SCRIPT=str(REPOSITORY / 'tests/native/persistence.js'),
                       CHA_PERSISTENCE_MODE=mode, CHA_PERSISTENCE_EXPECT=json.dumps(expected))
            try:
                result = subprocess.run([str(executable)], env=env, capture_output=True, text=True, timeout=30)
            finally:
                config.chmod(0o700)  # Also clean up a failed read-only test.
            assert result.returncode == 0, result.stdout + result.stderr
            assert 'PASS persistence probe' in result.stdout, result.stdout + result.stderr
            reports = [json.loads(line.removeprefix('PERSISTENCE_EXIT '))
                       for line in result.stdout.splitlines() if line.startswith('PERSISTENCE_EXIT ')]
            assert len(reports) == 1, result.stdout + result.stderr
            report = reports[0]
            assert report['state'] == (final or expected), report
            assert report['menu'] == menu_state(report['state']), report
            assert report['startup'] == menu_state(expected), report
            assert report['first'] == (final if mode in ('reload', 'failure-reload') else expected), report
            return result.stderr

        def observe(expected):
            before = preferences.read_bytes() if preferences.exists() else None
            stamp = preferences.stat().st_mtime_ns if preferences.exists() else None
            stderr = launch('observe', expected)
            assert (preferences.read_bytes() if preferences.exists() else None) == before
            assert (preferences.stat().st_mtime_ns if preferences.exists() else None) == stamp
            return stderr

        assert 'cannot read View preferences' not in observe(DEFAULTS)
        assert not preferences.exists(), 'Default startup created preferences'
        inverted = dict(DEFAULTS, **{key: not value for key, value in menu_state(DEFAULTS).items()})
        launch('change', DEFAULTS, inverted)
        assert json.loads(preferences.read_bytes()) == inverted
        observe(inverted)
        launch('change', inverted, DEFAULTS)
        assert json.loads(preferences.read_bytes()) == DEFAULTS
        observe(DEFAULTS)
        print('PASS actual native menu changes, immediate normal quit, and process restarts in both directions', flush=True)

        launch('reload', DEFAULTS, inverted)
        assert json.loads(preferences.read_bytes()) == inverted
        observe(inverted)
        print('PASS document reload retains applied choices without rewriting on startup', flush=True)

        before = preferences.read_bytes()
        launch('invalid', inverted)
        assert preferences.read_bytes() == before
        print('PASS stale, incomplete, and nonboolean native reports are ignored', flush=True)

        preferences.write_text(json.dumps(dict(russian=True, sidebar=0, obsolete=True, notes_font_size=18.5)))
        stderr = observe(dict(DEFAULTS, russian=True))
        assert 'invalid View preference sidebar' in stderr and 'unknown View preference obsolete' in stderr
        assert 'invalid View preference notes_font_size' in stderr
        for contents_text in ['not JSON', '[]', 'null']:
            preferences.write_text(contents_text)
            assert 'cannot read View preferences' in observe(DEFAULTS)
        print('PASS partial, obsolete, and malformed preferences with no startup rewrite', flush=True)

        # The first read happens before the host repairs directory permissions.
        # An inaccessible path must warn, rather than look like a missing file.
        preferences.write_text(json.dumps(inverted))
        before = preferences.read_bytes()
        config.chmod(0o000)
        assert 'cannot read View preferences' in launch('observe', DEFAULTS)
        assert preferences.read_bytes() == before
        print('PASS inaccessible preferences path warns and uses defaults', flush=True)

        preferences.write_text(json.dumps(DEFAULTS))
        before = preferences.read_bytes()
        stderr = launch('failure', DEFAULTS, dict(DEFAULTS, sidebar=False))
        assert 'cannot write View preferences' in stderr
        assert preferences.read_bytes() == before, 'Failed save damaged the previous file'
        observe(DEFAULTS)
        stderr = launch('failure-reload', DEFAULTS, dict(DEFAULTS, sidebar=False))
        assert stderr.count('cannot write View preferences') == 1, 'An equal report retried a failed save'
        assert preferences.read_bytes() == before, 'Reload replaced the previous saved preferences'
        recovered = dict(DEFAULTS, sidebar=False, pictures=True)
        stderr = launch('recovery', DEFAULTS, recovered)
        assert 'cannot write View preferences' in stderr
        assert json.loads(preferences.read_bytes()) == recovered
        observe(recovered)
        print('PASS save failure preserves live choices and disk data; next change saves the full snapshot', flush=True)

        preferences.write_text(json.dumps(DEFAULTS))
        env = dict(os.environ, CHA_PERSISTENCE_CONFIG=str(config),
                   CHA_PERSISTENCE_SCRIPT=str(REPOSITORY / 'tests/native/persistence.js'),
                   CHA_PERSISTENCE_MODE='hold', CHA_PERSISTENCE_EXPECT=json.dumps(DEFAULTS))
        with subprocess.Popen([str(executable)], env=env, stdin=subprocess.PIPE,
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE, bufsize=0) as held:
            try:
                assert select.select([held.stdout], [], [], 20)[0], 'Unchanged instance did not become ready'
                assert held.stdout.readline() == b'PERSISTENCE_READY\n'
                launch('change', DEFAULTS, inverted)
                stamp = preferences.stat().st_mtime_ns
                held.stdin.write(b'\n')
                output, errors = held.communicate(timeout=20)
                assert held.returncode == 0, output + errors
                report = json.loads(next(line.removeprefix('PERSISTENCE_EXIT ')
                    for line in output.decode().splitlines() if line.startswith('PERSISTENCE_EXIT ')))
                assert report['state'] == DEFAULTS and report['menu'] == menu_state(DEFAULTS), report
                assert json.loads(preferences.read_bytes()) == inverted
                assert preferences.stat().st_mtime_ns == stamp, 'Unchanged instance overwrote a newer save'
            finally:
                if held.poll() is None:
                    held.kill()
                    held.wait()
        observe(inverted)
        print('PASS concurrent instances keep live choices; unchanged reports and close preserve the newer save', flush=True)

        preferences.write_text(json.dumps(menu_state(DEFAULTS)))
        larger = dict(DEFAULTS, notes_font_size=16)
        smaller = dict(DEFAULTS, notes_font_size=12)
        launch('increase-font', DEFAULTS, larger)
        assert json.loads(preferences.read_bytes()) == larger
        launch('decrease-font', larger, smaller)
        assert json.loads(preferences.read_bytes()) == smaller
        launch('observe-font', smaller)
        print('PASS Notes font changes are saved and applied after process restarts', flush=True)


if __name__ == '__main__':
    main()
