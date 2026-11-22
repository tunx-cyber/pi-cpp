#!/usr/bin/env python3
"""Check changed project C++ files with clang-format 18 (or format with --fix).

格式结果与 clang-format 版本强相关（18 与 19+ 对聚合初始化、行尾注释对齐的处理不同），
因此默认要求二进制主版本为 18，与 CI 保持一致；确需用其他版本时加 --allow-any-version。
"""

import argparse
from pathlib import Path
import re
import subprocess
import sys

PINNED_MAJOR = 18


def git(*args):
    return subprocess.check_output(["git", *args]).decode().strip("\n")


def major_version(binary):
    try:
        out = subprocess.check_output([binary, "--version"], text=True, stderr=subprocess.STDOUT)
    except (OSError, subprocess.CalledProcessError) as exc:
        sys.exit(f"无法执行 {binary}：{exc}")
    match = re.search(r"clang-format version (\d+)", out)
    if not match:
        sys.exit(f"无法识别 {binary} 的版本输出：{out.strip()}")
    return int(match.group(1)), out.strip()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--base", default="HEAD")
    parser.add_argument("--head", help="Commit to compare; default checks the working tree")
    parser.add_argument("--binary", default="clang-format")
    parser.add_argument("--fix", action="store_true")
    parser.add_argument(
        "--allow-any-version",
        action="store_true",
        help=f"accept a clang-format major other than {PINNED_MAJOR}",
    )
    args = parser.parse_args()
    major, version = major_version(args.binary)
    if major != PINNED_MAJOR and not args.allow_any_version:
        sys.exit(
            f"{version} 与 CI 不一致（CI 用 {PINNED_MAJOR}）：\n"
            f"  python3 -m pip install clang-format=={PINNED_MAJOR}.1.8\n"
            "或加 --allow-any-version 忽略（结果可能与 CI 不同）。"
        )
    roots = ["include", "src", "apps", "tests"]
    revisions = [args.base] + ([args.head] if args.head else [])
    changed = git("diff", "--name-only", "--diff-filter=ACMR", "-z", *revisions, "--", *roots)
    files = set(changed.split("\0"))
    if not args.head:
        files.update(git("ls-files", "--others", "--exclude-standard", "-z", "--", *roots).split("\0"))
    files = sorted(p for p in files if Path(p).suffix in {".cpp", ".h", ".hpp"} and Path(p).is_file())
    if not files:
        print("No changed C++ files to check.")
        return 0
    flags = ["-i"] if args.fix else ["--dry-run", "--Werror"]
    return subprocess.run([args.binary, *flags, *files]).returncode


if __name__ == "__main__":
    raise SystemExit(main())
