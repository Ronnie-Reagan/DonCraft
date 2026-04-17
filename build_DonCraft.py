from __future__ import annotations

import argparse
import csv
import json
import os
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
        inferred_targets.append("Don_Craft_client")
    if effective_cache_variables.get("Don_Craft_BUILD_SERVER", "ON") == "ON":
        inferred_targets.append("Don_Craft_server")
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


def stage_runtime_package(runtime_dir: Path, package_dir: Path) -> Path:
    if runtime_dir.resolve() == package_dir.resolve():
        return runtime_dir

    if package_dir.exists():
        shutil.rmtree(package_dir)
    package_dir.parent.mkdir(parents=True, exist_ok=True)
    shutil.copytree(runtime_dir, package_dir)
    packaged_steam_appid = package_dir / "steam_appid.txt"
    if packaged_steam_appid.exists():
        packaged_steam_appid.unlink()
    return package_dir


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
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    source_dir = Path(args.source_dir).resolve()
    metrics_csv = None if args.no_metrics else Path(args.metrics_csv).resolve()

    if not (source_dir / "CMakeLists.txt").exists():
        print(f"Source directory does not contain CMakeLists.txt: {source_dir}", file=sys.stderr)
        return 1
    if not (source_dir / "CMakePresets.json").exists():
        print(f"Source directory does not contain CMakePresets.json: {source_dir}", file=sys.stderr)
        return 1

    try:
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
            staged_dir = stage_runtime_package(executable.parent, package_dir)
            print(f"Packaged runtime ({target_name}): {staged_dir}")

    print(f"Total Time Taken: {total_duration:.3f}s")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
