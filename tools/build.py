import argparse
import ctypes
import hashlib
import io
import os
from pathlib import Path
import struct
import subprocess
import tarfile


ROOT = Path(__file__).resolve().parents[1]
EDK2 = "d46aa46c8361194521391aa581593e556c707c6e"
FRAMEWORK = "89910a39dcfd788057caa5d88b7e76e112d187b5"
ORIGINAL_HASH = "bf6276e5afa2860afb2df579fc70edcb32d9df15f62fd06a3c11c22126fb8d99"
PATCHED_HASH = "e05382242cc07d6e1a7a598f2c2d9e0c4678e6fdea97f79092c91be07c540d91"
LENGTH = 45700
ROM_START = 0x84
ROM_CHECKSUM = ROM_START + 0x05
GFX_VOLTAGE = ROM_START + 0x25AE
POWERPLAY_START = 0x2288
POWERPLAY_LENGTH = 2470
MPT_TABLE_START = 0x100


def read_url(url):
    return subprocess.check_output([
        "curl", "--fail", "--silent", "--show-error", "--location", "--max-time", "90", url,
    ])


def headers():
    include = ROOT / ".deps" / EDK2 / "Include"
    marker = include / ".ready"
    if marker.is_file():
        return include
    url = f"https://codeload.github.com/tianocore/edk2/tar.gz/{EDK2}"
    prefix = f"edk2-{EDK2}/MdePkg/Include/"
    with tarfile.open(fileobj=io.BytesIO(read_url(url)), mode="r:gz") as archive:
        for member in archive:
            if member.isfile() and member.name.startswith(prefix):
                relative = Path(member.name.removeprefix(prefix))
                if relative.is_absolute() or ".." in relative.parts:
                    raise ValueError("Invalid header path in the EDK II archive")
                target = include / relative
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(archive.extractfile(member).read())
    base = f"https://raw.githubusercontent.com/tianocore/edk2/{FRAMEWORK}"
    framework = "IntelFrameworkPkg/Include/Protocol/AcpiSupport.h"
    (include / "Protocol" / "AcpiSupport.h").write_bytes(read_url(f"{base}/{framework}"))
    marker.touch()
    return include


def digest(data):
    return hashlib.sha256(data).hexdigest()


def patch(original, mpt_profile=None):
    if len(original) != LENGTH or digest(original) != ORIGINAL_HASH:
        raise ValueError("VFCT does not match the supported hardware capture")
    if original[:4] != b"VFCT" or sum(original) % 256:
        raise ValueError("Invalid ACPI table")
    if original[GFX_VOLTAGE:GFX_VOLTAGE + 2] != struct.pack("<H", 1150 * 4):
        raise ValueError("Unexpected GFX voltage ceiling")
    modified = bytearray(original)
    if mpt_profile is None:
        struct.pack_into("<H", modified, GFX_VOLTAGE, 1050 * 4)
    else:
        powerplay = mpt_profile[MPT_TABLE_START:]
        if not mpt_profile.startswith(b"\x55\xaa") or len(powerplay) != POWERPLAY_LENGTH:
            raise ValueError("Unsupported MPT PowerPlay layout")
        modified[POWERPLAY_START:POWERPLAY_START + POWERPLAY_LENGTH] = powerplay
    difference = sum(original) - sum(modified)
    modified[ROM_CHECKSUM] = (modified[ROM_CHECKSUM] + difference) % 256
    if sum(modified) % 256 or (mpt_profile is None and digest(modified) != PATCHED_HASH):
        raise ValueError("Unexpected checksum or patched VFCT hash")
    return bytes(modified)


def capture(output):
    if os.name != "nt":
        raise ValueError("VFCT capture requires Windows")
    get_table = ctypes.WinDLL("kernel32", use_last_error=True).GetSystemFirmwareTable
    get_table.argtypes = (ctypes.c_uint32, ctypes.c_uint32, ctypes.c_void_p, ctypes.c_uint32)
    get_table.restype = ctypes.c_uint32
    acpi, vfct = 0x41435049, 0x54434656
    length = get_table(acpi, vfct, None, 0)
    if not 36 <= length <= 16 * 1024 * 1024:
        raise ValueError("Windows did not expose a readable VFCT")
    buffer = ctypes.create_string_buffer(length)
    if get_table(acpi, vfct, buffer, length) != length:
        raise ValueError("VFCT length changed during capture")
    with output.open("xb") as file:
        file.write(buffer.raw)
    print(f"VFCT SHA-256: {digest(buffer.raw)}")


