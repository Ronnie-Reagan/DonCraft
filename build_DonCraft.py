from __future__ import annotations

import argparse
import csv
import hashlib
import json
import os
from fnmatch import fnmatch
import re
import shutil
import subprocess
import sys
import time
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Any


@dataclass
class TimedCommandResult:
    command: list[str]
    cwd: Path
    started_utc: str
    duration_seconds: float
    returncode: int


@dataclass
class ResolvedConfigurePreset:
    name: str
    binary_dir: Path
    generator: str | None
    architecture: str | None
    cache_variables: dict[str, str]


@dataclass
class ResolvedBuildPreset:
    name: str
    configure_preset: str
    configuration: str | None
    targets: list[str]


@dataclass
class ResolvedTestPreset:
    name: str
    configure_preset: str
    configuration: str | None


@dataclass
class PackageCopyStats:
    copied_files: int = 0
    copied_bytes: int = 0
    skipped_files: int = 0
    duplicate_files: int = 0

    def add(self, other: "PackageCopyStats") -> None:
        self.copied_files += other.copied_files
        self.copied_bytes += other.copied_bytes
        self.skipped_files += other.skipped_files
        self.duplicate_files += other.duplicate_files


@dataclass
class PackageOptions:
    source_dir: Path
    mode: str
    include_source_payload: bool
    include_steam_appid: bool
    payload_dirs: list[Path]
    runtime_roots: list[Path]
    include_globs: list[str]
    exclude_globs: list[str]
    verbose: bool


@dataclass
class StagedPackage:
    path: Path
    entrypoint: str
    stats: PackageCopyStats


CLIENT_TARGET = "Don_Craft_client"
SERVER_TARGET = "Don_Craft_server"
LAUNCHER_TARGET = "DonCraftLauncher"
DEFAULT_GITHUB_REPO = "Ronnie-Reagan/DonCraft"
DEFAULT_UPDATE_BRANCH = "main"
DEFAULT_UPDATE_CHANNEL = "alpha"

DEFAULT_PACKAGE_MODE = "filtered"
DEFAULT_PACKAGE_EXCLUDE_GLOBS = (
    ".git/*",
    ".vs/*",
    "CMakeFiles/*",
    "Testing/*",
    "*.pdb",
    "*.ilk",
    "*.exp",
    "*.lib",
    "*.obj",
    "*.iobj",
    "*.ipdb",
    "*.lastbuildstate",
    "*.tlog",
    "*.log",
    "*.tmp",
    "*.download",
    "desktop.ini",
    "Thumbs.db",
)
DEFAULT_SOURCE_PAYLOAD_DIRS = (
    "assets",
    "Assets",
    "data",
    "Data",
    "content",
    "Content",
    "resources",
    "Resources",
    "config",
    "Config",
    "shaders",
    "Shaders",
    "Native/assets",
    "Native/Assets",
    "Native/data",
    "Native/Data",
    "Native/content",
    "Native/Content",
    "Native/resources",
    "Native/Resources",
    "Native/config",
    "Native/Config",
    "Native/shaders",
    "Native/Shaders",
)


def find_cmake() -> str:
    cmake = shutil.which("cmake")
    if cmake:
        return cmake

    windows_fallbacks = (
        Path(r"C:\Program Files\CMake\bin\cmake.exe"),
        Path(r"C:\Program Files\Microsoft Visual Studio\2022\Professional\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"),
        Path(r"C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"),
    )
    for candidate in windows_fallbacks:
        if candidate.exists():
            return str(candidate)

    raise FileNotFoundError("cmake was not found on PATH and no known Windows install path exists")


def find_ctest(cmake: str) -> str:
    sibling_name = "ctest.exe" if os.name == "nt" else "ctest"
    sibling = Path(cmake).with_name(sibling_name)
    if sibling.exists():
        return str(sibling)

    ctest = shutil.which("ctest")
    if ctest:
        return ctest

    raise FileNotFoundError("ctest was not found alongside cmake or on PATH")


def stringify_command(command: list[str]) -> str:
    return " ".join(f'"{part}"' if " " in part else part for part in command)


def run_command(command: list[str], cwd: Path) -> TimedCommandResult:
    started_utc = datetime.now(timezone.utc).isoformat()
    started_perf = time.perf_counter()

    print("+", stringify_command(command))
    completed = subprocess.run(command, cwd=str(cwd), check=False)

    duration_seconds = time.perf_counter() - started_perf
    result = TimedCommandResult(
        command=command,
        cwd=cwd,
        started_utc=started_utc,
        duration_seconds=duration_seconds,
        returncode=completed.returncode,
    )

    print(f"  -> exit={result.returncode} elapsed={result.duration_seconds:.3f}s")

    if completed.returncode != 0:
        raise subprocess.CalledProcessError(completed.returncode, command)

    return result


def cmake_value_to_string(value: Any) -> str:
    if isinstance(value, bool):
        return "ON" if value else "OFF"
    return str(value)


def normalize_cache_value(value: str) -> str:
    return os.path.normcase(value.replace("\\", "/")).rstrip("/")


def load_cmake_presets(source_dir: Path) -> dict[str, Any]:
    presets_path = source_dir / "CMakePresets.json"
    return json.loads(presets_path.read_text(encoding="utf-8"))


def normalize_inherits(value: Any) -> list[str]:
    if value is None:
        return []
    if isinstance(value, list):
        return [str(item) for item in value]
    return [str(value)]


def expand_preset_path(template: str, source_dir: Path, preset_name: str) -> Path:
    expanded = template.replace("${sourceDir}", str(source_dir)).replace("${presetName}", preset_name)
    return Path(expanded).resolve()


def resolve_architecture(value: Any) -> str | None:
    if isinstance(value, dict):
        resolved = value.get("value")
        return None if resolved is None else str(resolved)
    if value is None:
        return None
    return str(value)


def resolve_configure_preset(presets: dict[str, Any], source_dir: Path, preset_name: str) -> ResolvedConfigurePreset:
    configure_index = {preset["name"]: preset for preset in presets.get("configurePresets", [])}
    if preset_name not in configure_index:
        raise KeyError(f"Unknown configure preset: {preset_name}")

    def resolve(name: str, stack: tuple[str, ...]) -> dict[str, Any]:
        if name in stack:
            cycle = " -> ".join((*stack, name))
            raise ValueError(f"Configure preset inheritance cycle detected: {cycle}")

        preset = configure_index[name]
        merged: dict[str, Any] = {
            "name": name,
            "generator": None,
            "architecture": None,
            "binaryDir": None,
            "cacheVariables": {},
        }

        for parent_name in normalize_inherits(preset.get("inherits")):
            parent = resolve(parent_name, (*stack, name))
            if parent["generator"] is not None:
                merged["generator"] = parent["generator"]
            if parent["architecture"] is not None:
                merged["architecture"] = parent["architecture"]
            if parent["binaryDir"] is not None:
                merged["binaryDir"] = parent["binaryDir"]
            merged["cacheVariables"].update(parent["cacheVariables"])

        if preset.get("generator") is not None:
            merged["generator"] = str(preset["generator"])
        if preset.get("architecture") is not None:
            merged["architecture"] = resolve_architecture(preset["architecture"])
        if preset.get("binaryDir") is not None:
            merged["binaryDir"] = str(preset["binaryDir"])
        for key, value in preset.get("cacheVariables", {}).items():
            merged["cacheVariables"][str(key)] = cmake_value_to_string(value)

        return merged

    resolved = resolve(preset_name, ())
    binary_dir_template = resolved["binaryDir"]
    if not binary_dir_template:
        raise ValueError(f"Configure preset '{preset_name}' does not define binaryDir")

    return ResolvedConfigurePreset(
        name=preset_name,
        binary_dir=expand_preset_path(binary_dir_template, source_dir, preset_name),
        generator=resolved["generator"],
        architecture=resolved["architecture"],
        cache_variables=dict(resolved["cacheVariables"]),
    )


