#!/usr/bin/env python3
"""Check changed project C++ files with clang-format 18 (or format with --fix)."""

import argparse
from pathlib import Path
import subprocess


def git(*args):
    return subprocess.check_output(["git", *args]).decode().strip("\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--base", default="HEAD")
    parser.add_argument("--head", help="Commit to compare; default checks the working tree")
    parser.add_argument("--binary", default="clang-format")
    parser.add_argument("--fix", action="store_true")
    args = parser.parse_args()
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
