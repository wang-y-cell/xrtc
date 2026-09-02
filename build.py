#!/usr/bin/env python3
"""构建 / 编译 / 运行 webrtc_test（MSVC 2022 + Ninja + CMake）。"""

from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent
TARGET_NAME = 'webrtc_test'
SDK_TARGET = 'xrtc'
GENERATOR = 'Ninja'


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description='构建 xrtc SDK 或 Qt Demo（固定 MSVC 2022 + Ninja）'
    )
    parser.add_argument(
        '--demo',
        action='store_true',
        help='构建 Qt Demo（webrtc_test.exe）',
    )
    parser.add_argument(
        '--qt_lib',
        type=str,
        default=None,
        help='Qt6 安装路径或 lib 目录（--demo 时必填），'
             '例: F:/Qt/6.8.3/msvc2022_64',
    )
    parser.add_argument(
        '--boost_path',
        type=str,
        default=None,
        help='Boost 安装前缀（首次配置必填），例: F:/wy/boost_install',
    )
    parser.add_argument(
        '--run', '-r',
        action='store_true',
        help='构建后运行 Demo（需配合 --demo）',
    )
    parser.add_argument(
        '--clean', '-c',
        action='store_true',
        help='清理构建目录；单独使用则只清理，与 --demo 联用则清理后重编',
    )
    parser.add_argument(
        '--job', '-j',
        type=int,
        default=os.cpu_count() or 1,
        help='并行编译线程数',
    )
    parser.add_argument(
        '--test',
        action='store_true',
        help='构建并运行 xrtc_tests（GoogleTest）',
    )
    parser.add_argument(
        '--config',
        default='debug',
        choices=['debug', 'release'],
        help='构建配置（默认 debug；会链接 webrtc/lib_debug 或 lib_release）',
    )
    return parser.parse_args()


def resolve_qt_prefix(qt: str | None) -> Path | None:
    """将 Qt 路径规范为安装前缀（若指向 lib 则上一级）。"""
    if not qt:
        return None
    path = Path(qt).expanduser().resolve()
    if path.name.lower() == 'lib':
        path = path.parent
    if not path.is_dir():
        raise FileNotFoundError(f'Qt 路径不存在: {path}')
    return path


def resolve_boost_prefix(boost: str | None) -> Path | None:
    """校验 Boost 安装前缀路径。"""
    if not boost:
        return None
    path = Path(boost).expanduser().resolve()
    if not path.is_dir():
        raise FileNotFoundError(f'Boost 路径不存在: {path}')
    return path


def build_dir_for(config: str) -> Path:
    return ROOT / 'build' / config.capitalize()


def env_path(env: dict[str, str]) -> str:
    for key in ('PATH', 'Path', 'path'):
        if key in env:
            return env[key]
    return ''


def find_vcvarsall() -> Path | None:
    vswhere = (
        Path(os.environ.get('ProgramFiles(x86)', r'C:\Program Files (x86)'))
        / 'Microsoft Visual Studio'
        / 'Installer'
        / 'vswhere.exe'
    )
    if not vswhere.is_file():
        return None
    result = subprocess.run(
        [
            str(vswhere),
            '-latest',
            '-products', '*',
            '-requires', 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64',
            '-property', 'installationPath',
        ],
        capture_output=True,
        text=True,
        check=False,
    )
    install = (result.stdout or '').strip()
    if not install:
        return None
    vcvars = Path(install) / 'VC' / 'Auxiliary' / 'Build' / 'vcvarsall.bat'
    return vcvars if vcvars.is_file() else None


def load_msvc_env(base: dict[str, str] | None = None) -> dict[str, str]:
    env = dict(base or os.environ)
    if shutil.which('cl', path=env_path(env)):
        return env

    vcvars = find_vcvarsall()
    if vcvars is None:
        raise RuntimeError(
            '未找到 MSVC 环境（cl.exe / vcvarsall.bat）。'
            '请安装 VS 2022 C++ 工作负载，或在 x64 Native Tools 中运行。'
        )

    cmd = f'cmd /u /c "call \"{vcvars}\" x64 >nul && set"'
    result = subprocess.run(cmd, capture_output=True, check=False)
    if result.returncode != 0:
        err = result.stderr.decode('utf-16le', errors='replace')
        raise RuntimeError(f'加载 MSVC 环境失败:\n{err}')

    text = result.stdout.decode('utf-16le', errors='replace')
    for line in text.splitlines():
        if '=' not in line:
            continue
        key, value = line.split('=', 1)
        env[key] = value
        if key.lower() == 'path':
            env['PATH'] = value
            env['Path'] = value

    if not shutil.which('cl', path=env_path(env)):
        raise RuntimeError('已执行 vcvarsall.bat，但仍找不到 cl.exe')
    return env


def run_cmd(
    cmd: list[str],
    *,
    cwd: Path | None = None,
    env: dict[str, str] | None = None,
) -> None:
    printable = ' '.join(f'"{c}"' if ' ' in c else c for c in cmd)
    print(f'+ {printable}')
    subprocess.run(cmd, cwd=cwd, env=env, check=True)