def resolve_build_preset(presets: dict[str, Any], preset_name: str) -> ResolvedBuildPreset:
    build_index = {preset["name"]: preset for preset in presets.get("buildPresets", [])}
    if preset_name not in build_index:
        raise KeyError(f"Unknown build preset: {preset_name}")

    def resolve(name: str, stack: tuple[str, ...]) -> dict[str, Any]:
        if name in stack:
            cycle = " -> ".join((*stack, name))
            raise ValueError(f"Build preset inheritance cycle detected: {cycle}")

        preset = build_index[name]
        merged: dict[str, Any] = {
            "name": name,
            "configurePreset": None,
            "configuration": None,
            "targets": [],
        }

        for parent_name in normalize_inherits(preset.get("inherits")):
            parent = resolve(parent_name, (*stack, name))
            if parent["configurePreset"] is not None:
                merged["configurePreset"] = parent["configurePreset"]
            if parent["configuration"] is not None:
                merged["configuration"] = parent["configuration"]
            if parent["targets"]:
                merged["targets"] = list(parent["targets"])

        if preset.get("configurePreset") is not None:
            merged["configurePreset"] = str(preset["configurePreset"])
        if preset.get("configuration") is not None:
            merged["configuration"] = str(preset["configuration"])
        if preset.get("targets") is not None:
            merged["targets"] = [str(target) for target in preset["targets"]]

        return merged

    resolved = resolve(preset_name, ())
    if not resolved["configurePreset"]:
        raise ValueError(f"Build preset '{preset_name}' does not define configurePreset")

    return ResolvedBuildPreset(
        name=preset_name,
        configure_preset=resolved["configurePreset"],
        configuration=resolved["configuration"],
        targets=list(resolved["targets"]),
    )


def resolve_test_preset(presets: dict[str, Any], preset_name: str) -> ResolvedTestPreset:
    test_index = {preset["name"]: preset for preset in presets.get("testPresets", [])}
    if preset_name not in test_index:
        raise KeyError(f"Unknown test preset: {preset_name}")

    def resolve(name: str, stack: tuple[str, ...]) -> dict[str, Any]:
        if name in stack:
            cycle = " -> ".join((*stack, name))
            raise ValueError(f"Test preset inheritance cycle detected: {cycle}")

        preset = test_index[name]
        merged: dict[str, Any] = {
            "name": name,
            "configurePreset": None,
            "configuration": None,
        }

        for parent_name in normalize_inherits(preset.get("inherits")):
            parent = resolve(parent_name, (*stack, name))
            if parent["configurePreset"] is not None:
                merged["configurePreset"] = parent["configurePreset"]
            if parent["configuration"] is not None:
                merged["configuration"] = parent["configuration"]

        if preset.get("configurePreset") is not None:
            merged["configurePreset"] = str(preset["configurePreset"])
        if preset.get("configuration") is not None:
            merged["configuration"] = str(preset["configuration"])

        return merged

    resolved = resolve(preset_name, ())
    if not resolved["configurePreset"]:
        raise ValueError(f"Test preset '{preset_name}' does not define configurePreset")

    return ResolvedTestPreset(
        name=preset_name,
        configure_preset=resolved["configurePreset"],
        configuration=resolved["configuration"],
    )


def auto_select_build_preset_name(presets: dict[str, Any], configure_preset_name: str) -> str | None:
    for preset in presets.get("buildPresets", []):
        if str(preset.get("configurePreset", "")) == configure_preset_name:
            return str(preset["name"])
    return None


def auto_select_test_preset_name(presets: dict[str, Any], configure_preset_name: str) -> str | None:
    for preset in presets.get("testPresets", []):
        if str(preset.get("configurePreset", "")) == configure_preset_name:
            return str(preset["name"])
    return None


def read_cmake_cache(cache_path: Path) -> dict[str, str]:
    cache_values: dict[str, str] = {}
    for line in cache_path.read_text(encoding="utf-8", errors="ignore").splitlines():
        if not line or line.startswith("//") or line.startswith("#"):
            continue
        key_and_type, separator, value = line.partition("=")
        if not separator:
            continue
        key, _, _cache_type = key_and_type.partition(":")
        if key:
            cache_values[key] = value
    return cache_values


def cmake_cache_matches_request(
    cache_path: Path,
    source_dir: Path,
    configure_preset: ResolvedConfigurePreset,
    effective_cache_variables: dict[str, str],
) -> bool:
    cache_values = read_cmake_cache(cache_path)

    cached_home = cache_values.get("CMAKE_HOME_DIRECTORY")
    if cached_home is None or normalize_cache_value(cached_home) != normalize_cache_value(str(source_dir)):
        return False

    if configure_preset.generator is not None:
        cached_generator = cache_values.get("CMAKE_GENERATOR")
        if cached_generator is None or normalize_cache_value(cached_generator) != normalize_cache_value(configure_preset.generator):
            return False

    if configure_preset.architecture is not None:
        cached_architecture = cache_values.get("CMAKE_GENERATOR_PLATFORM")
        if cached_architecture and normalize_cache_value(cached_architecture) != normalize_cache_value(configure_preset.architecture):
            return False

    for key, value in effective_cache_variables.items():
        cached_value = cache_values.get(key)
        if cached_value is None or normalize_cache_value(cached_value) != normalize_cache_value(value):
            return False

    return True


def append_metrics_row(csv_path: Path, row: dict[str, str | int | float]) -> None:
    csv_path.parent.mkdir(parents=True, exist_ok=True)

    fieldnames = [
        "timestamp_utc",
        "run_label",
        "phase",
        "status",
        "duration_seconds",
        "returncode",
        "command",
        "cwd",
        "source_dir",
        "build_dir",
        "configure_preset",
        "build_preset",
        "test_preset",
        "config",
        "target",
        "clean_first",
        "reconfigure",
        "fresh",
        "ctest_requested",
        "package_requested",
        "cache_state",
        "notes",
    ]

    file_exists = csv_path.exists()
    with csv_path.open("a", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=fieldnames)
        if not file_exists:
            writer.writeheader()
        writer.writerow(row)


