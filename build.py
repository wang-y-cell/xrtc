#!/usr/bin/env python3
"""构建 / 编译 / 运行 webrtc_test（Qt6 + CMake）。"""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent
TARGET_NAME = 'webrtc_test'


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description='构建和编译项目，添加编译和构建选项'
    )
    parser.add_argument(
        '--run', '-r',
        action='store_true',
        help='构建和编译之后运行程序',
    )
    parser.add_argument(
        '--clean', '-c',
        action='store_true',
        help='清理构建和编译生成的文件',
    )
    parser.add_argument(
        '--qt', '-q',
        type=str,
        default=os.environ.get('QT6_MSVC_LIB'),
        help='Qt6 安装路径或 lib 目录（默认读取环境变量 QT6_MSVC_LIB）',
    )
    parser.add_argument(
        '--generate', '-G',
        action='store_true',
        help='强制重新生成构建文件（cmake configure）',
    )
    parser.add_argument(
        '--job', '-j',
        type=int,
        default=os.cpu_count() or 1,
        help='构建和编译时使用的线程数',
    )
    parser.add_argument(
        '--config',
        default='debug',
        choices=['debug', 'release'],
        help='构建和编译时使用的配置',
    )
    parser.add_argument(
        '--generator',
        default='Ninja',
        help='CMake 生成器（默认 Ninja）',
    )
    return parser.parse_args()


def resolve_qt_prefix(qt: str | None) -> Path | None:
    """将 Qt 路径规范为 CMAKE_PREFIX_PATH（若指向 lib 则上一级）。"""
    if not qt:
        return None
    path = Path(qt).expanduser().resolve()
    if path.name.lower() == 'lib': # 若指向 lib 则上一级
        path = path.parent
    if not path.is_dir(): # 若路径不存在则抛出异常
        raise FileNotFoundError(f'Qt 路径不存在: {path}')
    return path


def build_dir_for(config: str) -> Path:
    """返回构建路径"""
    return ROOT / 'build' / config.capitalize()


def env_path(env: dict[str, str]) -> str:
    """读取 PATH（Windows 下可能是 Path）。"""
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
    """确保 MSVC 工具链在环境中可用（cl / link 等）。"""
    env = dict(base or os.environ)
    if shutil.which('cl', path=env_path(env)):
        return env

    vcvars = find_vcvarsall()
    if vcvars is None:
        raise RuntimeError(
            '未找到 MSVC 环境（cl.exe / vcvarsall.bat）。'
            '请在“x64 Native Tools Command Prompt”中运行，或安装 VS C++ 工作负载。'
        )

    # call vcvarsall 后导出环境；/u 使输出为 UTF-16LE，避免中文环境乱码
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
        # Windows 下 Path/PATH 大小写并存时，保证两者一致
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


def find_ninja() -> Path | None:
    """查找可用的 ninja（跳过 depot_tools 等损坏的包装脚本）。"""
    candidates: list[Path] = []
    which = shutil.which('ninja')
    if which:
        candidates.append(Path(which))
    # 常见独立安装路径
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
            # depot_tools 的 ninja.bat 常不可用，尝试同目录 ninja.exe
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


def configure(
    build_dir: Path,
    *,
    config: str,
    qt_prefix: Path | None,
    generator: str,
    env: dict[str, str],
) -> None:
    build_dir.mkdir(parents=True, exist_ok=True)
    cmd = [
        'cmake',
        '-S', str(ROOT),
        '-B', str(build_dir),
        '-G', generator,
        f'-DCMAKE_BUILD_TYPE={config.capitalize()}',
    ]
    if qt_prefix is not None:
        cmd.append(f'-DCMAKE_PREFIX_PATH={qt_prefix.as_posix()}')

    if generator.lower() == 'ninja':
        ninja = find_ninja()
        if ninja is None:
            raise RuntimeError(
                '未找到可用的 ninja。请安装 ninja，或使用 --generator '
                '"NMake Makefiles" / "Visual Studio 17 2022"。'
            )
        cmd.append(f'-DCMAKE_MAKE_PROGRAM={ninja.as_posix()}')
        # 确保构建阶段也能直接调用该 ninja
        path = str(ninja.parent) + os.pathsep + env_path(env)
        env['PATH'] = path
        env['Path'] = path

    run_cmd(cmd, env=env)


def build(build_dir: Path, *, config: str, job: int, env: dict[str, str]) -> None:
    run_cmd(
        [
            'cmake',
            '--build', str(build_dir),
            '--config', config.capitalize(),
            '--parallel', str(job),
        ],
        env=env,
    )


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
    generate: bool,
    qt: str | None,
    clean: bool,
    run: bool,
    generator: str = 'Ninja',
) -> None:
    build_dir = build_dir_for(config)
    qt_prefix = resolve_qt_prefix(qt)

    # -c 单独使用：只清理；与 -G / -r 联用：清理后继续配置/构建/运行
    if clean:
        clean_build(build_dir)
        if not generate and not run:
            return

    env = load_msvc_env()
    if qt_prefix is not None:
        env['CMAKE_PREFIX_PATH'] = qt_prefix.as_posix()
        env['Path'] = str(qt_prefix / 'bin') + os.pathsep + env_path(env)
        env['PATH'] = env['Path']

    if generate or not cmake_configured(build_dir):
        if qt_prefix is None:
            raise RuntimeError(
                '未指定 Qt 路径。请设置环境变量 QT6_MSVC_LIB，或使用 --qt / -q 传入。'
            )
        configure(
            build_dir,
            config=config,
            qt_prefix=qt_prefix,
            generator=generator,
            env=env,
        )

    build(build_dir, config=config, job=job, env=env)

    if run:
        exe = find_executable(build_dir, config)
        print(f'运行: {exe}')
        run_app(exe, qt_prefix=qt_prefix, env=env)


def main() -> int:
    args = parse_args()
    common = {
        'config': args.config,
        'job': args.job,
        'generate': args.generate,
        'qt': args.qt,
        'clean': args.clean,
        'run': args.run,
        'generator': args.generator,
    }

    try:
        build_target(**common)
    except subprocess.CalledProcessError as e:
        print(f'error: 命令失败 (exit {e.returncode}): {e.cmd}', file=sys.stderr)
        return e.returncode or 1
    except Exception as e:
        print(f'error: {e}', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
