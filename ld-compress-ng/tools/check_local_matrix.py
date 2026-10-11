#!/usr/bin/env python3
"""Run the local ld-compress-ng validation matrix."""

from __future__ import annotations

import argparse
import shlex
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path


DEFAULT_REAL_FIXTURE_DIR = (
    "reference/testdata/ld-decode-testdata-ci/"
    "1cf698d2025e8515e9ef57e34adaf85a194da96a"
)
DEFAULT_FLAC_TEST_FILE_DIR = "reference/flac-test-files"


@dataclass(frozen=True)
class Step:
    label: str
    command: list[str]


def project_root() -> Path:
    return Path(__file__).resolve().parents[1]


def project_path(root: Path, path: Path) -> Path:
    return path if path.is_absolute() else root / path


def display_path(root: Path, path: Path) -> str:
    try:
        return path.relative_to(root).as_posix()
    except ValueError:
        return str(path)


def cmake_configure(
    build_dir: Path,
    build_type: str,
    cache_args: list[str],
) -> list[str]:
    return [
        "cmake",
        "-S",
        ".",
        "-B",
        str(build_dir),
        f"-DCMAKE_BUILD_TYPE={build_type}",
        *cache_args,
    ]


def cmake_build(build_dir: Path, jobs: str | None) -> list[str]:
    command = ["cmake", "--build", str(build_dir), "--parallel"]
    if jobs is not None:
        command.append(jobs)
    return command


def ctest(build_dir: Path, *args: str) -> list[str]:
    return ["ctest", "--test-dir", str(build_dir), *args, "--output-on-failure"]


def add_configured_suite(
    steps: list[Step],
    name: str,
    build_dir: Path,
    build_type: str,
    jobs: str | None,
    cache_args: list[str],
    ctest_args: list[str],
) -> None:
    steps.extend(
        [
            Step(f"{name}: configure", cmake_configure(build_dir, build_type, cache_args)),
            Step(f"{name}: build", cmake_build(build_dir, jobs)),
            Step(f"{name}: test", ctest(build_dir, *ctest_args)),
        ]
    )


def optional_dir(
    root: Path,
    requested: bool,
    strict: bool,
    path: Path,
    label: str,
) -> Path | None:
    resolved = project_path(root, path)
    if resolved.is_dir():
        return resolved
    if requested and strict:
        raise RuntimeError(f"{label} directory not found: {resolved}")
    if requested:
        print(f"SKIP {label}: directory not found: {display_path(root, resolved)}")
    return None


def python_cache_args(root: Path, python_executable: Path | None) -> list[str]:
    if python_executable is None:
        return []
    resolved = project_path(root, python_executable)
    if not resolved.is_file():
        raise RuntimeError(f"Python executable not found: {resolved}")
    return [f"-DPython3_EXECUTABLE={resolved}"]