def log_phase_metrics(
    metrics_csv: Path | None,
    run_label: str,
    phase: str,
    result: TimedCommandResult | None,
    *,
    status: str,
    source_dir: Path,
    build_dir: Path,
    configure_preset_name: str,
    build_preset_name: str | None,
    test_preset_name: str | None,
    config: str,
    target: str,
    clean_first: bool,
    reconfigure: bool,
    fresh: bool,
    ctest_requested: bool,
    package_requested: bool,
    cache_state: str,
    notes: str = "",
    returncode_override: int | None = None,
) -> None:
    if metrics_csv is None:
        return

    append_metrics_row(
        metrics_csv,
        {
            "timestamp_utc": result.started_utc if result is not None else datetime.now(timezone.utc).isoformat(),
            "run_label": run_label,
            "phase": phase,
            "status": status,
            "duration_seconds": f"{(result.duration_seconds if result is not None else 0.0):.6f}",
            "returncode": returncode_override if returncode_override is not None else (result.returncode if result is not None else ""),
            "command": stringify_command(result.command) if result is not None else "",
            "cwd": str(result.cwd) if result is not None else "",
            "source_dir": str(source_dir),
            "build_dir": str(build_dir),
            "configure_preset": configure_preset_name,
            "build_preset": build_preset_name or "",
            "test_preset": test_preset_name or "",
            "config": config,
            "target": target,
            "clean_first": int(clean_first),
            "reconfigure": int(reconfigure),
            "fresh": int(fresh),
            "ctest_requested": int(ctest_requested),
            "package_requested": int(package_requested),
            "cache_state": cache_state,
            "notes": notes,
        },
    )


def build_effective_cache_variables(
    configure_preset: ResolvedConfigurePreset,
    args: argparse.Namespace,
) -> dict[str, str]:
    effective = dict(configure_preset.cache_variables)

    if args.sdl3_source_dir is not None:
        effective["Don_Craft_SDL3_SOURCE_DIR"] = normalize_optional_cache_path(args.sdl3_source_dir)
    if args.steamworks_sdk_root is not None:
        effective["STEAMWORKS_SDK_ROOT"] = normalize_optional_cache_path(args.steamworks_sdk_root)
    if args.build_client:
        effective["Don_Craft_BUILD_CLIENT"] = "ON"
    elif args.no_build_client:
        effective["Don_Craft_BUILD_CLIENT"] = "OFF"
    if args.build_server:
        effective["Don_Craft_BUILD_SERVER"] = "ON"
    elif args.no_build_server:
        effective["Don_Craft_BUILD_SERVER"] = "OFF"
    if args.build_launcher:
        effective["Don_Craft_BUILD_LAUNCHER"] = "ON"
    elif args.no_build_launcher:
        effective["Don_Craft_BUILD_LAUNCHER"] = "OFF"
    if args.enable_tests:
        effective["Don_Craft_ENABLE_TESTS"] = "ON"
    elif args.disable_tests:
        effective["Don_Craft_ENABLE_TESTS"] = "OFF"
    if args.create_local_steam_appid:
        effective["Don_Craft_CREATE_LOCAL_STEAM_APPID"] = "ON"
    elif args.no_local_steam_appid:
        effective["Don_Craft_CREATE_LOCAL_STEAM_APPID"] = "OFF"

    for define in args.define:
        name, separator, value = define.partition("=")
        if not separator or not name:
            raise ValueError(f"Expected --define entries in VAR=value form, got: {define}")
        effective[name] = value

    return effective


def build_override_defines(
    configure_preset: ResolvedConfigurePreset,
    effective_cache_variables: dict[str, str],
) -> list[str]:
    defines: list[str] = []
    for key in sorted(effective_cache_variables):
        preset_value = configure_preset.cache_variables.get(key)
        effective_value = effective_cache_variables[key]
        if preset_value != effective_value:
            defines.append(f"-D{key}={effective_value}")
    return defines


def configure_if_needed(
    cmake: str,
    source_dir: Path,
    configure_preset: ResolvedConfigurePreset,
    override_defines: list[str],
    effective_cache_variables: dict[str, str],
    *,
    reconfigure: bool,
    fresh: bool,
) -> tuple[TimedCommandResult | None, str]:
    cache_path = configure_preset.binary_dir / "CMakeCache.txt"
    cache_state = "no_cache"

    if cache_path.exists():
        cache_state = "cache_miss"
        if fresh:
            print("CMake configure cache will be ignored because --fresh was requested.")
        elif reconfigure:
            print("CMake configure will run again because --reconfigure was requested.")
        elif cmake_cache_matches_request(
            cache_path=cache_path,
            source_dir=source_dir,
            configure_preset=configure_preset,
            effective_cache_variables=effective_cache_variables,
        ):
            print("CMake configure step skipped: matching cache already present.")
            return None, "cache_hit"
        else:
            print("Cached CMake settings differ from the requested arguments; reconfiguring.")

    configure_preset.binary_dir.mkdir(parents=True, exist_ok=True)
    command = [cmake]
    if fresh:
        command.append("--fresh")
    command.extend(["--preset", configure_preset.name])
    command.extend(override_defines)
    return run_command(command, cwd=source_dir), cache_state


def build_target(
    cmake: str,
    source_dir: Path,
    build_preset_name: str | None,
    build_dir: Path,
    config: str,
    targets: list[str] | None,
    clean_first: bool,
) -> TimedCommandResult:
    if build_preset_name:
        command = [cmake, "--build", "--preset", build_preset_name]
    else:
        command = [cmake, "--build", str(build_dir)]
        if config:
            command.extend(["--config", config])
    if targets:
        command.extend(["--target", *targets])
    if clean_first:
        command.append("--clean-first")
    return run_command(command, cwd=source_dir)


def run_ctest_with_preset(ctest: str, source_dir: Path, test_preset_name: str) -> TimedCommandResult:
    command = [ctest, "--preset", test_preset_name]
    return run_command(command, cwd=source_dir)


def run_ctest_with_build_dir(ctest: str, build_dir: Path, config: str) -> TimedCommandResult:
    command = [ctest, "--test-dir", str(build_dir), "--output-on-failure"]
    if config:
        command.extend(["-C", config])
    return run_command(command, cwd=build_dir)


def infer_targets(
    requested_target: str | None,
    build_preset: ResolvedBuildPreset | None,
    effective_cache_variables: dict[str, str],
) -> list[str]:
    if requested_target:
        return [requested_target]
    if build_preset is not None and build_preset.targets:
        return list(build_preset.targets)

    inferred_targets: list[str] = []
    if effective_cache_variables.get("Don_Craft_BUILD_CLIENT", "ON") == "ON":
        inferred_targets.append(CLIENT_TARGET)
    if effective_cache_variables.get("Don_Craft_BUILD_SERVER", "ON") == "ON":
        inferred_targets.append(SERVER_TARGET)
    if inferred_targets:
        return inferred_targets
    if effective_cache_variables.get("Don_Craft_ENABLE_TESTS", "ON") == "ON":
        return ["Don_Craft_selfcheck"]
    return []


def find_built_executable(build_dir: Path, config: str, target: str) -> Path | None:
    executable_names = [f"{target}.exe"] if os.name == "nt" else [target]
    candidate_dirs = [
        build_dir / "bin" / config,
        build_dir / config,
        build_dir / "bin",
        build_dir,
    ]

    for directory in candidate_dirs:
        for executable_name in executable_names:
            candidate = directory / executable_name
            if candidate.exists():
                return candidate

    for executable_name in executable_names:
        matches = sorted(build_dir.rglob(executable_name))
        if matches:
            return matches[0]

    return None


def copy_runtime_file(source: Path, destination: Path) -> None:
    if not source.exists():
        raise FileNotFoundError(f"Required runtime file was not found: {source.name}")
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source, destination)