def write_header(folder, original, modified):
    with (folder / "VfctData.h").open("w", encoding="ascii", newline="\n") as file:
        for name, data in (("OriginalVfct", original), ("TrialVfct", modified)):
            file.write(f"static const UINT8 {name}[] = {{\n")
            for offset in range(0, len(data), 16):
                file.write("    " + ", ".join(f"0x{byte:02x}" for byte in data[offset:offset + 16]) + ",\n")
            file.write("};\n")


def build(args):
    original = args.vfct.read_bytes()
    profile = args.mpt_profile.read_bytes() if args.mpt_profile else None
    if (profile is not None) != (args.mpt_profile_sha256 is not None):
        raise ValueError("--mpt-profile and --mpt-profile-sha256 must be used together")
    if profile is not None and digest(profile) != args.mpt_profile_sha256.lower():
        raise ValueError("MPT file does not match the expected SHA-256")
    modified = patch(original, profile)
    include = headers()
    out = ROOT / "build"
    out.mkdir(exist_ok=True)
    write_header(out, original, modified)
    source, stem, entry, subsystem = (
        ("driver", "Rx6600Driver", "EfiDriverMain", "efi_boot_service_driver")
        if args.driver else ("boot", "Rx6600Boot", "EfiMain", "efi_application")
    )
    if profile is not None:
        stem += "Perfil"
    obj = out / f"{stem}.obj"
    image = out / f"{stem}.efi"
    subprocess.run([
        args.clang, "--target=x86_64-pc-windows-msvc", "-fshort-wchar", "-ffreestanding",
        "-fno-stack-protector", "-mno-red-zone", "-O2", "-Wall", "-Wextra", "-Werror",
        "-I", include, "-I", include / "X64", "-I", out,
        "-c", ROOT / "src" / f"{source}.c", "-o", obj,
    ], check=True)
    subprocess.run([
        args.linker, f"/subsystem:{subsystem}", f"/entry:{entry}", "/nodefaultlib",
        "/machine:x64", "/dynamicbase", f"/out:{image}", obj,
    ], check=True)
    print(f"{image}\nSHA-256: {digest(image.read_bytes())}")
    print(f"VFCT SHA-256: {digest(modified)}")


def test(args):
    include = headers()
    out = ROOT / "build" / "mock"
    out.mkdir(parents=True, exist_ok=True)
    # The synthetic table exercises UEFI behavior without distributing a VBIOS.
    original = bytearray(256)
    struct.pack_into("<4sI", original, 0, b"VFCT", len(original))
    original[9] = -sum(original) % 256
    modified = original.copy()
    modified[40] = 1
    modified[9] = (modified[9] - 1) % 256
    write_header(out, original, modified)
    executable = out / ("boot_mock.exe" if os.name == "nt" else "boot_mock")
    subprocess.run([
        args.cc, "-fshort-wchar", "-O2", "-Wall", "-Wextra", "-Werror",
        "-I", include, "-I", include / "X64", "-I", out, "-I", ROOT / "src",
        ROOT / "tests" / "boot_mock.c", "-o", executable,
    ], check=True)
    subprocess.run([executable], check=True)
    try:
        patch(original)
    except ValueError:
        pass
    else:
        raise ValueError("Firmware guard accepted a synthetic capture")


def main():
    parser = argparse.ArgumentParser(description="RX 6600 VFCT boot tool")
    commands = parser.add_subparsers(dest="command", required=True)
    capture_cmd = commands.add_parser("capture")
    capture_cmd.add_argument("output", type=Path)
    build_cmd = commands.add_parser("build")
    build_cmd.add_argument("--vfct", type=Path, required=True)
    build_cmd.add_argument("--driver", action="store_true")
    build_cmd.add_argument("--mpt-profile", type=Path, help="Use a locally tested MorePowerTool profile")
    build_cmd.add_argument("--mpt-profile-sha256", help="Expected SHA-256 of the local MPT file")
    build_cmd.add_argument("--clang", default="clang")
    build_cmd.add_argument("--linker", default="lld-link")
    test_cmd = commands.add_parser("test")
    test_cmd.add_argument("--cc", default="gcc")
    args = parser.parse_args()
    try:
        if args.command == "capture":
            capture(args.output)
        elif args.command == "build":
            build(args)
        else:
            test(args)
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"{error}\n")


if __name__ == "__main__":
    main()
