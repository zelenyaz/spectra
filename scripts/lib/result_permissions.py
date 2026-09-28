#!/usr/bin/env python3
"""Give generated results back to the user who launched the experiments."""

import argparse
import os
from pathlib import Path
import stat


def _identity(uid, gid):
    values = (str(uid), str(gid))
    if not all(value.isascii() and value.isdecimal() for value in values):
        raise RuntimeError("Result UID and GID must be nonnegative integers")
    return tuple(int(value) for value in values)


def results_owner(root: Path) -> tuple[int, int]:
    for prefix in ("SPECTRA_RESULTS", "SUDO"):
        uid, gid = (os.environ.get(f"{prefix}_{field}") for field in ("UID", "GID"))
        if uid is not None or gid is not None:
            if uid is None or gid is None:
                raise RuntimeError(f"{prefix}_UID and {prefix}_GID must be set together")
            return _identity(uid, gid)
    if os.geteuid() != 0:
        return os.getuid(), os.getgid()
    info = root.stat()
    return info.st_uid, info.st_gid


def repair_results(root: Path, paths: list[Path], owner: tuple[int, int]):
    owner = _identity(*owner)
    results = Path(os.path.abspath(root)) / "results"
    targets = [Path(os.path.abspath(path)) for path in paths]
    # Validate the scope before changing permissions anywhere.
    for target in targets:
        try:
            target.relative_to(results)
        except ValueError as exc:
            raise RuntimeError(f"Result path is outside {results}: {target}") from exc

    def fix(path, recursive=False):
        try:
            info = path.lstat()
        except FileNotFoundError:
            return
        if stat.S_ISLNK(info.st_mode):
            return
        directory = stat.S_ISDIR(info.st_mode)
        mode = stat.S_IMODE(info.st_mode) | stat.S_IRUSR | stat.S_IWUSR
        if directory:
            mode |= stat.S_IXUSR
        changed_owner = (info.st_uid, info.st_gid) != owner
        if changed_owner:
            os.chown(path, *owner, follow_symlinks=False)
        if changed_owner or mode != stat.S_IMODE(info.st_mode):
            os.chmod(path, mode, follow_symlinks=False)
        if recursive and directory:
            for child in path.iterdir():
                fix(child, recursive=True)

    for target in targets:
        parents = []
        parent = target.parent
        while target != results and parent != results.parent:
            parents.append(parent)
            parent = parent.parent
        for parent in reversed(parents):
            if parent.is_symlink():
                raise RuntimeError(f"Result parent is a symbolic link: {parent}")
            fix(parent)
        fix(target, recursive=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser("owner").add_argument("root", type=Path)
    repair = commands.add_parser("repair")
    repair.add_argument("root", type=Path)
    repair.add_argument("uid")
    repair.add_argument("gid")
    repair.add_argument("paths", nargs="+", type=Path)
    args = parser.parse_args()
    try:
        if args.command == "owner":
            print(":".join(map(str, results_owner(args.root))))
        else:
            repair_results(args.root, args.paths, _identity(args.uid, args.gid))
    except (RuntimeError, OSError, ValueError, OverflowError) as exc:
        parser.exit(1, f"[result-permissions] {exc}\n")


if __name__ == "__main__":
    main()