def copy_runtime_file_if_present(source: Path, destination: Path) -> bool:
    if not source.exists():
        return False
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source, destination)
    return True


def copy_steam_appid_file_if_present(source: Path, destination: Path) -> bool:
    if not source.exists():
        return False

    appid_text = source.read_bytes().replace(b"\r\n", b"\n").replace(b"\r", b"\n")
    if not appid_text.endswith(b"\n"):
        appid_text += b"\n"

    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_bytes(appid_text)
    return True


def path_is_relative_to(path: Path, parent: Path) -> bool:
    try:
        path.resolve().relative_to(parent.resolve())
        return True
    except ValueError:
        return False


def normalize_glob_patterns(patterns: list[str] | tuple[str, ...]) -> list[str]:
    normalized: list[str] = []
    for pattern in patterns:
        text = str(pattern).strip().replace("\\", "/")
        if not text:
            continue
        while text.startswith("./"):
            text = text[2:]
        normalized.append(text)
    return normalized


def path_matches_any_glob(relative_path: str, patterns: list[str]) -> bool:
    normalized = relative_path.replace("\\", "/").lstrip("/")
    basename = Path(normalized).name
    for pattern in patterns:
        if fnmatch(normalized, pattern) or fnmatch(basename, pattern):
            return True
    return False


def should_stage_package_file(relative_path: str, options: PackageOptions) -> bool:
    normalized = relative_path.replace("\\", "/").lstrip("/")
    if normalized == "":
        return False

    if not options.include_steam_appid and normalized.lower() == "steam_appid.txt":
        return False

    if path_matches_any_glob(normalized, options.exclude_globs):
        return False

    if options.include_globs and not path_matches_any_glob(normalized, options.include_globs):
        return False

    return True


def copy_package_file(
    source: Path,
    destination: Path,
    relative_path: str,
    copied_paths: set[str],
    options: PackageOptions,
    *,
    required: bool = False,
) -> PackageCopyStats:
    stats = PackageCopyStats()
    normalized = relative_path.replace("\\", "/").lstrip("/")

    if not should_stage_package_file(normalized, options):
        stats.skipped_files += 1
        return stats

    if normalized in copied_paths:
        stats.duplicate_files += 1
        return stats

    if not source.is_file():
        if required:
            raise FileNotFoundError(f"Required package file was not found: {source}")
        stats.skipped_files += 1
        return stats

    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source, destination)
    copied_paths.add(normalized)
    stats.copied_files += 1
    stats.copied_bytes += destination.stat().st_size

    if options.verbose:
        print(f"  + package {normalized}")

    return stats


def copy_package_tree(
    source_root: Path,
    destination_root: Path,
    copied_paths: set[str],
    options: PackageOptions,
    *,
    label: str,
) -> PackageCopyStats:
    stats = PackageCopyStats()
    if not source_root.exists():
        return stats
    if not source_root.is_dir():
        raise ValueError(f"Package source is not a directory: {source_root}")

    if path_is_relative_to(destination_root, source_root):
        raise ValueError(f"Package destination cannot live inside a package source: {destination_root} inside {source_root}")

    if options.verbose:
        print(f"Packaging {label}: {source_root} -> {destination_root}")

    for source_file in sorted(path for path in source_root.rglob("*") if path.is_file()):
        relative_path = relative_posix(source_file, source_root)
        destination = destination_root / relative_path
        stats.add(copy_package_file(source_file, destination, relative_path, copied_paths, options))

    return stats


def discover_source_payload_dirs(source_dir: Path) -> list[Path]:
    discovered: list[Path] = []
    seen: set[Path] = set()
    for relative in DEFAULT_SOURCE_PAYLOAD_DIRS:
        candidate = (source_dir / relative).resolve()
        if not candidate.is_dir():
            continue
        if candidate in seen:
            continue
        seen.add(candidate)
        discovered.append(candidate)
    return discovered


def copy_source_payload_dir(
    source_root: Path,
    package_dir: Path,
    copied_paths: set[str],
    options: PackageOptions,
) -> PackageCopyStats:
    source_root = source_root.expanduser().resolve()
    if not source_root.exists():
        raise FileNotFoundError(f"Package payload directory was not found: {source_root}")
    if not source_root.is_dir():
        raise ValueError(f"Package payload path is not a directory: {source_root}")

    destination_root = package_dir / source_root.name
    return copy_package_tree(source_root, destination_root, copied_paths, options, label=f"payload:{source_root.name}")


def copy_shader_payload(runtime_dir: Path, package_dir: Path, copied_paths: set[str], options: PackageOptions) -> PackageCopyStats:
    shader_dir = runtime_dir / "shaders"
    if not shader_dir.exists():
        return PackageCopyStats()
    return copy_package_tree(shader_dir, package_dir / "shaders", copied_paths, options, label="filtered shaders")


def stage_runtime_package(
    runtime_dir: Path,
    package_dir: Path,
    target_name: str,
    executable: Path,
    options: PackageOptions,
) -> StagedPackage:
    runtime_dir = runtime_dir.resolve()
    executable = executable.resolve()
    package_dir = package_dir.resolve()

    if runtime_dir == package_dir or path_is_relative_to(package_dir, runtime_dir):
        raise ValueError("Runtime packaging cannot be staged over, or inside, the build output directory.")

    if package_dir.exists():
        shutil.rmtree(package_dir)
    package_dir.mkdir(parents=True, exist_ok=True)

    copied_paths: set[str] = set()
    stats = PackageCopyStats()

    stats.add(copy_package_file(executable, package_dir / executable.name, executable.name, copied_paths, options, required=True))

    if options.mode == "complete":
        stats.add(copy_package_tree(runtime_dir, package_dir, copied_paths, options, label="runtime directory"))

        for extra_root in options.runtime_roots:
            extra_root = extra_root.expanduser().resolve()
            if not extra_root.exists():
                raise FileNotFoundError(f"Additional runtime root was not found: {extra_root}")
            stats.add(copy_package_tree(extra_root, package_dir, copied_paths, options, label="extra runtime root"))

        if options.include_source_payload:
            for payload_dir in discover_source_payload_dirs(options.source_dir):
                if payload_dir.resolve() == runtime_dir:
                    continue
                stats.add(copy_source_payload_dir(payload_dir, package_dir, copied_paths, options))

        for payload_dir in options.payload_dirs:
            stats.add(copy_source_payload_dir(payload_dir, package_dir, copied_paths, options))

    elif options.mode == "filtered":
        if target_name == CLIENT_TARGET:
            stats.add(copy_package_file(runtime_dir / "steam_api64.dll", package_dir / "steam_api64.dll", "steam_api64.dll", copied_paths, options, required=True))
            if options.include_steam_appid:
                stats.add(copy_package_file(runtime_dir / "steam_appid.txt", package_dir / "steam_appid.txt", "steam_appid.txt", copied_paths, options))
            stats.add(copy_package_file(runtime_dir / "SDL3.dll", package_dir / "SDL3.dll", "SDL3.dll", copied_paths, options))
            stats.add(copy_shader_payload(runtime_dir, package_dir, copied_paths, options))
            stats.add(copy_package_file(runtime_dir / "DonCraft.ini", package_dir / "DonCraft.ini", "DonCraft.ini", copied_paths, options))
        elif target_name == SERVER_TARGET:
            stats.add(copy_package_file(runtime_dir / "steam_api64.dll", package_dir / "steam_api64.dll", "steam_api64.dll", copied_paths, options, required=True))
            if options.include_steam_appid:
                stats.add(copy_package_file(runtime_dir / "steam_appid.txt", package_dir / "steam_appid.txt", "steam_appid.txt", copied_paths, options))
        else:
            stats.add(copy_shader_payload(runtime_dir, package_dir, copied_paths, options))
    else:
        raise ValueError(f"Unknown package mode: {options.mode}")

    if not (package_dir / executable.name).exists():
        raise FileNotFoundError(f"Packaged runtime is missing the entrypoint executable: {executable.name}")

    return StagedPackage(path=package_dir, entrypoint=executable.name, stats=stats)


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def run_git_text(source_dir: Path, *args: str) -> str:
    completed = subprocess.run(
        ["git", *args],
        cwd=str(source_dir),
        check=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        encoding="utf-8",
    )
    return completed.stdout.strip()


