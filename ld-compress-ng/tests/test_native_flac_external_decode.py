#!/usr/bin/env python3

import argparse
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path


SKIP_RETURN_CODE = 77
DEFAULT_NATIVE_FRAME_SAMPLES = 4608
# Two full native FLAC frames plus one LDS group, exercising a 4-sample final frame.
DEFAULT_FIXTURE_GROUPS = 2305


def skip(message):
    print(f"SKIP: {message}")
    return SKIP_RETURN_CODE


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def pack_group(samples):
    words = [(sample // 64) + 512 for sample in samples]
    return bytes(
        [
            (words[0] & 0x03FC) >> 2,
            ((words[0] & 0x0003) << 6) + ((words[1] & 0x03F0) >> 4),
            ((words[1] & 0x000F) << 4) + ((words[2] & 0x03C0) >> 6),
            ((words[2] & 0x003F) << 2) + ((words[3] & 0x0300) >> 8),
            words[3] & 0x00FF,
        ]
    )


def unpack_group(data):
    require(len(data) == 5, "truncated LDS sample group")
    words = [
        (data[0] << 2) | (data[1] >> 6),
        ((data[1] & 0x3F) << 4) | (data[2] >> 4),
        ((data[2] & 0x0F) << 6) | (data[3] >> 2),
        ((data[3] & 0x03) << 8) | data[4],
    ]
    return [(word - 512) * 64 for word in words]


def make_fixture(group_count=DEFAULT_FIXTURE_GROUPS):
    lds = bytearray()
    pcm = bytearray()
    for group in range(group_count):
        samples = [((((group * 7) + index) % 1024) - 512) * 64 for index in range(4)]
        lds.extend(pack_group(samples))
        for sample in samples:
            pcm.extend(struct.pack("<h", sample))
    return bytes(lds), bytes(pcm)


def total_lds_samples(lds_path):
    size = lds_path.stat().st_size
    require(size % 5 == 0, f"{lds_path} is not a whole number of LDS groups")
    return (size // 5) * 4


def read_expected_pcm_window(lds_path, sample, readlen):
    total_samples = total_lds_samples(lds_path)
    require(sample >= 0, "negative sample offset requested")
    require(readlen >= 0, "negative sample count requested")
    require(
        sample + readlen <= total_samples,
        f"{lds_path} does not contain {readlen} samples at offset {sample}",
    )

    first_group = sample // 4
    end_sample = sample + readlen
    group_count = ((end_sample + 3) // 4) - first_group

    with lds_path.open("rb") as infile:
        infile.seek(first_group * 5)
        group_bytes = infile.read(group_count * 5)
    require(
        len(group_bytes) == group_count * 5,
        f"{lds_path} ended while reading expected LDS samples",
    )

    pcm = bytearray()
    for offset in range(0, len(group_bytes), 5):
        for value in unpack_group(group_bytes[offset : offset + 5]):
            pcm.extend(struct.pack("<h", value))

    start_byte = (sample - (first_group * 4)) * 2
    end_byte = start_byte + (readlen * 2)
    return bytes(pcm[start_byte:end_byte])


def loader_read_windows(total_samples):
    readlen = min(8192, total_samples)
    if readlen == 0:
        return []

    starts = [0]
    if total_samples > readlen + 257:
        starts.append(257)
    if total_samples > readlen:
        starts.append(max(0, (total_samples // 2) - (readlen // 2)))
    if total_samples > readlen * 2:
        starts.append(total_samples - readlen)

    windows = []
    seen = set()
    for start in starts:
        if start in seen:
            continue
        seen.add(start)
        windows.append((start, min(readlen, total_samples - start)))
    return windows


def find_lds_fixtures(root, limit):
    fixtures = sorted(path for path in root.rglob("*.lds") if path.is_file())
    if limit is not None:
        require(limit > 0, "real fixture limit must be positive")
        fixtures = fixtures[:limit]
    return fixtures


def duplicate_file(source, target):
    try:
        os.link(source, target)
    except OSError:
        shutil.copyfile(source, target)


def run_checked(args, **kwargs):
    result = subprocess.run(args, text=True, capture_output=True, **kwargs)
    if result.returncode != 0:
        raise RuntimeError(
            f"command failed ({result.returncode}): {' '.join(map(str, args))}\n"
            f"stdout:\n{result.stdout}\n"
            f"stderr:\n{result.stderr}"
        )
    return result


def parsed_device_usable(fields, backend):
    if backend == "opencl":
        return fields.get("available") == "yes"
    if backend == "metal":
        return fields.get("available") == "yes"
    return fields.get("vulkan backend usable") == "yes"


def parse_backend_devices(devices_output, backend):
    section_labels = {
        "opencl": "OpenCL",
        "vulkan": "Vulkan",
        "metal": "Metal",
    }
    label = section_labels[backend]
    if f"{label} support: not built" in devices_output:
        return None, f"{label} support is not built"

    in_section = False
    current_index = None
    current_fields = {}
    devices = []

    def append_current_device():
        nonlocal current_index, current_fields
        if current_index is None:
            return
        devices.append({
            "index": current_index,
            **current_fields,
        })

    for line in devices_output.splitlines():
        if line.startswith("OpenCL support:"):
            append_current_device()
            in_section = backend == "opencl"
            current_index = None
            current_fields = {}
        elif line.startswith("Vulkan support:"):
            append_current_device()
            in_section = backend == "vulkan"
            current_index = None
            current_fields = {}
        elif line.startswith("Metal support:"):
            append_current_device()
            in_section = backend == "metal"
            current_index = None
            current_fields = {}
        elif in_section and line.startswith("[") and "]" in line:
            append_current_device()
            current_index = line.split("]", 1)[0][1:]
            current_fields = {}
        elif in_section and current_index is not None and ":" in line:
            key, value = line.strip().split(":", 1)
            current_fields[key] = value.strip()

    append_current_device()
    return devices, None


def select_backend_device_from_devices_output(devices_output, backend, requested_device):
    section_labels = {
        "opencl": "OpenCL",
        "vulkan": "Vulkan",
        "metal": "Metal",
    }
    label = section_labels[backend]
    devices, skip_reason = parse_backend_devices(devices_output, backend)
    if skip_reason is not None:
        return None, skip_reason

    if requested_device is not None:
        for device in devices:
            if device["index"] == requested_device:
                if parsed_device_usable(device, backend):
                    return requested_device, None
                if backend == "vulkan":
                    return None, f"requested {label} device {requested_device} is not backend-usable"
                return None, f"requested {label} device {requested_device} is not available"
        return None, f"requested {label} device {requested_device} is not visible"

    if backend == "opencl":
        for device in devices:
            if parsed_device_usable(device, backend):
                return device["index"], None
        return None, f"no available {label} device"

    if backend == "metal":
        usable_devices = [
            device for device in devices
            if parsed_device_usable(device, backend)
        ]
        for device in usable_devices:
            if device.get("low power") == "no":
                return device["index"], None
        if usable_devices:
            return usable_devices[0]["index"], None
        return None, f"no available {label} device"

    usable_devices = [
        device for device in devices
        if parsed_device_usable(device, backend)
    ]
    for device in usable_devices:
        if device.get("type") == "discrete-gpu":
            return device["index"], None
    for device in usable_devices:
        if device.get("type") != "cpu":
            return device["index"], None
    if usable_devices:
        return None, f"no backend-usable non-CPU {label} device"
    return None, f"no backend-usable {label} device"


def available_backend_device(binary, backend, requested_device):
    result = run_checked([str(binary), "devices"])
    return select_backend_device_from_devices_output(result.stdout, backend, requested_device)


def available_opencl_device(binary, requested_device):
    return available_backend_device(binary, "opencl", requested_device)


def available_vulkan_device(binary, requested_device):
    return available_backend_device(binary, "vulkan", requested_device)


def available_metal_device(binary, requested_device):
    return available_backend_device(binary, "metal", requested_device)


def run_binary_decode(args):
    result = subprocess.run(args, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if result.returncode != 0:
        raise RuntimeError(
            f"decode command failed ({result.returncode}): {' '.join(map(str, args))}\n"
            f"stderr:\n{result.stderr.decode(errors='replace')}"
        )
    return result.stdout


def resolve_tool(path_or_name):
    if not path_or_name:
        return None
    if path_or_name.endswith("-NOTFOUND"):
        return None
    path = Path(path_or_name)
    if path.exists():
        return str(path)
    resolved = shutil.which(path_or_name)
    return resolved


def ffprobe_native_flac(ffprobe, flac_path):
    if ffprobe is None:
        return
    result = run_checked(
        [
            ffprobe,
            "-v",
            "error",
            "-select_streams",
            "a:0",
            "-show_entries",
            "stream=codec_name,sample_rate,channels",
            "-of",
            "json",
            str(flac_path),
        ]
    )
    streams = json.loads(result.stdout).get("streams", [])
    require(streams, "ffprobe did not find an audio stream")
    stream = streams[0]
    require(stream.get("codec_name") == "flac", "ffprobe did not report FLAC audio")
    require(stream.get("sample_rate") == "40000", "ffprobe did not report 40 kHz sample rate")
    require(stream.get("channels") == 1, "ffprobe did not report mono audio")


def compress_native_flac_backend(
    binary,
    backend,
    lds_path,
    flac_path,
    threads=None,
    device=None,
):
    command = [
        str(binary),
        "compress",
        "--backend",
        backend,
    ]
    if threads is not None:
        command.extend(["--threads", str(threads)])
    if device is not None:
        command.extend(["--device", str(device)])
    command.extend(
        [
            "--overwrite",
            str(lds_path),
            str(flac_path),
        ]
    )
    run_checked(command)
    require(flac_path.exists(), f"{backend} compression did not create output")


def compress_native_fixed(binary, lds_path, flac_path, threads=None):
    compress_native_flac_backend(
        binary,
        "native-fixed",
        lds_path,
        flac_path,
        threads=threads,
    )


def compress_opencl_native(binary, lds_path, flac_path, opencl_device):
    compress_native_flac_backend(
        binary,
        "opencl",
        lds_path,
        flac_path,
        device=opencl_device,
    )


def compress_vulkan_native(binary, lds_path, flac_path, vulkan_device):
    compress_native_flac_backend(
        binary,
        "vulkan",
        lds_path,
        flac_path,
        device=vulkan_device,
    )


def compress_metal_native(binary, lds_path, flac_path, metal_device):
    compress_native_flac_backend(
        binary,
        "metal",
        lds_path,
        flac_path,
        device=metal_device,
    )


def compress_cpu_ogg(binary, lds_path, flac_path):
    run_checked(
        [
            str(binary),
            "compress",
            "--backend",
            "cpu",
            "--overwrite",
            str(lds_path),
            str(flac_path),
        ]
    )
    require(flac_path.exists(), "CPU compression did not create output")


def decode_with_ffmpeg(args, flac_path):
    ffmpeg = resolve_tool(args.ffmpeg)
    if ffmpeg is None:
        return None, "ffmpeg is not available"
    ffprobe = resolve_tool(args.ffprobe)
    ffprobe_native_flac(ffprobe, flac_path)
    decoded = run_binary_decode(
        [
            ffmpeg,
            "-hide_banner",
            "-loglevel",
            "error",
            "-i",
            str(flac_path),
            "-map",
            "0:a:0",
            "-c:a",
            "pcm_s16le",
            "-f",
            "s16le",
            "-",
        ]
    )
    return decoded, None


def decode_with_ld_decode_pyav(args, flac_path):
    ldf_reader = Path(args.ldf_reader) if args.ldf_reader else None
    if ldf_reader is None or not ldf_reader.exists():
        return None, f"reference ld-decode reader not found: {ldf_reader}"

    try:
        import av  # noqa: F401
    except ImportError:
        return None, "PyAV is not installed"

    decoded = run_binary_decode(
        [sys.executable, str(ldf_reader), "-q", str(flac_path)]
    )
    return decoded, None


def require_ld_decode_pyav_pcm(decoded, expected_pcm):
    if decoded == expected_pcm:
        return
    # The standalone ldf_reader.py writes whole PyAV plane buffers; a tiny final
    # FLAC frame can expose zero padding past the true STREAMINFO sample count.
    require(
        decoded.startswith(expected_pcm),
        "ld-decode-pyav changed native .flac.ldf PCM bytes",
    )
    padding = decoded[len(expected_pcm) :]
    require(
        len(padding) <= 64 and len(padding) % 2 == 0 and all(byte == 0 for byte in padding),
        "ld-decode-pyav emitted unexpected trailing PCM bytes",
    )


def close_loader(loader):
    close = getattr(loader, "_close", None)
    if close is not None:
        close()


def require_loader_window(utils, path, sample, readlen, expected):
    loader = utils.make_loader(str(path))
    try:
        require(
            isinstance(loader, utils.LoadLDF),
            f"{path.name} did not dispatch to LoadLDF",
        )
        with path.open("rb") as infile:
            decoded = loader(infile, sample, readlen)
        require(decoded is not None, f"{path.name} loader returned EOF")
        require(
            decoded.tobytes() == expected,
            f"{path.name} loader changed samples at offset {sample}",
        )
    finally:
        close_loader(loader)


def require_loader_bytes(utils, path, sample, readlen, expected_pcm):
    expected = expected_pcm[sample * 2 : (sample + readlen) * 2]
    require_loader_window(utils, path, sample, readlen, expected)


def import_ld_decode_utils(args, temp_dir):
    ld_decode_dir = Path(args.ld_decode_dir) if args.ld_decode_dir else None
    if ld_decode_dir is None or not ld_decode_dir.exists():
        return None, "reference ld-decode directory not found"

    os.environ["NUMBA_CACHE_DIR"] = str(temp_dir / "numba-cache")
    sys.dont_write_bytecode = True
    sys.path.insert(0, str(ld_decode_dir))
    try:
        from lddecode import utils
    except ImportError as ex:
        return None, f"reference ld-decode loader dependencies are not available: {ex}"

    return utils, None


def decode_with_ld_decode_loader(args, binary, lds_path, temp_dir, expected_pcm):
    utils, skip_reason = import_ld_decode_utils(args, temp_dir)
    if skip_reason is not None:
        return skip_reason

    ogg_ldf = temp_dir / "fixture.ldf"
    ogg_raw_oga = temp_dir / "fixture.raw.oga"
    native_flac_ldf = temp_dir / "fixture.flac.ldf"
    native_flac = temp_dir / "fixture.flac"
    compress_cpu_ogg(binary, lds_path, ogg_ldf)
    duplicate_file(ogg_ldf, ogg_raw_oga)
    compress_native_fixed(binary, lds_path, native_flac_ldf)
    duplicate_file(native_flac_ldf, native_flac)

    outputs = [
        ("ogg .ldf", ogg_ldf),
        ("ogg .raw.oga", ogg_raw_oga),
        ("native .flac.ldf", native_flac_ldf),
        ("native .flac", native_flac),
    ]

    total_samples = len(expected_pcm) // 2
    offset_sample = 257
    offset_readlen = 4096
    require(
        offset_sample + offset_readlen <= total_samples,
        "fixture is too small for nonzero loader read",
    )
    for _, path in outputs:
        require_loader_bytes(utils, path, 0, total_samples, expected_pcm)
        require_loader_bytes(utils, path, offset_sample, offset_readlen, expected_pcm)

    return None


def require_real_loader_windows(utils, compressed_path, lds_path):
    for sample, readlen in loader_read_windows(total_lds_samples(lds_path)):
        expected = read_expected_pcm_window(lds_path, sample, readlen)
        require_loader_window(utils, compressed_path, sample, readlen, expected)


def decode_real_fixtures_with_ld_decode_loader(args, binary, temp_dir):
    root = Path(args.real_fixture_dir) if args.real_fixture_dir else None
    if root is None or not root.is_dir():
        return "real LDS fixture directory not found"
    if args.real_fixture_threads is not None:
        require(args.real_fixture_threads > 0, "real fixture thread count must be positive")
    require(
        args.real_fixture_backend not in ("opencl", "vulkan", "metal") or
        args.real_fixture_threads is None,
        "accelerated real fixture mode does not accept --real-fixture-threads",
    )

    utils, skip_reason = import_ld_decode_utils(args, temp_dir)
    if skip_reason is not None:
        return skip_reason

    opencl_device = args.opencl_device
    vulkan_device = args.vulkan_device
    metal_device = args.metal_device
    if args.real_fixture_backend == "opencl":
        opencl_device, skip_reason = available_opencl_device(binary, opencl_device)
        if skip_reason is not None:
            return skip_reason
    elif args.real_fixture_backend == "vulkan":
        vulkan_device, skip_reason = available_vulkan_device(binary, vulkan_device)
        if skip_reason is not None:
            return skip_reason
    elif args.real_fixture_backend == "metal":
        metal_device, skip_reason = available_metal_device(binary, metal_device)
        if skip_reason is not None:
            return skip_reason

    fixtures = find_lds_fixtures(root, args.real_fixture_limit)
    if not fixtures:
        return f"no .lds fixtures found under {root}"

    for index, lds_path in enumerate(fixtures, start=1):
        case_dir = temp_dir / f"real-fixture-{index}"
        case_dir.mkdir()

        ogg_ldf = case_dir / "fixture.ldf"
        ogg_raw_oga = case_dir / "fixture.raw.oga"
        native_flac_ldf = case_dir / f"fixture.{args.real_fixture_backend}.flac.ldf"
        native_flac = case_dir / f"fixture.{args.real_fixture_backend}.flac"

        compressed_paths = []
        if args.real_fixture_suffixes == "all":
            compress_cpu_ogg(binary, lds_path, ogg_ldf)
            duplicate_file(ogg_ldf, ogg_raw_oga)
            compressed_paths.extend([ogg_ldf, ogg_raw_oga])

        if args.real_fixture_backend == "native-fixed":
            compress_native_fixed(
                binary,
                lds_path,
                native_flac_ldf,
                threads=args.real_fixture_threads or 8,
            )
        elif args.real_fixture_backend == "opencl":
            compress_opencl_native(binary, lds_path, native_flac_ldf, opencl_device)
        elif args.real_fixture_backend == "metal":
            compress_metal_native(binary, lds_path, native_flac_ldf, metal_device)
        else:
            compress_vulkan_native(binary, lds_path, native_flac_ldf, vulkan_device)
        duplicate_file(native_flac_ldf, native_flac)
        compressed_paths.extend([native_flac_ldf, native_flac])

        for compressed_path in compressed_paths:
            require_real_loader_windows(utils, compressed_path, lds_path)

        try:
            fixture_name = lds_path.relative_to(root).as_posix()
        except ValueError:
            fixture_name = str(lds_path)
        print(
            f"checked ld-decode loader real fixture {index}/{len(fixtures)}: "
            f"{fixture_name} ({args.real_fixture_backend})",
            flush=True,
        )

    return None


def parse_args(argv):
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--decoder",
        choices=("ffmpeg", "ld-decode-pyav", "ld-decode-loader"),
        required=True,
    )
    parser.add_argument("--binary", required=True)
    parser.add_argument("--ffmpeg", default="ffmpeg")
    parser.add_argument("--ffprobe", default="ffprobe")
    parser.add_argument("--ld-decode-dir")
    parser.add_argument("--ldf-reader")
    parser.add_argument(
        "--fixture-dir",
        "--real-fixture-dir",
        dest="real_fixture_dir",
    )
    parser.add_argument(
        "--fixture-limit",
        "--real-fixture-limit",
        dest="real_fixture_limit",
        type=int,
    )
    parser.add_argument(
        "--real-fixture-suffixes",
        choices=("native", "all"),
        default="native",
    )
    parser.add_argument(
        "--real-fixture-backend",
        choices=("native-fixed", "opencl", "vulkan", "metal"),
        default="native-fixed",
    )
    parser.add_argument("--real-fixture-threads", type=int)
    parser.add_argument("--opencl-device")
    parser.add_argument("--vulkan-device")
    parser.add_argument("--metal-device")
    return parser.parse_args(argv)


def main(argv):
    args = parse_args(argv)
    binary = Path(args.binary)
    require(binary.exists(), f"ld-compress-ng not found: {binary}")

    with tempfile.TemporaryDirectory(prefix="ld-compress-ng-external-decode-") as temp:
        temp_dir = Path(temp)
        if args.real_fixture_dir:
            require(
                args.decoder == "ld-decode-loader",
                "real fixtures are only supported by the ld-decode loader mode",
            )
            skip_reason = decode_real_fixtures_with_ld_decode_loader(
                args, binary, temp_dir
            )
            if skip_reason is not None:
                return skip(skip_reason)
            return 0

        lds_path = temp_dir / "fixture.lds"
        flac_path = temp_dir / "fixture.flac.ldf"
        expected_lds, expected_pcm = make_fixture()
        require(
            ((len(expected_pcm) // 2) % DEFAULT_NATIVE_FRAME_SAMPLES) == 4,
            "generated fixture does not exercise a short native FLAC tail frame",
        )
        lds_path.write_bytes(expected_lds)

        if args.decoder == "ffmpeg":
            compress_native_fixed(binary, lds_path, flac_path)
            decoded, skip_reason = decode_with_ffmpeg(args, flac_path)
            if skip_reason is None:
                require(
                    decoded == expected_pcm,
                    f"{args.decoder} changed native .flac.ldf PCM bytes",
                )
        elif args.decoder == "ld-decode-pyav":
            compress_native_fixed(binary, lds_path, flac_path)
            decoded, skip_reason = decode_with_ld_decode_pyav(args, flac_path)
            if skip_reason is None:
                require_ld_decode_pyav_pcm(decoded, expected_pcm)
        else:
            skip_reason = decode_with_ld_decode_loader(
                args, binary, lds_path, temp_dir, expected_pcm)

        if skip_reason is not None:
            return skip(skip_reason)

    return 0


if __name__ == "__main__":
    try:
        sys.exit(main(sys.argv[1:]))
    except Exception as ex:
        print(f"test_native_flac_external_decode: {ex}", file=sys.stderr)
        sys.exit(1)
