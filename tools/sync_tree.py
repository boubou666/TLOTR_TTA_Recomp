#!/usr/bin/env python3
"""Mirror a directory tree, writing only the files whose content changed.

    python3 -I tools/sync_tree.py <source> <destination> [<manifest>]

Unchanged files keep their mtime, so ninja rebuilds only the sources that really changed; files missing from the
source are deleted. Replaces `rsync -rc --delete`, which takes minutes for the ~49,000 generated files on Windows
(MSYS2 rsync costs several ms per file). With a manifest (the content hashes written by the previous sync), the
destination is not read back at all: only the freshly written source is hashed. Prints the number of files written or
deleted.
"""

import hashlib
import os
import shutil
import sys


def walk_files(root):
    """Relative paths (with forward slashes) of all files under root."""
    files = set()
    stack = [""]
    while stack:
        relative = stack.pop()
        with os.scandir(os.path.join(root, relative)) as entries:
            for entry in entries:
                path = f"{relative}/{entry.name}" if relative else entry.name
                if entry.is_dir(follow_symlinks=False):
                    stack.append(path)
                else:
                    files.add(path)
    return files


def digest(path):
    with open(path, "rb") as file:
        return hashlib.blake2b(file.read(), digest_size=16).hexdigest()


def load_manifest(path):
    manifest = {}
    if path and os.path.exists(path):
        with open(path, encoding="utf-8") as file:
            for line in file:
                name, _, value = line.rstrip("\n").rpartition("\t")
                manifest[name] = value
    return manifest


def main():
    if len(sys.argv) not in (3, 4):
        sys.exit(__doc__)
    source, destination = sys.argv[1], sys.argv[2]
    manifest_path = sys.argv[3] if len(sys.argv) == 4 else None
    previous = load_manifest(manifest_path)
    os.makedirs(destination, exist_ok=True)

    changed = 0
    current = {}
    wanted = walk_files(source)
    for relative in wanted:
        origin = os.path.join(source, relative)
        target = os.path.join(destination, relative)
        current[relative] = digest(origin)
        if relative in previous and os.path.exists(target):
            unchanged = previous[relative] == current[relative]
        else:
            unchanged = os.path.exists(target) and digest(target) == current[relative]
        if not unchanged:
            os.makedirs(os.path.dirname(target), exist_ok=True)
            shutil.copyfile(origin, target)
            changed += 1

    for relative in walk_files(destination) - wanted:
        os.remove(os.path.join(destination, relative))
        changed += 1
    for directory, _, _ in sorted(os.walk(destination), key=lambda item: -len(item[0])):
        if directory != destination and not os.listdir(directory):
            os.rmdir(directory)

    if manifest_path:
        with open(manifest_path + ".tmp", "w", encoding="utf-8") as file:
            file.writelines(f"{name}\t{current[name]}\n" for name in sorted(current))
        os.replace(manifest_path + ".tmp", manifest_path)
    print(changed)


if __name__ == "__main__":
    main()