def infer_github_repo(source_dir: Path, explicit_repo: str) -> str:
    if explicit_repo.strip():
        repo = explicit_repo.strip().removeprefix("https://github.com/").removesuffix(".git")
        if re.fullmatch(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+", repo):
            return repo
        raise ValueError(f"Expected --github-repo in owner/name form, got: {explicit_repo}")

    try:
        remote_url = run_git_text(source_dir, "remote", "get-url", "origin")
    except (subprocess.CalledProcessError, FileNotFoundError):
        return DEFAULT_GITHUB_REPO

    patterns = (
        r"^https://github\.com/([^/]+/[^/]+?)(?:\.git)?$",
        r"^https://[^@]+@github\.com/([^/]+/[^/]+?)(?:\.git)?$",
        r"^git@github\.com:([^/]+/[^/]+?)(?:\.git)?$",
    )
    for pattern in patterns:
        match = re.match(pattern, remote_url)
        if match:
            return match.group(1)

    return DEFAULT_GITHUB_REPO


def git_commit_version(source_dir: Path) -> str:
    try:
        return run_git_text(source_dir, "rev-parse", "--short=12", "HEAD")
    except (subprocess.CalledProcessError, FileNotFoundError):
        return "uncommitted"


def ensure_relative_to_source(path: Path, source_dir: Path) -> Path:
    try:
        return path.resolve().relative_to(source_dir.resolve())
    except ValueError as error:
        raise ValueError(f"Distribution directory must stay inside the repository: {path}") from error


def relative_posix(path: Path, root: Path) -> str:
    return path.relative_to(root).as_posix()


def write_update_channel(
    staged_runtime_dir: Path,
    source_dir: Path,
    dist_dir: Path,
    channel: str,
    github_repo: str,
    branch: str,
    version: str,
    entrypoint: str,
) -> tuple[Path, str, int, int]:
    channel_root = dist_dir / "update" / channel
    files_root = channel_root / "files"
    if channel_root.exists():
        shutil.rmtree(channel_root)
    files_root.mkdir(parents=True, exist_ok=True)

    entries: list[dict[str, str | int]] = []
    total_size = 0
    for source_file in sorted(path for path in staged_runtime_dir.rglob("*") if path.is_file()):
        relative_path = relative_posix(source_file, staged_runtime_dir)
        destination = files_root / relative_path
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source_file, destination)
        file_size = destination.stat().st_size
        total_size += file_size
        entries.append(
            {
                "path": relative_path,
                "size": file_size,
                "sha256": sha256_file(destination),
            }
        )

    if not entries:
        raise ValueError(f"No files were staged for update channel from {staged_runtime_dir}")

    channel_relative = ensure_relative_to_source(channel_root, source_dir).as_posix()
    raw_base_url = f"https://raw.githubusercontent.com/{github_repo}/{branch}/{channel_relative}/files/"
    manifest_url = f"https://raw.githubusercontent.com/{github_repo}/{branch}/{channel_relative}/manifest.json"
    manifest = {
        "schema": 1,
        "product": "DonCraft",
        "channel": channel,
        "version": version,
        "generated_utc": datetime.now(timezone.utc).replace(microsecond=0).isoformat(),
        "base_url": raw_base_url,
        "entrypoint": entrypoint,
        "file_count": len(entries),
        "total_size": total_size,
        "files": entries,
    }
    manifest_path = channel_root / "manifest.json"
    with manifest_path.open("w", encoding="utf-8", newline="\n") as handle:
        handle.write(json.dumps(manifest, indent=2, sort_keys=True) + "\n")
    return manifest_path, manifest_url, len(entries), total_size


def count_tree_files(root: Path) -> tuple[int, int]:
    file_count = 0
    total_size = 0
    if not root.exists():
        return file_count, total_size

    for path in root.rglob("*"):
        if path.is_file():
            file_count += 1
            total_size += path.stat().st_size
    return file_count, total_size


def stage_launcher_distribution(
    build_dir: Path,
    config: str,
    dist_dir: Path,
    bootstrap_runtime: StagedPackage | None,
) -> tuple[Path | None, Path | None, int, int]:
    launcher = find_built_executable(build_dir, config, LAUNCHER_TARGET)
    if launcher is None:
        return None, None, 0, 0

    launcher_dir = dist_dir / "launcher" / "windows-x64"
    if launcher_dir.exists():
        shutil.rmtree(launcher_dir)
    launcher_dir.mkdir(parents=True, exist_ok=True)

    launcher_path = launcher_dir / launcher.name
    shutil.copy2(launcher, launcher_path)

    bootstrap_path: Path | None = None
    bootstrap_files = 0
    bootstrap_bytes = 0
    if bootstrap_runtime is not None:
        bootstrap_path = launcher_dir / "DonCraftBootstrap"
        shutil.copytree(bootstrap_runtime.path, bootstrap_path)
        bootstrap_files, bootstrap_bytes = count_tree_files(bootstrap_path)

    return launcher_path, bootstrap_path, bootstrap_files, bootstrap_bytes


def normalize_optional_cache_path(value: str) -> str:
    if value == "":
        return ""
    return str(Path(value).expanduser().resolve())


