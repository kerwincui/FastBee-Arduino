"""Extract a verified full device snapshot, preserving private config for deployment.

This never contacts a device, formats a filesystem, or overwrites an existing
output directory. Only file names, sizes and hashes are printed.
"""
import argparse
import hashlib
import json
import struct
from pathlib import Path, PurePosixPath
from littlefs import LittleFS


def partitions(image):
    table = image[0x8000:0x9000]
    entries = []
    for position in range(0, len(table), 32):
        row = table[position:position + 32]
        magic = struct.unpack_from("<H", row)[0]
        if magic == 0xFFFF:
            break
        if magic == 0xEBEB:
            if hashlib.md5(table[:position]).digest() != row[16:]:
                raise ValueError("Partition table MD5 mismatch")
            break
        if magic != 0x50AA:
            raise ValueError("Invalid partition table magic")
        _, kind, subtype, offset, size, label, flags = struct.unpack("<HBBII16sI", row)
        if not size or offset + size > len(image):
            raise ValueError("Partition falls outside full flash snapshot")
        entries.append(dict(type=kind, subtype=subtype, offset=offset, size=size,
                            name=label.split(b"\0", 1)[0].decode("ascii"), flags=flags))
    if not entries:
        raise ValueError("No partitions found")
    return entries


def extract(snapshot, destination, expected_size):
    snapshot = Path(snapshot).resolve()
    destination = Path(destination).resolve()
    if destination.exists():
        raise ValueError("Refusing to overwrite an existing extraction")
    image = snapshot.read_bytes()
    if len(image) != expected_size:
        raise ValueError("Snapshot size does not match detected physical flash")
    entries = partitions(image)
    filesystem = next((p for p in entries if p["type"] == 1 and
                       p["name"] in ("spiffs", "littlefs")), None)
    if not filesystem or filesystem["flags"] & 1 or filesystem["size"] % 4096:
        raise ValueError("No supported unencrypted LittleFS partition")
    content = image[filesystem["offset"]:filesystem["offset"] + filesystem["size"]]
    fs = LittleFS(block_size=4096, block_count=len(content) // 4096, mount=False)
    fs.context.buffer = bytearray(content)
    fs.mount()  # Failure is fatal: never format a device snapshot.
    manifest = dict(snapshot=str(snapshot), sha256=hashlib.sha256(image).hexdigest(),
                    flashBytes=len(image), partitions=entries, files=[])
    destination.mkdir(parents=True, exist_ok=False)
    try:
        for root, directories, files in fs.walk("/"):
            parents = PurePosixPath(root).parts[1:]
            for name in list(directories) + list(files):
                parts = parents + (name,)
                if any(p in ("", ".", "..") or "\\" in p or ":" in p for p in parts):
                    raise ValueError("Unsafe path in LittleFS")
                target = destination.joinpath(*parts).resolve()
                target.relative_to(destination)
                if name in directories:
                    target.mkdir(parents=True, exist_ok=True)
                else:
                    target.parent.mkdir(parents=True, exist_ok=True)
                    with fs.open("/" + "/".join(parts), "rb") as source:
                        data = source.read()
                    target.write_bytes(data)
                    manifest["files"].append(dict(path="/".join(parts), bytes=len(data),
                                                  sha256=hashlib.sha256(data).hexdigest()))
    finally:
        fs.unmount()
    report = destination.parent / (destination.name + "-manifest.json")
    report.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(dict(snapshot=snapshot.name, sha256=manifest["sha256"],
                          flashBytes=len(image), filesystemOffset=filesystem["offset"],
                          filesystemBytes=filesystem["size"], fileCount=len(manifest["files"]),
                          report=str(report)), ensure_ascii=True))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("snapshot")
    parser.add_argument("destination")
    parser.add_argument("--expected-size", type=int, required=True)
    args = parser.parse_args()
    extract(args.snapshot, args.destination, args.expected_size)