def build_steps(args: argparse.Namespace) -> list[Step]:
    root = project_root()
    build_root = project_path(root, args.build_root)
    common_cache_args = python_cache_args(root, args.python_executable)
    steps: list[Step] = []

    if not args.skip_default:
        default_build = build_root / "default"
        add_configured_suite(
            steps,
            "default",
            default_build,
            args.build_type,
            args.jobs,
            [
                "-DLDCOMPRESS_ENABLE_REAL_FIXTURE_TESTS=OFF",
                "-DLDCOMPRESS_ENABLE_FLAC_TEST_FILE_TESTS=OFF",
                *common_cache_args,
            ],
            [],
        )
        steps.append(
            Step("default: devices", [str(default_build / "ld-compress-ng"), "devices"])
        )

    if not args.skip_no_opencl:
        no_opencl_build = build_root / "no-opencl"
        add_configured_suite(
            steps,
            "no-opencl",
            no_opencl_build,
            args.build_type,
            args.jobs,
            [
                "-DLDCOMPRESS_ENABLE_OPENCL=OFF",
                "-DLDCOMPRESS_ENABLE_REAL_FIXTURE_TESTS=OFF",
                "-DLDCOMPRESS_ENABLE_FLAC_TEST_FILE_TESTS=OFF",
                *common_cache_args,
            ],
            [],
        )
        steps.append(
            Step("no-opencl: devices", [str(no_opencl_build / "ld-compress-ng"), "devices"])
        )

    if not args.skip_no_vulkan:
        no_vulkan_build = build_root / "no-vulkan"
        add_configured_suite(
            steps,
            "no-vulkan",
            no_vulkan_build,
            args.build_type,
            args.jobs,
            [
                "-DLDCOMPRESS_ENABLE_VULKAN=OFF",
                "-DLDCOMPRESS_ENABLE_REAL_FIXTURE_TESTS=OFF",
                "-DLDCOMPRESS_ENABLE_FLAC_TEST_FILE_TESTS=OFF",
                *common_cache_args,
            ],
            [],
        )
        steps.append(
            Step("no-vulkan: devices", [str(no_vulkan_build / "ld-compress-ng"), "devices"])
        )

    if not args.skip_no_metal:
        no_metal_build = build_root / "no-metal"
        add_configured_suite(
            steps,
            "no-metal",
            no_metal_build,
            args.build_type,
            args.jobs,
            [
                "-DLDCOMPRESS_ENABLE_METAL=OFF",
                "-DLDCOMPRESS_ENABLE_REAL_FIXTURE_TESTS=OFF",
                "-DLDCOMPRESS_ENABLE_FLAC_TEST_FILE_TESTS=OFF",
                *common_cache_args,
            ],
            [],
        )
        steps.append(
            Step("no-metal: devices", [str(no_metal_build / "ld-compress-ng"), "devices"])
        )

    include_flac = args.all_local or args.include_flac_test_files
    flac_dir = optional_dir(
        root,
        include_flac,
        args.strict_optional,
        args.flac_test_file_dir,
        "FLAC testbench",
    )
    if include_flac and flac_dir is not None:
        flac_build = build_root / "flac-test-files"
        add_configured_suite(
            steps,
            "flac-test-files",
            flac_build,
            args.build_type,
            args.jobs,
            [
                "-DLDCOMPRESS_ENABLE_REAL_FIXTURE_TESTS=OFF",
                "-DLDCOMPRESS_ENABLE_FLAC_TEST_FILE_TESTS=ON",
                f"-DLDCOMPRESS_FLAC_TEST_FILE_DIR={flac_dir}",
                *common_cache_args,
            ],
            ["-L", "flac-test-files"],
        )

    include_real = (
        args.all_local or
        args.include_real_fixtures or
        args.include_opencl_real_fixture or
        args.include_vulkan_real_fixture or
        args.include_metal_real_fixture
    )
    real_dir = optional_dir(
        root,
        include_real,
        args.strict_optional,
        args.real_fixture_dir,
        "real fixture",
    )
    if include_real and real_dir is not None:
        real_build = build_root / "real-fixtures"
        real_ctest_args = ["-L", "real-fixtures"]
        excluded_labels = []
        if not args.include_opencl_real_fixture:
            excluded_labels.append("opencl")
        if not args.include_vulkan_real_fixture:
            excluded_labels.append("vulkan")
        if not args.include_metal_real_fixture:
            excluded_labels.append("metal")
        if excluded_labels:
            real_ctest_args.extend(["-LE", "|".join(excluded_labels)])
        real_cache_args = [
            "-DLDCOMPRESS_ENABLE_REAL_FIXTURE_TESTS=ON",
            f"-DLDCOMPRESS_REAL_FIXTURE_DIR={real_dir}",
            "-DLDCOMPRESS_ENABLE_FLAC_TEST_FILE_TESTS=OFF",
            *common_cache_args,
        ]
        if args.opencl_device is not None:
            real_cache_args.append(f"-DLDCOMPRESS_OPENCL_TEST_DEVICE={args.opencl_device}")
        if args.vulkan_device is not None:
            real_cache_args.append(f"-DLDCOMPRESS_VULKAN_TEST_DEVICE={args.vulkan_device}")
        if args.metal_device is not None:
            real_cache_args.append(f"-DLDCOMPRESS_METAL_TEST_DEVICE={args.metal_device}")
        add_configured_suite(
            steps,
            "real-fixtures",
            real_build,
            args.build_type,
            args.jobs,
            real_cache_args,
            real_ctest_args,
        )

    return steps


