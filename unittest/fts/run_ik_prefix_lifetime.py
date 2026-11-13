"""Run the native IK prefix lifetime regression against a configured Makefiles build.

Example:
  python3 unittest/fts/run_ik_prefix_lifetime.py build_release
  python3 unittest/fts/run_ik_prefix_lifetime.py build_release --compare-ref 881991c
  python3 unittest/fts/run_ik_prefix_lifetime.py build_release \
    --test-source unittest/fts/test_word_frequency_semantics.cpp

The optional comparison compiles the old CJK walker under a different class name
and requires it to fail with poisoned retired nodes. It never starts a server.
"""
import argparse
import resource
import shlex
import signal
import subprocess
import tempfile
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('build_dir', type=Path)
    parser.add_argument('--compare-ref', help='Git revision with the pre-fix CJK walker')
    parser.add_argument('--test-source', type=Path,
                        help='Run another standalone FTS test with the same build libraries')
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[2]
    if args.test_source and args.compare_ref:
        parser.error('--compare-ref is only supported by the IK lifetime test')
    test_source = (args.test_source.resolve() if args.test_source else
                   repo / 'unittest/fts/test_ik_prefix_lifetime.cpp')
    build = args.build_dir.resolve()
    flags_file = build / 'src/storage/CMakeFiles/ob_storage.dir/flags.make'
    link_file = build / 'src/observer/CMakeFiles/seekdb.dir/link.txt'
    if not flags_file.is_file() or not link_file.is_file():
        parser.error('Build seekdb with the CMake Unix Makefiles generator first')
    flags = {}
    for line in flags_file.read_text().splitlines():
        if ' = ' in line:
            key, value = line.split(' = ', 1)
            flags[key] = shlex.split(value)
    options = flags['CXX_DEFINES'] + flags['CXX_INCLUDES'] + flags['CXX_FLAGS']
    original_link = shlex.split(link_file.read_text())
    compiler = original_link[0]
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    with tempfile.TemporaryDirectory(prefix='ik-prefix-test-') as folder:
        scratch = Path(folder)
        for legacy in ([False, True] if args.compare_ref else [False]):
            name = 'legacy' if legacy else 'current'
            objects = []
            if legacy:
                old_source = scratch / 'legacy_cjk.cpp'
                old_source.write_bytes(subprocess.check_output(
                    ['git', 'show', f'{args.compare_ref}:src/storage/fts/ik/ob_ik_cjk_processor.cpp'],
                    cwd=repo))
                old_object = scratch / 'legacy_cjk.o'
                subprocess.run([compiler, *options, '-DObIKCJKProcessor=LegacyIKCJKProcessor',
                                '-c', str(old_source), '-o', str(old_object)], check=True)
                objects.append(str(old_object))
            test_object = scratch / f'{name}.o'
            subprocess.run([compiler, *options,
                            *(['-DIK_PREFIX_TEST_LEGACY'] if legacy else []), '-c',
                            str(test_source),
                            '-o', str(test_object)], check=True)
            objects.append(str(test_object))
            link = [item for item in original_link if not item.endswith((
                'CMakeFiles/ob_main.dir/main.cpp.o',
                'CMakeFiles/ob_main.dir/ob_command_line_parser.cpp.o'))]
            link[1:1] = objects
            binary = scratch / name
            link[link.index('-o') + 1] = str(binary)
            subprocess.run(link, cwd=build / 'src/observer', check=True)
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=30)
            print(result.stdout, end='')
            print(result.stderr, end='')
            print(f'{name} exit: {result.returncode}', flush=True)
            expected = {-signal.SIGSEGV, -signal.SIGBUS} if legacy else {0}
            if result.returncode not in expected:
                raise AssertionError(f'{name}: unexpected exit {result.returncode}')


if __name__ == '__main__':
    main()
