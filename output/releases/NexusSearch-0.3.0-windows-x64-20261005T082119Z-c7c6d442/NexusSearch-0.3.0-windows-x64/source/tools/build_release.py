#!/usr/bin/env python3
"""Build and verify a self-contained, local Windows portable NexusSearch ZIP.

Only Python's standard library is needed. Build tools are required on the build
machine; they are never needed by the resulting application's launchers.
"""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shutil
import socket
import stat
import subprocess
import sys
import tempfile
import time
import urllib.parse
import urllib.request
import uuid
import zipfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE_DIRS = ("src", "include", "bindings", "tests", "tools", "packaging",
               "cmake", "docs", "examples", "bench", "ui", ".github")
SOURCE_FILES = ("CMakeLists.txt", "README.md", "LICENSE", "LICENSE.txt", "LICENSE.md")


class ReleaseError(RuntimeError):
    pass


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def regular_files(root: Path) -> list[Path]:
    result = []
    for folder, directories, names in os.walk(root, followlinks=False):
        directories[:] = sorted(d for d in directories if d != "__pycache__")
        for name in directories + names:
            path = Path(folder) / name
            attributes = path.lstat()
            if path.is_symlink() or getattr(attributes, "st_file_attributes", 0) & 0x400:
                raise ReleaseError(f"Linked/reparse package paths are unsupported: {path}")
        for name in sorted(names):
            path = Path(folder) / name
            if name.endswith(".pyc"):
                continue
            if not stat.S_ISREG(path.stat().st_mode):
                raise ReleaseError(f"Non-regular package file: {path}")
            result.append(path)
    return sorted(result, key=lambda p: p.relative_to(root).as_posix())


def inventory(root: Path) -> list[dict]:
    return [{"path": p.relative_to(root).as_posix(), "bytes": p.stat().st_size,
             "sha256": digest(p)} for p in regular_files(root)]


def write_json(path: Path, value: object) -> None:
    path.write_text(json.dumps(value, indent=2, ensure_ascii=False, sort_keys=True) + "\n",
                    encoding="utf-8", newline="\n")


def source_inventory(root: Path) -> list[dict]:
    paths = [root / name for name in SOURCE_FILES if (root / name).is_file()]
    for name in SOURCE_DIRS:
        if (root / name).is_dir():
            paths.extend(regular_files(root / name))
    return [{"path": p.relative_to(root).as_posix(), "bytes": p.stat().st_size,
             "sha256": digest(p)} for p in sorted(paths)]


def freeze_source(root: Path, destination: Path) -> list[dict]:
    before = source_inventory(root)
    for entry in before:
        target = destination / entry["path"]
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(root / entry["path"], target)
        if digest(target) != entry["sha256"]:
            raise ReleaseError("Source changed while copying; rerun after edits stop")
    if source_inventory(root) != before:
        raise ReleaseError("Source changed while copying; rerun after edits stop")
    return before


def run(command: list[str | Path], *, cwd: Path, env: dict | None = None,
        timeout: int = 300) -> subprocess.CompletedProcess:
    result = subprocess.run([str(item) for item in command], cwd=cwd, env=env,
                            capture_output=True, text=True, encoding="utf-8",
                            errors="replace", timeout=timeout)
    if result.returncode:
        raise ReleaseError(f"Command failed ({result.returncode}): {command}\n"
                           f"{result.stdout[-6000:]}\n{result.stderr[-6000:]}")
    return result


def find_tool(name: str, toolchain: Path) -> Path:
    local = toolchain / (name + ".exe")
    found = str(local) if local.is_file() else shutil.which(name)
    if not found:
        raise ReleaseError(f"Build tool not found: {name}. Set --toolchain-bin.")
    return Path(found).resolve()


def dll_imports(binary: Path, objdump: Path, env: dict) -> list[str]:
    result = run([objdump, "-p", binary], cwd=binary.parent, env=env)
    names = re.findall(r"DLL Name:\s*([^\r\n]+)", result.stdout)
    if not names:
        raise ReleaseError(f"Could not inspect PE imports: {binary}")
    return sorted(set(name.strip() for name in names), key=str.casefold)