def cmake_configured(build_dir: Path) -> bool:
    return (build_dir / 'CMakeCache.txt').is_file()


def read_cmake_cache(build_dir: Path, key: str) -> str | None:
    cache = build_dir / 'CMakeCache.txt'
    if not cache.is_file():
        return None
    pattern = re.compile(rf'^{re.escape(key)}:(?:[^\n=]+)=(.*)$')
    for line in cache.read_text(encoding='utf-8', errors='replace').splitlines():
        match = pattern.match(line)
        if match:
            return match.group(1)
    return None


def resolve_qt_prefix_from_cache(build_dir: Path) -> Path | None:
    raw = read_cmake_cache(build_dir, 'QT6_ROOT')
    if not raw:
        return None
    path = Path(raw)
    if path.name.lower() == 'lib':
        path = path.parent
    return path if path.is_dir() else None


def resolve_boost_prefix_from_cache(build_dir: Path) -> Path | None:
    raw = read_cmake_cache(build_dir, 'Boost_ROOT')
    if not raw:
        return None
    path = Path(raw)
    return path if path.is_dir() else None


def find_ninja() -> Path | None:
    candidates: list[Path] = []
    which = shutil.which('ninja')
    if which:
        candidates.append(Path(which))
    candidates.extend(
        [
            Path(r'F:\ninja\ninja.exe'),
            Path(os.environ.get('ProgramFiles', r'C:\Program Files')) / 'Ninja' / 'ninja.exe',
        ]
    )
    seen: set[str] = set()
    for path in candidates:
        key = str(path).lower()
        if key in seen:
            continue
        seen.add(key)
        exe = path
        if exe.suffix.lower() == '.bat':
            sibling = exe.with_suffix('.exe')
            if sibling.is_file():
                exe = sibling
            else:
                continue
        if not exe.is_file():
            continue
        try:
            result = subprocess.run(
                [str(exe), '--version'],
                capture_output=True,
                check=False,
            )
            if result.returncode == 0:
                return exe
        except OSError:
            continue
    return None


def prepare_build_env(env: dict[str, str]) -> dict[str, str]:
    env = load_msvc_env(env)
    ninja = find_ninja()
    if ninja is None:
        raise RuntimeError('未找到可用的 ninja.exe，请安装 Ninja 并加入 PATH。')
    path = str(ninja.parent) + os.pathsep + env_path(env)
    env['PATH'] = path
    env['Path'] = path
    return env


def configure(
    build_dir: Path,
    *,
    config: str,
    demo: bool,
    tests: bool,
    qt_prefix: Path | None,
    boost_prefix: Path,
    env: dict[str, str],
) -> None:
    build_dir.mkdir(parents=True, exist_ok=True)
    ninja = find_ninja()
    if ninja is None:
        raise RuntimeError('未找到可用的 ninja.exe')

    cmd = [
        'cmake',
        '-S', str(ROOT),
        '-B', str(build_dir),
        '-G', GENERATOR,
        f'-DCMAKE_BUILD_TYPE={config.capitalize()}',
        f'-DCMAKE_MAKE_PROGRAM={ninja.as_posix()}',
        f'-DBUILD_XRTC_DEMO={"ON" if demo else "OFF"}',
        f'-DBUILD_XRTC_TESTS=ON',
        f'-DBoost_ROOT={boost_prefix.as_posix()}',
    ]
    if demo:
        if qt_prefix is None:
            raise RuntimeError(
                '--demo 需要 --qt_lib 指定 Qt 路径，'
                '例: --qt_lib=F:/Qt/6.8.3/msvc2022_64'
            )
        cmd.append(f'-DQT6_ROOT={qt_prefix.as_posix()}')

    run_cmd(cmd, env=env)


def sync_cmake_options(
    build_dir: Path,
    *,
    demo: bool,
    tests: bool,
    qt_prefix: Path | None,
    boost_prefix: Path | None,
    env: dict[str, str],
) -> None:
    if boost_prefix is None:
        boost_prefix = resolve_boost_prefix_from_cache(build_dir)
    if boost_prefix is None:
        raise RuntimeError(
            '需要 --boost_path 指定 Boost 路径，'
            '例: --boost_path=F:/wy/boost_install'
        )

    cmd = [
        'cmake',
        '-S', str(ROOT),
        '-B', str(build_dir),
        f'-DBUILD_XRTC_DEMO={"ON" if demo else "OFF"}',
        f'-DBUILD_XRTC_TESTS=ON',
        f'-DBoost_ROOT={boost_prefix.as_posix()}',
    ]
    if demo:
        if qt_prefix is None:
            cached = resolve_qt_prefix_from_cache(build_dir)
            if cached is None:
                raise RuntimeError(
                    '--demo 需要 --qt_lib 指定 Qt 路径，'
                    '例: --qt_lib=F:/Qt/6.8.3/msvc2022_64'
                )
            qt_prefix = cached
        cmd.append(f'-DQT6_ROOT={qt_prefix.as_posix()}')
    run_cmd(cmd, env=env)