def parse_args() -> argparse.Namespace:
    script_path = Path(__file__).resolve()
    source_dir = script_path.parent

    parser = argparse.ArgumentParser(
        description="Configure, build, test, and stage a runtime package for DonCraft.",
    )
    parser.add_argument(
        "--source-dir",
        default=str(source_dir),
        help="CMake source directory. Defaults to the repository root.",
    )
    parser.add_argument(
        "--configure-preset",
        default="vs2022-release",
        help="Configure preset to use. Defaults to vs2022-release.",
    )
    parser.add_argument(
        "--build-preset",
        default="",
        help="Optional explicit build preset. Defaults to the first build preset tied to the configure preset.",
    )
    parser.add_argument(
        "--test-preset",
        default="",
        help="Optional explicit test preset. Defaults to the first test preset tied to the configure preset.",
    )
    parser.add_argument(
        "--config",
        default="",
        help="Optional build configuration override when not using a build preset. Also used for package path naming if needed.",
    )
    parser.add_argument(
        "--target",
        default="",
        help="Optional explicit build target. Defaults to the build preset target, Don_Craft_client, or Don_Craft_selfcheck.",
    )
    parser.add_argument(
        "--reconfigure",
        action="store_true",
        help="Force a CMake configure pass even if the existing cache matches the requested arguments.",
    )
    parser.add_argument(
        "--fresh",
        "--no-cache",
        action="store_true",
        dest="fresh",
        help="Run CMake configure with --fresh so the existing configure cache is not reused.",
    )
    parser.add_argument(
        "--clean-first",
        action="store_true",
        help="Pass --clean-first to the build step.",
    )
    parser.add_argument(
        "--ctest",
        action="store_true",
        help="Run ctest after the build completes.",
    )
    parser.add_argument(
        "--configure-only",
        action="store_true",
        help="Only run configure. Skip build, tests, and packaging.",
    )
    parser.add_argument(
        "--sdl3-source-dir",
        default=None,
        help="Override Don_Craft_SDL3_SOURCE_DIR for this run.",
    )
    parser.add_argument(
        "--steamworks-sdk-root",
        default=None,
        help="Override STEAMWORKS_SDK_ROOT for this run.",
    )
    client_group = parser.add_mutually_exclusive_group()
    client_group.add_argument(
        "--build-client",
        action="store_true",
        help="Force Don_Craft_BUILD_CLIENT=ON for this run.",
    )
    client_group.add_argument(
        "--no-build-client",
        action="store_true",
        help="Force Don_Craft_BUILD_CLIENT=OFF for this run.",
    )
    server_group = parser.add_mutually_exclusive_group()
    server_group.add_argument(
        "--build-server",
        action="store_true",
        help="Force Don_Craft_BUILD_SERVER=ON for this run.",
    )
    server_group.add_argument(
        "--no-build-server",
        action="store_true",
        help="Force Don_Craft_BUILD_SERVER=OFF for this run.",
    )
    launcher_group = parser.add_mutually_exclusive_group()
    launcher_group.add_argument(
        "--build-launcher",
        action="store_true",
        help="Force Don_Craft_BUILD_LAUNCHER=ON for this run.",
    )
    launcher_group.add_argument(
        "--no-build-launcher",
        action="store_true",
        help="Force Don_Craft_BUILD_LAUNCHER=OFF for this run.",
    )
    tests_group = parser.add_mutually_exclusive_group()
    tests_group.add_argument(
        "--enable-tests",
        action="store_true",
        help="Force Don_Craft_ENABLE_TESTS=ON for this run.",
    )
    tests_group.add_argument(
        "--disable-tests",
        action="store_true",
        help="Force Don_Craft_ENABLE_TESTS=OFF for this run.",
    )
    appid_group = parser.add_mutually_exclusive_group()
    appid_group.add_argument(
        "--create-local-steam-appid",
        action="store_true",
        help="Force Don_Craft_CREATE_LOCAL_STEAM_APPID=ON for this run.",
    )
    appid_group.add_argument(
        "--no-local-steam-appid",
        action="store_true",
        help="Force Don_Craft_CREATE_LOCAL_STEAM_APPID=OFF for this run.",
    )
    parser.add_argument(
        "--define",
        action="append",
        default=[],
        metavar="VAR=value",
        help="Additional CMake cache variable override. May be repeated.",
    )
    parser.add_argument(
        "--metrics-csv",
        default=str(source_dir / "build" / "compile_metrics.csv"),
        help="CSV file used to append timing metrics. Defaults to build/compile_metrics.csv.",
    )
    parser.add_argument(
        "--no-metrics",
        action="store_true",
        help="Disable CSV metrics logging.",
    )
    parser.add_argument(
        "--run-label",
        default="",
        help="Optional label written to the metrics CSV for grouping related runs.",
    )
    parser.add_argument(
        "--package-dir",
        default="",
        help="Optional staged runtime package directory. Defaults to build/package/<config>/<target>.",
    )
    parser.add_argument(
        "--no-package",
        action="store_true",
        help="Skip staging a packaged runtime directory after the build completes.",
    )
    parser.add_argument(
        "--package-mode",
        choices=("complete", "filtered"),
        default=DEFAULT_PACKAGE_MODE,
        help="Runtime packaging mode. filtered stages only the files needed by the selected target; complete mirrors the playable runtime folder for diagnostics.",
    )
    parser.add_argument(
        "--package-runtime-root",
        action="append",
        default=[],
        help="Additional directory whose contents are copied into the package root. May be repeated.",
    )
    parser.add_argument(
        "--package-payload-dir",
        action="append",
        default=[],
        help="Additional payload directory copied into the package under its folder name. May be repeated.",
    )
    parser.add_argument(
        "--no-source-payload",
        action="store_true",
        help="Do not auto-copy source payload folders such as assets/, data/, resources/, config/, or shaders/.",
    )
    parser.add_argument(
        "--package-include-glob",
        action="append",
        default=[],
        help="Only include package files matching this glob. May be repeated. Empty means include everything except excludes.",
    )
    parser.add_argument(
        "--package-exclude-glob",
        action="append",
        default=[],
        help="Exclude package files matching this glob. May be repeated and extends the default excludes.",
    )
    parser.add_argument(
        "--include-steam-appid",
        action="store_true",
        help="Include steam_appid.txt in packaged/update output. Off by default so public launcher builds do not ship the dev AppID file.",
    )
    parser.add_argument(
        "--verbose-package",
        action="store_true",
        help="Print every file copied into the staged runtime package.",
    )
    parser.add_argument(
        "--dist-dir",
        default=str(source_dir / "dist"),
        help="Tracked distribution directory for the launcher and GitHub update channel. Defaults to dist/.",
    )
    parser.add_argument(
        "--update-channel",
        default=DEFAULT_UPDATE_CHANNEL,
        help=f"Update channel directory name under dist/update/. Defaults to {DEFAULT_UPDATE_CHANNEL}.",
    )
    parser.add_argument(
        "--update-branch",
        default=DEFAULT_UPDATE_BRANCH,
        help=f"GitHub branch used for raw update URLs. Defaults to {DEFAULT_UPDATE_BRANCH}.",
    )
    parser.add_argument(
        "--github-repo",
        default="",
        help=f"GitHub repository in owner/name form. Defaults to origin, then {DEFAULT_GITHUB_REPO}.",
    )
    bootstrap_group = parser.add_mutually_exclusive_group()
    bootstrap_group.add_argument(
        "--launcher-bootstrap-runtime",
        dest="launcher_bootstrap_runtime",
        action="store_true",
        help="Copy the packaged client runtime into dist/launcher/windows-x64/DonCraftBootstrap. Off by default.",
    )
    bootstrap_group.add_argument(
        "--no-launcher-bootstrap-runtime",
        dest="launcher_bootstrap_runtime",
        action="store_false",
        help="Do not copy a client runtime bootstrap beside the launcher. This is the default.",
    )
    parser.set_defaults(launcher_bootstrap_runtime=False)
    parser.add_argument(
        "--no-dist",
        action="store_true",
        help="Skip writing dist/launcher and dist/update artifacts.",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    source_dir = Path(args.source_dir).resolve()
    metrics_csv = None if args.no_metrics else Path(args.metrics_csv).resolve()
    dist_dir = Path(args.dist_dir).expanduser().resolve()
    github_repo = DEFAULT_GITHUB_REPO

    if not (source_dir / "CMakeLists.txt").exists():
        print(f"Source directory does not contain CMakeLists.txt: {source_dir}", file=sys.stderr)
        return 1
    if not (source_dir / "CMakePresets.json").exists():
        print(f"Source directory does not contain CMakePresets.json: {source_dir}", file=sys.stderr)
        return 1

    try:
        if not args.no_dist and not args.configure_only:
            github_repo = infer_github_repo(source_dir, args.github_repo)
            ensure_relative_to_source(dist_dir, source_dir)
        cmake = find_cmake()
        presets = load_cmake_presets(source_dir)
        configure_preset = resolve_configure_preset(presets, source_dir, args.configure_preset)
    except (FileNotFoundError, KeyError, ValueError, json.JSONDecodeError) as error:
        print(str(error), file=sys.stderr)
        return 1

    build_preset_name = args.build_preset.strip() or auto_select_build_preset_name(presets, configure_preset.name)
    build_preset: ResolvedBuildPreset | None = None
    if build_preset_name:
        try:
            build_preset = resolve_build_preset(presets, build_preset_name)
        except (KeyError, ValueError) as error:
            print(str(error), file=sys.stderr)
            return 1
        if build_preset.configure_preset != configure_preset.name:
            print(
                f"Build preset '{build_preset.name}' is bound to configure preset '{build_preset.configure_preset}', not '{configure_preset.name}'.",
                file=sys.stderr,
            )
            return 1

    test_preset_name = args.test_preset.strip() or auto_select_test_preset_name(presets, configure_preset.name)
    test_preset: ResolvedTestPreset | None = None
    if test_preset_name:
        try:
            test_preset = resolve_test_preset(presets, test_preset_name)
        except (KeyError, ValueError) as error:
            print(str(error), file=sys.stderr)
            return 1
        if test_preset.configure_preset != configure_preset.name:
            print(
                f"Test preset '{test_preset.name}' is bound to configure preset '{test_preset.configure_preset}', not '{configure_preset.name}'.",
                file=sys.stderr,
            )
            return 1

    try:
        effective_cache_variables = build_effective_cache_variables(configure_preset, args)
    except ValueError as error:
        print(str(error), file=sys.stderr)
        return 1

    if args.ctest and effective_cache_variables.get("Don_Craft_ENABLE_TESTS", "ON") != "ON":
        print("ctest was requested, but Don_Craft_ENABLE_TESTS is OFF for this run.", file=sys.stderr)
        return 1

    override_defines = build_override_defines(configure_preset, effective_cache_variables)
    requested_target = args.target.strip() or None
    primary_targets = infer_targets(requested_target, build_preset, effective_cache_variables)
    build_targets = [requested_target] if requested_target else (list(build_preset.targets) if build_preset and build_preset.targets else [])
    target_display = ", ".join(primary_targets) if primary_targets else "default"
    config = args.config.strip() or (build_preset.configuration if build_preset and build_preset.configuration else "")
    if not config and test_preset and test_preset.configuration:
        config = test_preset.configuration
    if not config:
        config = "Release"

    package_requested = not args.no_package and not args.configure_only and bool(primary_targets)
    dist_requested = not args.no_dist and not args.configure_only

    run_label = args.run_label.strip()
    if not run_label:
        parts = [
            configure_preset.name,
            "ctest" if args.ctest else "build",
            "fresh" if args.fresh else ("reconfig" if args.reconfigure else "cached"),
            "clean" if args.clean_first else "incremental",
        ]
        run_label = "-".join(parts)

    print(f"Source dir: {source_dir}")
    print(f"Configure preset: {configure_preset.name}")
    print(f"Build preset: {build_preset.name if build_preset is not None else 'none (raw build mode)'}")
    print(f"Test preset: {test_preset.name if test_preset is not None else 'none (raw ctest mode)'}")
    print(f"Build dir: {configure_preset.binary_dir}")
    print(f"Config: {config}")
    print(f"Target: {target_display}")
    print(f"Metrics CSV: {metrics_csv if metrics_csv is not None else 'disabled'}")
    print(f"Distribution dir: {dist_dir if dist_requested else 'disabled'}")
    if package_requested:
        print(f"Package mode: {args.package_mode}")
        source_payload_enabled = args.package_mode == "complete" and not args.no_source_payload
        print(f"Source payload discovery: {'enabled' if source_payload_enabled else 'disabled'}")
    if dist_requested:
        print(f"GitHub update channel: {github_repo}@{args.update_branch}/{args.update_channel}")
        print(f"Launcher bootstrap runtime: {'enabled' if args.launcher_bootstrap_runtime else 'disabled'}")
    print(f"Run label: {run_label}")
    if override_defines:
        print("Override cache variables:")
        for define in override_defines:
            print(f"  {define}")
    else:
        print("Override cache variables: none")

    overall_started_utc = datetime.now(timezone.utc).isoformat()
    overall_started_perf = time.perf_counter()
    cache_state = "unknown"

    try:
        configure_result, cache_state = configure_if_needed(
            cmake=cmake,
            source_dir=source_dir,
            configure_preset=configure_preset,
            override_defines=override_defines,
            effective_cache_variables=effective_cache_variables,
            reconfigure=args.reconfigure,
            fresh=args.fresh,
        )

        if configure_result is None:
            log_phase_metrics(
                metrics_csv,
                run_label,
                "configure",
                None,
                status="skipped",
                source_dir=source_dir,
                build_dir=configure_preset.binary_dir,
                configure_preset_name=configure_preset.name,
                build_preset_name=build_preset.name if build_preset is not None else None,
                test_preset_name=test_preset.name if test_preset is not None else None,
                config=config,
                target=",".join(primary_targets),
                clean_first=args.clean_first,
                reconfigure=args.reconfigure,
                fresh=args.fresh,
                ctest_requested=args.ctest,
                package_requested=package_requested,
                cache_state=cache_state,
                notes="matching CMake cache reused",
            )
        else:
            log_phase_metrics(
                metrics_csv,
                run_label,
                "configure",
                configure_result,
                status="ok",
                source_dir=source_dir,
                build_dir=configure_preset.binary_dir,
                configure_preset_name=configure_preset.name,
                build_preset_name=build_preset.name if build_preset is not None else None,
                test_preset_name=test_preset.name if test_preset is not None else None,
                config=config,
                target=",".join(primary_targets),
                clean_first=args.clean_first,
                reconfigure=args.reconfigure,
                fresh=args.fresh,
                ctest_requested=args.ctest,
                package_requested=package_requested,
                cache_state=cache_state,
            )

        if not args.configure_only:
            build_result = build_target(
                cmake=cmake,
                source_dir=source_dir,
                build_preset_name=build_preset.name if build_preset is not None else None,
                build_dir=configure_preset.binary_dir,
                config=config,
                targets=build_targets or None,
                clean_first=args.clean_first,
            )
            log_phase_metrics(
                metrics_csv,
                run_label,
                "build",
                build_result,
                status="ok",
                source_dir=source_dir,
                build_dir=configure_preset.binary_dir,
                configure_preset_name=configure_preset.name,
                build_preset_name=build_preset.name if build_preset is not None else None,
                test_preset_name=test_preset.name if test_preset is not None else None,
                config=config,
                target=",".join(primary_targets),
                clean_first=args.clean_first,
                reconfigure=args.reconfigure,
                fresh=args.fresh,
                ctest_requested=args.ctest,
                package_requested=package_requested,
                cache_state=cache_state,
            )

            if args.ctest:
                ctest = find_ctest(cmake)
                if test_preset is not None:
                    test_result = run_ctest_with_preset(ctest, source_dir, test_preset.name)
                else:
                    test_result = run_ctest_with_build_dir(ctest, configure_preset.binary_dir, config)
                log_phase_metrics(
                    metrics_csv,
                    run_label,
                    "ctest",
                    test_result,
                    status="ok",
                    source_dir=source_dir,
                    build_dir=configure_preset.binary_dir,
                    configure_preset_name=configure_preset.name,
                    build_preset_name=build_preset.name if build_preset is not None else None,
                    test_preset_name=test_preset.name if test_preset is not None else None,
                    config=config,
                    target=",".join(primary_targets),
                    clean_first=args.clean_first,
                    reconfigure=args.reconfigure,
                    fresh=args.fresh,
                    ctest_requested=args.ctest,
                    package_requested=package_requested,
                    cache_state=cache_state,
                )
    except subprocess.CalledProcessError as error:
        failed_duration = time.perf_counter() - overall_started_perf
        failed_command = TimedCommandResult(
            command=[str(part) for part in error.cmd] if isinstance(error.cmd, (list, tuple)) else [str(error.cmd)],
            cwd=configure_preset.binary_dir,
            started_utc=overall_started_utc,
            duration_seconds=failed_duration,
            returncode=error.returncode,
        )
        log_phase_metrics(
            metrics_csv,
            run_label,
            "failed",
            failed_command,
            status="failed",
            source_dir=source_dir,
            build_dir=configure_preset.binary_dir,
            configure_preset_name=configure_preset.name,
            build_preset_name=build_preset.name if build_preset is not None else None,
            test_preset_name=test_preset.name if test_preset is not None else None,
            config=config,
            target=",".join(primary_targets),
            clean_first=args.clean_first,
            reconfigure=args.reconfigure,
            fresh=args.fresh,
            ctest_requested=args.ctest,
            package_requested=package_requested,
            cache_state=cache_state,
            notes="command failed",
            returncode_override=error.returncode,
        )
        return error.returncode
    except FileNotFoundError as error:
        print(str(error), file=sys.stderr)
        return 1

    total_duration = time.perf_counter() - overall_started_perf
    total_result = TimedCommandResult(
        command=["<overall>"],
        cwd=configure_preset.binary_dir,
        started_utc=overall_started_utc,
        duration_seconds=total_duration,
        returncode=0,
    )
    log_phase_metrics(
        metrics_csv,
        run_label,
        "overall",
        total_result,
        status="ok",
        source_dir=source_dir,
        build_dir=configure_preset.binary_dir,
        configure_preset_name=configure_preset.name,
        build_preset_name=build_preset.name if build_preset is not None else None,
        test_preset_name=test_preset.name if test_preset is not None else None,
        config=config,
        target=",".join(primary_targets),
        clean_first=args.clean_first,
        reconfigure=args.reconfigure,
        fresh=args.fresh,
        ctest_requested=args.ctest,
        package_requested=package_requested,
        cache_state=cache_state,
    )

    if args.configure_only:
        print(f"Configure completed in {total_duration:.3f}s")
        return 0

    if not primary_targets:
        print(f"Build completed in {total_duration:.3f}s")
        return 0

    client_package: StagedPackage | None = None
    package_options = PackageOptions(
        source_dir=source_dir,
        mode=args.package_mode,
        include_source_payload=not args.no_source_payload,
        include_steam_appid=args.include_steam_appid,
        payload_dirs=[Path(path).expanduser().resolve() for path in args.package_payload_dir],
        runtime_roots=[Path(path).expanduser().resolve() for path in args.package_runtime_root],
        include_globs=normalize_glob_patterns(args.package_include_glob),
        exclude_globs=normalize_glob_patterns([*DEFAULT_PACKAGE_EXCLUDE_GLOBS, *args.package_exclude_glob]),
        verbose=args.verbose_package,
    )

    for target_name in primary_targets:
        executable = find_built_executable(configure_preset.binary_dir, config, target_name)
        if executable is None:
            print(f"Build finished, but the executable path for '{target_name}' could not be resolved automatically.", file=sys.stderr)
            return 2

        print(f"Built executable ({target_name}): {executable}")

        if package_requested:
            if args.package_dir:
                base_package_dir = Path(args.package_dir).expanduser().resolve()
                package_dir = base_package_dir if len(primary_targets) == 1 else base_package_dir / target_name
            else:
                package_dir = source_dir / "build" / "package" / config / target_name
            try:
                staged_package = stage_runtime_package(executable.parent, package_dir, target_name, executable, package_options)
            except (FileNotFoundError, ValueError) as error:
                print(str(error), file=sys.stderr)
                return 1
            print(
                f"Packaged runtime ({target_name}): {staged_package.path} "
                f"({staged_package.stats.copied_files} files, {staged_package.stats.copied_bytes} bytes, "
                f"{staged_package.stats.skipped_files} skipped, {staged_package.stats.duplicate_files} duplicates)"
            )
            if target_name == CLIENT_TARGET:
                client_package = staged_package

    if dist_requested:
        bootstrap_runtime = client_package if args.launcher_bootstrap_runtime else None
        launcher_path, launcher_bootstrap_path, launcher_bootstrap_files, launcher_bootstrap_bytes = stage_launcher_distribution(
            configure_preset.binary_dir,
            config,
            dist_dir,
            bootstrap_runtime,
        )
        if launcher_path is not None:
            print(f"Launcher distribution: {launcher_path}")
            if launcher_bootstrap_path is not None:
                print(
                    f"Launcher bootstrap runtime: {launcher_bootstrap_path} "
                    f"({launcher_bootstrap_files} files, {launcher_bootstrap_bytes} bytes)"
                )
            elif args.launcher_bootstrap_runtime:
                print("Launcher bootstrap runtime skipped: client runtime package was not built.")
        else:
            print("Launcher distribution skipped: DonCraftLauncher executable was not found.")

        if client_package is not None:
            try:
                manifest_path, manifest_url, manifest_file_count, manifest_total_size = write_update_channel(
                    staged_runtime_dir=client_package.path,
                    source_dir=source_dir,
                    dist_dir=dist_dir,
                    channel=args.update_channel.strip() or DEFAULT_UPDATE_CHANNEL,
                    github_repo=github_repo,
                    branch=args.update_branch.strip() or DEFAULT_UPDATE_BRANCH,
                    version=git_commit_version(source_dir),
                    entrypoint=client_package.entrypoint,
                )
            except (OSError, ValueError, subprocess.CalledProcessError) as error:
                print(str(error), file=sys.stderr)
                return 1
            print(f"GitHub update manifest: {manifest_path}")
            print(f"GitHub update files: {manifest_file_count} files, {manifest_total_size} bytes")
            print(f"Launcher manifest URL: {manifest_url}")
        elif package_requested:
            print("GitHub update channel skipped: client runtime package was not built.")

    print(f"Total Time Taken: {total_duration:.3f}s")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