def bundle_dependencies(package: Path, objdump: Path, search: list[Path], env: dict) -> dict:
    system = Path(os.environ.get("SystemRoot", r"C:\Windows")) / "System32"
    queue = [package / "nexus.exe", package / "libnexus.dll"]
    inspected = set()
    records = []
    while queue:
        binary = queue.pop(0)
        if binary.name.casefold() in inspected:
            continue
        inspected.add(binary.name.casefold())
        imports = []
        for name in dll_imports(binary, objdump, env):
            if not re.fullmatch(r"[A-Za-z0-9_.+-]+\.dll", name, re.IGNORECASE):
                raise ReleaseError(f"Unexpected imported DLL name: {name}")
            api_contract = name.casefold().startswith(("api-ms-win-", "ext-ms-win-"))
            if api_contract or (system / name).is_file():
                imports.append({"name": name, "resolution": "windows-system"})
                continue
            candidate = next((folder / name for folder in [package, *search]
                              if (folder / name).is_file()), None)
            if candidate is None:
                raise ReleaseError(f"Unresolved runtime DLL {name}, imported by {binary.name}")
            target = package / name
            if candidate.resolve() != target.resolve():
                if target.exists() and digest(target) != digest(candidate):
                    raise ReleaseError(f"Conflicting dependency DLL: {name}")
                shutil.copyfile(candidate, target)
            imports.append({"name": name, "resolution": "bundled", "sha256": digest(target)})
            queue.append(target)
        records.append({"binary": binary.name, "imports": imports})
    return {"method": "recursive PE import inspection with objdump",
            "binaries": sorted(records, key=lambda row: row["binary"].casefold()),
            "system_resolution": "API-set contracts or files present in Windows System32"}


def clean_environment() -> dict[str, str]:
    system = Path(os.environ.get("SystemRoot", r"C:\Windows"))
    keep = ("SystemRoot", "WINDIR", "TEMP", "TMP", "USERPROFILE", "LOCALAPPDATA",
            "APPDATA", "COMSPEC", "PATHEXT", "PROCESSOR_ARCHITECTURE")
    result = {key: os.environ[key] for key in keep if key in os.environ}
    result["PATH"] = os.pathsep.join(str(p) for p in
        (system / "System32", system, system / "System32/WindowsPowerShell/v1.0"))
    result["PYTHONNOUSERSITE"] = "1"
    result["PYTHONDONTWRITEBYTECODE"] = "1"
    return result


def free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as listener:
        listener.bind(("127.0.0.1", 0))
        return listener.getsockname()[1]


