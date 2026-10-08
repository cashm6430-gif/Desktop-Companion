"""Hash the editable assets and their production code, including Git blobs.

Refresh explicitly for an accepted checkpoint. Normal verification is read-only.
"""

import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import subprocess


ROOT = Path(__file__).resolve().parents[1]
CATALOG = "art/live2d/workflow/asset-catalog.json"
SCOPE = (
    "art", "assets", "images", "tools", "src", "tests", "godot",
    ".gitattributes", ".gitignore", "CMakeLists.txt", "CMakePresets.json",
    "conanfile.py", "conan.lock",
)


def safe_path(value):
    if not isinstance(value, str) or not value or "\\" in value or ":" in value:
        raise ValueError(f"Invalid repository path: {value!r}")
    path = PurePosixPath(value)
    if path.is_absolute() or ".." in path.parts or str(path) != value:
        raise ValueError(f"Invalid repository path: {value!r}")
    if any(ord(char) < 32 for char in value):
        raise ValueError(f"Invalid repository path: {value!r}")
    return path


def digest_file(path):
    sha = hashlib.sha256()
    size = 0
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            size += len(chunk)
            sha.update(chunk)
    return {"bytes": size, "sha256": sha.hexdigest()}


def local_path(root, name, allow_new_file=False):
    safe_path(name)
    path = root / name
    if not path.resolve().is_relative_to(root):
        raise ValueError(f"Asset resolves outside the recovery root: {name}")
    node = root
    for part in PurePosixPath(name).parts:
        node = node / part
        # Windows junctions and other reparse points can also refer elsewhere.
        try:
            attributes = getattr(node.lstat(), "st_file_attributes", 0)
        except FileNotFoundError:
            if allow_new_file and node == path:
                break
            raise
        if node.is_symlink() or attributes & 0x400:
            raise ValueError(f"Asset uses a symlink or reparse point: {name}")
    return path


def git_paths(root, prefix=None):
    if prefix and prefix != ":":
        command = ["git", "ls-tree", "-r", "--name-only", "-z", prefix[:-1], "--", *SCOPE]
    else:
        command = ["git", "ls-files", "--cached"]
        if prefix is None:
            command += ["--others", "--exclude-standard"]
        command += ["-z", "--", *SCOPE]
    result = subprocess.run(command, cwd=root, check=True, capture_output=True)
    return set(result.stdout.decode("utf8").split("\0")) - {"", CATALOG}


def inventory(root, prefix):
    if prefix is not None:
        return git_paths(root, prefix), "git"
    result = subprocess.run(
        ["git", "rev-parse", "--show-toplevel"], cwd=root, capture_output=True, text=True,
    )
    if result.returncode == 0 and Path(result.stdout.strip()).resolve() == root:
        return git_paths(root), "git"
    # A standalone recovery has no index or ignore engine. Check every declared
    # byte; Git modes are used to gate whether newly added files were catalogued.
    return set(), "standalone_recovery_catalog_only"


class GitBlobs:
    def __init__(self, root, prefix):
        self.prefix = prefix
        self.process = subprocess.Popen(
            ["git", "cat-file", "--batch"], cwd=root,
            stdin=subprocess.PIPE, stdout=subprocess.PIPE,
        )

    def read(self, path, collect=False):
        safe_path(path)
        self.process.stdin.write((self.prefix + path + "\n").encode("utf8"))
        self.process.stdin.flush()
        header = self.process.stdout.readline().decode("utf8").rstrip("\n")
        if header.endswith(" missing"):
            raise FileNotFoundError(path)
        fields = header.split()
        if len(fields) != 3 or fields[1] != "blob":
            raise ValueError(f"Expected a Git blob for {path}: {header}")
        size = remaining = int(fields[2])
        sha = hashlib.sha256()
        chunks = []
        while remaining:
            chunk = self.process.stdout.read(min(remaining, 1024 * 1024))
            if not chunk:
                raise EOFError(f"Truncated Git blob: {path}")
            remaining -= len(chunk)
            sha.update(chunk)
            if collect:
                chunks.append(chunk)
        if self.process.stdout.read(1) != b"\n":
            raise ValueError(f"Invalid Git batch delimiter: {path}")
        return b"".join(chunks) if collect else {"bytes": size, "sha256": sha.hexdigest()}

    def close(self):
        self.process.stdin.close()
        self.process.stdout.close()
        self.process.wait()


def refresh(root, prefix=None):
    files = {}
    reader = GitBlobs(root, prefix) if prefix is not None else None
    try:
        for name in sorted(git_paths(root, prefix)):
            files[name] = reader.read(name) if reader else digest_file(local_path(root, name))
    finally:
        if reader:
            reader.close()
    catalog = {
        "version": 1,
        "purpose": "Exact-byte recovery of author assets, runtime assets, review evidence and matching production code.",
        "refresh_policy": "Refresh explicitly after accepting new assets; commit the catalog with those assets.",
        "scope": list(SCOPE),
        "excluded": ["build/", ".local/", "licensed SDK and installed tools", "ignored incomplete/rejected review attempts"],
        "files": files,
    }
    local_path(root, CATALOG, allow_new_file=True).write_text(json.dumps(catalog, ensure_ascii=False, indent=2) + "\n", encoding="utf8")
    return {"mode": "refresh_index" if prefix == ":" else "refresh", "files": len(files),
            "bytes": sum(item["bytes"] for item in files.values())}


def verify(root, prefix=None):
    reader = GitBlobs(root, prefix) if prefix is not None else None
    try:
        data = reader.read(CATALOG, collect=True) if reader else local_path(root, CATALOG).read_bytes()
        catalog = json.loads(data)
        if catalog["version"] != 1 or not catalog["files"]:
            raise ValueError("Unsupported or empty asset catalog")
        failures = []
        for name, expected in catalog["files"].items():
            safe_path(name)
            try:
                actual = reader.read(name) if reader else digest_file(local_path(root, name))
                if actual != expected:
                    failures.append({"path": name, "expected": expected, "actual": actual})
            except FileNotFoundError:
                failures.append({"path": name, "error": "missing"})
        paths, inventory_mode = inventory(root, prefix)
        for name in sorted(paths - set(catalog["files"])):
            failures.append({"path": name, "error": "not_catalogued; refresh explicitly for the accepted checkpoint"})
        return {
            "mode": prefix or "worktree", "files": len(catalog["files"]),
            "inventory": inventory_mode, "passed": not failures, "failures": failures,
        }
    finally:
        if reader:
            reader.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=ROOT, help="Repository or recovered checkout root")
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--refresh", action="store_true")
    mode.add_argument("--refresh-index", action="store_true",
                      help="Refresh from staged bytes only; do not include unrelated untracked assets")
    mode.add_argument("--index", action="store_true")
    mode.add_argument("--git-ref", help="Verify the catalog and assets stored in a Git commit/ref")
    args = parser.parse_args()
    root = args.root.resolve()
    prefix = None
    if args.index:
        prefix = ":"
    elif args.git_ref:
        revision = subprocess.run(
            ["git", "rev-parse", "--verify", "--end-of-options", args.git_ref + "^{commit}"],
            cwd=root, check=True, capture_output=True, text=True,
        ).stdout.strip()
        prefix = revision + ":"
    try:
        if args.refresh_index:
            result = refresh(root, ":")
        else:
            result = refresh(root) if args.refresh else verify(root, prefix)
    except (OSError, ValueError, KeyError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"Asset verification failed: {error}\n")
    print(json.dumps(result, ensure_ascii=False, indent=2))
    raise SystemExit(0 if result.get("passed", True) else 1)


if __name__ == "__main__":
    main()