def run_steps(root: Path, steps: list[Step], dry_run: bool) -> None:
    for index, step in enumerate(steps, start=1):
        print(f"[{index}/{len(steps)}] {step.label}", flush=True)
        print("+ " + shlex.join(step.command), flush=True)
        if dry_run:
            continue
        subprocess.run(step.command, cwd=root, check=True)


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Run repeatable local configure/build/test validation suites."
    )
    parser.add_argument("--build-root", default=Path("build/local-check"), type=Path)
    parser.add_argument("--build-type", default="RelWithDebInfo")
    parser.add_argument("--jobs", help="job count passed to cmake --build --parallel")
    parser.add_argument("--skip-default", action="store_true",
        help="do not run the default configure/build/test lane")
    parser.add_argument("--skip-no-opencl", action="store_true",
        help="do not run the LDCOMPRESS_ENABLE_OPENCL=OFF lane")
    parser.add_argument("--skip-no-vulkan", action="store_true",
        help="do not run the LDCOMPRESS_ENABLE_VULKAN=OFF lane")
    parser.add_argument("--skip-no-metal", action="store_true",
        help="do not run the LDCOMPRESS_ENABLE_METAL=OFF lane")
    parser.add_argument("--include-flac-test-files", action="store_true")
    parser.add_argument("--include-real-fixtures", action="store_true")
    parser.add_argument(
        "--include-opencl-real-fixture",
        action="store_true",
        help="include the real-fixture lane and its OpenCL-labelled compatibility test",
    )
    parser.add_argument(
        "--include-vulkan-real-fixture",
        action="store_true",
        help="include the real-fixture lane and its Vulkan-labelled compatibility test",
    )
    parser.add_argument(
        "--include-metal-real-fixture",
        action="store_true",
        help="include the real-fixture lane and its Metal-labelled compatibility test",
    )
    parser.add_argument(
        "--opencl-device",
        help="flattened OpenCL device index for the OpenCL real-fixture compatibility test",
    )
    parser.add_argument(
        "--vulkan-device",
        help="Vulkan device index for the Vulkan real-fixture compatibility test",
    )
    parser.add_argument(
        "--metal-device",
        help="Metal device index for the Metal real-fixture compatibility test",
    )
    parser.add_argument(
        "--python-executable",
        type=Path,
        help="Python interpreter to pass to CMake as Python3_EXECUTABLE",
    )
    parser.add_argument(
        "--all-local",
        action="store_true",
        help="include all optional local suites whose fixture directories exist",
    )
    parser.add_argument(
        "--strict-optional",
        action="store_true",
        help="fail when a requested optional fixture directory is missing",
    )
    parser.add_argument(
        "--real-fixture-dir",
        default=Path(DEFAULT_REAL_FIXTURE_DIR),
        type=Path,
    )
    parser.add_argument(
        "--flac-test-file-dir",
        default=Path(DEFAULT_FLAC_TEST_FILE_DIR),
        type=Path,
    )
    parser.add_argument("--dry-run", action="store_true")
    return parser.parse_args(argv)


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    if args.opencl_device is not None and not args.include_opencl_real_fixture:
        raise RuntimeError("--opencl-device requires --include-opencl-real-fixture")
    if args.vulkan_device is not None and not args.include_vulkan_real_fixture:
        raise RuntimeError("--vulkan-device requires --include-vulkan-real-fixture")
    if args.metal_device is not None and not args.include_metal_real_fixture:
        raise RuntimeError("--metal-device requires --include-metal-real-fixture")
    root = project_root()
    steps = build_steps(args)
    if not steps:
        raise RuntimeError("no validation steps selected")
    run_steps(root, steps, args.dry_run)
    if args.dry_run:
        print("Dry run complete.")
    else:
        print("All selected validation steps passed.")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main(sys.argv[1:]))
    except (RuntimeError, OSError, subprocess.SubprocessError) as exc:
        print(f"check_local_matrix.py: {exc}", file=sys.stderr)
        raise SystemExit(1)