def smoke_test(package: Path, version: str, *, launcher: bool = True) -> dict:
    """Exercise a copied package, never a checkout or a development PATH."""
    with tempfile.TemporaryDirectory(prefix="NexusSearch portable check ") as temporary:
        copied = Path(temporary) / "Copied release with spaces and café"
        shutil.copytree(package, copied)
        env = clean_environment()
        exe = copied / "nexus.exe"
        version_text = run([exe, "--version"], cwd=copied, env=env, timeout=15).stdout.strip()
        if version_text != f"NexusSearch {version}":
            raise ReleaseError(f"Executable/source version mismatch: {version_text!r}, {version}")
        snapshot = copied / "sample with spaces.nxs"
        built = json.loads(run([exe, "build", copied / "examples/documents.jsonl", snapshot],
                               cwd=copied, env=env, timeout=30).stdout)
        result = json.loads(run([exe, "search", snapshot, 'title="Local search in C"'],
                                cwd=copied, env=env, timeout=15).stdout)
        if result["total"] != 1 or result["hits"][0]["_id"] != "paper-01":
            raise ReleaseError("Copied executable search did not return the expected document")
        prepared_result = json.loads(run([exe, "search", snapshot, "title:search", "--prepared"],
                                         cwd=copied, env=env, timeout=15).stdout)
        if prepared_result["execution"]["lexical"] != "text_postings":
            raise ReleaseError("Copied executable did not use prepared exact text search")
        python_env = dict(env, PYTHONPATH=str(copied / "bindings/python"),
                          NEXUS_LIB_PATH=str(copied / "libnexus.dll"))
        python_code = (
            "import os; before=os.environ['PATH']; from nexus import Snapshot; "
            "s=Snapshot.open('sample with spaces.nxs'); "
            "assert s.search('title=\"Local search in C\"').hits[0].id=='paper-01'; "
            "p=s.prepare(); assert p.search('title:search').prepared_text; p.close(); "
            "assert os.environ['PATH']==before; print('Python shared library passed')"
        )
        run([sys.executable, "-c", python_code], cwd=copied, env=python_env, timeout=20)
        port = free_port()
        with (copied / "smoke-server.log").open("wb") as log:
            server = subprocess.Popen([str(exe), "serve", str(snapshot), "--host", "127.0.0.1",
                                       "--port", str(port)], cwd=copied, env=env,
                                      stdout=log, stderr=log)
            try:
                deadline = time.monotonic() + 10
                health = None
                while time.monotonic() < deadline:
                    if server.poll() is not None:
                        raise ReleaseError("Copied HTTP server exited during startup")
                    try:
                        with urllib.request.urlopen(f"http://127.0.0.1:{port}/health", timeout=1) as response:
                            health = json.load(response)
                        break
                    except OSError:
                        time.sleep(.1)
                if health != {"status": "ok", "version": version}:
                    raise ReleaseError(f"Copied HTTP health/version check failed: {health}")
                query = urllib.parse.urlencode({"q": 'title="Local search in C"'})
                with urllib.request.urlopen(f"http://127.0.0.1:{port}/api/search?{query}", timeout=5) as response:
                    http_result = json.load(response)
                if http_result["hits"][0]["_id"] != "paper-01" or http_result["total"] != 1:
                    raise ReleaseError("Copied HTTP search check failed")
            finally:
                server.terminate()
                try:
                    server.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    server.kill()
                    server.wait(timeout=5)
        if launcher:
            powershell = Path(os.environ.get("SystemRoot", r"C:\Windows")) / "System32/WindowsPowerShell/v1.0/powershell.exe"
            launch_port = free_port()
            launch = run([powershell, "-NoProfile", "-ExecutionPolicy", "Bypass", "-File",
                          copied / "Start-NexusSearch.ps1", "-CheckOnly", "-NoBrowser",
                          "-Port", str(launch_port)], cwd=copied, env=env, timeout=30)
            if "Startup check passed." not in launch.stdout:
                raise ReleaseError("Portable launcher did not confirm readiness")
            preserved = digest(copied / "data/example.nxs")
            run([powershell, "-NoProfile", "-ExecutionPolicy", "Bypass", "-File",
                 copied / "Start-NexusSearch.ps1", "-CheckOnly", "-NoBrowser",
                 "-Port", str(free_port())], cwd=copied, env=env, timeout=30)
            if digest(copied / "data/example.nxs") != preserved:
                raise ReleaseError("Launcher replaced an existing snapshot")
        return {"passed": True, "version": version, "host": platform.platform(),
                "python_machine": platform.machine(), "binary_target": "windows-x64",
                "python_version": platform.python_version(), "development_path_removed": True,
                "copied_path_includes_spaces_and_unicode": True,
                "checks": ["CLI version", "snapshot build", "exact search", "prepared text search", "Python shared library",
                           "unchanged Python PATH", "HTTP health", "HTTP search"] +
                          (["launcher readiness", "launcher preserves existing snapshot"] if launcher else []),
                "built_rows": built.get("rows")}


def make_manifest(package: Path, version: str) -> None:
    entries = [entry for entry in inventory(package) if entry["path"] != "RELEASE_MANIFEST.json"]
    write_json(package / "RELEASE_MANIFEST.json", {"format": 1, "version": version,
               "target": "windows-x64", "hash_algorithm": "sha256", "files": entries})