def build(
    build_dir: Path,
    *,
    config: str,
    job: int,
    demo: bool,
    tests: bool,
    env: dict[str, str],
) -> None:
    targets: list[str] = []
    if demo:
        targets.append(TARGET_NAME)
    else:
        targets.append(SDK_TARGET)
    if tests:
        targets.append('xrtc_tests')

    for target in targets:
        run_cmd(
            [
                'cmake',
                '--build', str(build_dir),
                '--config', config.capitalize(),
                '--parallel', str(job),
                '--target', target,
            ],
            env=env,
        )


def run_tests(build_dir: Path, *, config: str, env: dict[str, str]) -> None:
    exe_name = 'xrtc_tests.exe' if sys.platform == 'win32' else 'xrtc_tests'
    exe = build_dir / exe_name
    if not exe.is_file():
        exe = build_dir / config.capitalize() / exe_name
    if not exe.is_file():
        raise FileNotFoundError(f'未找到测试可执行文件: {exe_name}')
    run_cmd([str(exe)], cwd=exe.parent, env=env)


def find_executable(build_dir: Path, config: str) -> Path:
    name = f'{TARGET_NAME}.exe' if sys.platform == 'win32' else TARGET_NAME
    candidates = [
        build_dir / name,
        build_dir / config.capitalize() / name,
        build_dir / 'bin' / name,
        build_dir / config.capitalize() / 'bin' / name,
    ]
    for path in candidates:
        if path.is_file():
            return path
    raise FileNotFoundError(
        f'未找到可执行文件 {name}，已搜索:\n'
        + '\n'.join(f'  - {p}' for p in candidates)
    )


def run_app(exe: Path, *, qt_prefix: Path | None, env: dict[str, str]) -> None:
    run_env = dict(env)
    if qt_prefix is not None:
        qt_bin = str(qt_prefix / 'bin')
        path = qt_bin + os.pathsep + env_path(run_env)
        run_env['Path'] = path
        run_env['PATH'] = path
    run_cmd([str(exe)], cwd=exe.parent, env=run_env)


def clean_build(build_dir: Path) -> None:
    if build_dir.exists():
        print(f'清理: {build_dir}')
        shutil.rmtree(build_dir)
    else:
        print(f'无需清理（不存在）: {build_dir}')


def build_target(
    *,
    config: str,
    job: int,
    demo: bool,
    tests: bool,
    qt_lib: str | None,
    boost_path: str | None,
    clean: bool,
    run: bool,
) -> None:
    if run and not demo:
        raise RuntimeError('--run 需要配合 --demo 使用')

    build_dir = build_dir_for(config)
    qt_prefix = resolve_qt_prefix(qt_lib)
    boost_prefix = resolve_boost_prefix(boost_path)

    if clean:
        clean_build(build_dir)

    env = prepare_build_env(os.environ.copy())

    if not cmake_configured(build_dir):
        if boost_prefix is None:
            raise RuntimeError(
                '首次配置需要 --boost_path 指定 Boost 路径，'
                '例: --boost_path=F:/wy/boost_install'
            )
        if demo and qt_prefix is None:
            raise RuntimeError(
                '--demo 需要 --qt_lib 指定 Qt 路径，'
                '例: --qt_lib=F:/Qt/6.8.3/msvc2022_64'
            )
        configure(
            build_dir,
            config=config,
            demo=demo,
            tests=tests,
            qt_prefix=qt_prefix,
            boost_prefix=boost_prefix,
            env=env,
        )
    else:
        sync_cmake_options(
            build_dir,
            demo=demo,
            tests=tests,
            qt_prefix=qt_prefix,
            boost_prefix=boost_prefix,
            env=env,
        )

    build(build_dir, config=config, job=job, demo=demo, tests=tests, env=env)

    if tests:
        print('运行单元测试: xrtc_tests')
        run_tests(build_dir, config=config, env=env)

    if run:
        exe = find_executable(build_dir, config)
        if qt_prefix is None:
            qt_prefix = resolve_qt_prefix_from_cache(build_dir)
        print(f'运行: {exe}')
        run_app(exe, qt_prefix=qt_prefix, env=env)


def main() -> int:
    args = parse_args()
    try:
        # 仅 -c：只清理，不编译
        if args.clean and not args.demo and not args.run and not args.test:
            clean_build(build_dir_for(args.config))
            return 0

        build_target(
            config=args.config,
            job=args.job,
            demo=args.demo,
            tests=args.test,
            qt_lib=args.qt_lib,
            boost_path=args.boost_path,
            clean=args.clean,
            run=args.run,
        )
    except subprocess.CalledProcessError as e:
        print(f'error: 命令失败 (exit {e.returncode}): {e.cmd}', file=sys.stderr)
        return e.returncode or 1
    except Exception as e:
        print(f'error: {e}', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