def verify_package(package: Path) -> dict:
    manifest = json.loads((package / "RELEASE_MANIFEST.json").read_text(encoding="utf-8"))
    actual = {entry["path"]: entry for entry in inventory(package)
              if entry["path"] != "RELEASE_MANIFEST.json"}
    entries = manifest["files"]
    expected = {entry["path"]: entry for entry in entries}
    if len(expected) != len(entries) or actual != expected:
        raise ReleaseError("Portable package content does not match RELEASE_MANIFEST.json")
    required = {"nexus.exe", "libnexus.dll", "Start NexusSearch.cmd", "Start-NexusSearch.ps1",
                "examples/documents.jsonl", "SOURCE_MANIFEST.json", "DEPENDENCIES.json",
                "bindings/python/nexus/_ffi.py", "README-PORTABLE.txt"}
    if not required <= actual.keys():
        raise ReleaseError(f"Portable package is missing required files: {required - actual.keys()}")
    source = json.loads((package / "SOURCE_MANIFEST.json").read_text(encoding="utf-8"))
    if inventory(package / "source") != source["files"]:
        raise ReleaseError("Bundled source does not match SOURCE_MANIFEST.json")
    return manifest


def deterministic_zip(package: Path, archive: Path, archive_root: str) -> str:
    with zipfile.ZipFile(archive, "x", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as zipped:
        for path in regular_files(package):
            name = archive_root + "/" + path.relative_to(package).as_posix()
            entry = zipfile.ZipInfo(name, date_time=(1980, 1, 1, 0, 0, 0))
            entry.compress_type = zipfile.ZIP_DEFLATED
            entry.create_system = 3
            entry.external_attr = (stat.S_IFREG | 0o644) << 16
            zipped.writestr(entry, path.read_bytes(), compresslevel=9)
    with zipfile.ZipFile(archive) as zipped:
        if zipped.testzip() is not None:
            raise ReleaseError("Portable ZIP CRC verification failed")
        for path in regular_files(package):
            name = archive_root + "/" + path.relative_to(package).as_posix()
            if hashlib.sha256(zipped.read(name)).hexdigest() != digest(path):
                raise ReleaseError(f"Portable ZIP content mismatch: {name}")
    return digest(archive)


def build_release(source: Path, output: Path, toolchain: Path, jobs: int) -> dict:
    if os.name != "nt":
        raise ReleaseError("This portable build command targets Windows and must run on Windows")
    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ") + "-" + uuid.uuid4().hex[:8]
    workspace = source / "build-portable" / stamp
    frozen = workspace / "source"
    frozen.mkdir(parents=True, exist_ok=False)
    files = freeze_source(source, frozen)
    project = (frozen / "CMakeLists.txt").read_text(encoding="utf-8")
    version_match = re.search(r"project\(NexusSearch\s+VERSION\s+(\d+\.\d+\.\d+)", project)
    if not version_match:
        raise ReleaseError("Could not read NexusSearch version from CMakeLists.txt")
    version = version_match.group(1)
    cmake, ninja, gcc, objdump = [find_tool(name, toolchain) for name in ("cmake", "ninja", "gcc", "objdump")]
    extra_runtime = toolchain.parent.parent / "usr" / "bin"
    build_path = [str(toolchain)]
    if extra_runtime.is_dir():
        build_path.append(str(extra_runtime))
    if os.environ.get("PATH"):
        build_path.append(os.environ["PATH"])
    env = dict(os.environ, PATH=os.pathsep.join(build_path))
    target = run([gcc, "-dumpmachine"], cwd=workspace, env=env).stdout.strip()
    if target != "x86_64-w64-mingw32":
        raise ReleaseError(f"This package recipe requires MinGW x64; found {target}")
    build = workspace / "build"
    configure = run([cmake, "-S", frozen, "-B", build, "-G", "Ninja",
                     f"-DCMAKE_MAKE_PROGRAM={ninja}", f"-DCMAKE_C_COMPILER={gcc}",
                     "-DCMAKE_BUILD_TYPE=Release", "-DNX_MEM_DEBUG=OFF",
                     "-DNX_BUILD_TESTS=ON", "-DNX_BUILD_BENCH=OFF"], cwd=workspace, env=env)
    (workspace / "configure.log").write_text(configure.stdout + configure.stderr, encoding="utf-8")
    compiled = run([cmake, "--build", build, "--parallel", str(jobs)], cwd=workspace, env=env, timeout=600)
    (workspace / "build.log").write_text(compiled.stdout + compiled.stderr, encoding="utf-8")
    ctest = cmake.parent / "ctest.exe"
    test = run([ctest, "--test-dir", build, "--output-on-failure"], cwd=workspace, env=env, timeout=600)
    (workspace / "ctest.log").write_text(test.stdout + test.stderr, encoding="utf-8")
    stem = f"NexusSearch-{version}-windows-x64"
    destination = output / f"{stem}-{stamp}"
    destination.mkdir(parents=True, exist_ok=False)
    package = destination / stem
    package.mkdir()
    for name in ("nexus.exe", "libnexus.dll"):
        shutil.copyfile(build / name, package / name)
    for name in ("bindings", "docs", "examples"):
        shutil.copytree(frozen / name, package / name)
    for path in regular_files(frozen / "packaging"):
        if path.name in ("Start NexusSearch.cmd", "Start-NexusSearch.ps1", "README-PORTABLE.txt"):
            shutil.copyfile(path, package / path.name)
    shutil.copytree(frozen, package / "source")
    # Sort by POSIX relative path, just like inventory(), independent of Windows separators.
    files.sort(key=lambda entry: entry["path"])
    write_json(package / "SOURCE_MANIFEST.json", {"format": 1, "version": version,
               "hash_algorithm": "sha256", "files": files})
    imports = bundle_dependencies(package, objdump, [build, toolchain], env)
    write_json(package / "DEPENDENCIES.json", imports)
    compiler = run([gcc, "--version"], cwd=workspace, env=env).stdout.splitlines()[0]
    write_json(package / "BUILD_INFO.json", {"version": version, "compiler": compiler,
               "target": target, "configuration": "Release", "memory_debug": False,
               "cpu_baseline": "x86-64-v2", "ctest_output": test.stdout})
    receipt = smoke_test(package, version)
    write_json(package / "VALIDATION.json", receipt)
    make_manifest(package, version)
    verify_package(package)
    archive = destination / (stem + ".zip")
    checksum = deterministic_zip(package, archive, stem)
    (destination / (archive.name + ".sha256")).write_text(checksum + "  " + archive.name + "\n", encoding="ascii")
    result = {"version": version, "directory": str(package), "archive": str(archive),
              "sha256": checksum, "build_directory": str(build),
              "files": len(inventory(package)), "validation": receipt}
    write_json(destination / "release-result.json", result)
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=ROOT)
    parser.add_argument("--output-dir", type=Path, default=ROOT / "output/releases")
    parser.add_argument("--toolchain-bin", type=Path, default=Path(r"C:\msys64\ucrt64\bin"))
    parser.add_argument("--jobs", type=int, default=4)
    parser.add_argument("--verify", type=Path, help="Verify an extracted package without building")
    args = parser.parse_args()
    try:
        if args.verify:
            manifest = verify_package(args.verify.resolve())
            print(json.dumps({"verified": str(args.verify.resolve()), "files": len(manifest["files"])}))
        else:
            if args.jobs < 1 or args.jobs > 64:
                raise ReleaseError("--jobs must be between 1 and 64")
            result = build_release(args.source.resolve(), args.output_dir.resolve(), args.toolchain_bin.resolve(), args.jobs)
            print(json.dumps(result, indent=2))
        return 0
    except (ReleaseError, OSError, ValueError, subprocess.SubprocessError) as error:
        print(f"release: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
