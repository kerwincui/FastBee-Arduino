"""Build and verify a LittleFS image with a device's original private files.

Only www is replaced. Configuration, history and other device files come from
the verified flash extraction, never from repository sample configuration.
This is an offline operation; it refuses to overwrite output artifacts.
"""
import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
from littlefs import LittleFS


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def safe_path(root, relative):
    parts = PurePosixPath(relative).parts
    if not parts or any(p in ("", ".", "..", "/") or "\\" in p or ":" in p for p in parts):
        raise ValueError("Unsafe path in extraction manifest")
    result = root.joinpath(*parts).resolve()
    result.relative_to(root)
    return result


def build(extracted, web, output):
    extracted, web, output = (Path(p).resolve() for p in (extracted, web, output))
    report_path = output.with_suffix(".json")
    if output.exists() or report_path.exists():
        raise ValueError("Refusing to overwrite an existing image/report")
    manifest_path = extracted.parent / (extracted.name + "-manifest.json")
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    snapshot = Path(manifest["snapshot"])
    if sha256(snapshot.read_bytes()) != manifest["sha256"]:
        raise ValueError("Full flash backup no longer matches its verified extraction")
    partition = next(p for p in manifest["partitions"] if p["type"] == 1 and p["name"] in ("spiffs", "littlefs"))
    contents = {}
    preserved = []
    for entry in manifest["files"]:
        name = entry["path"]
        content = safe_path(extracted, name).read_bytes()
        if len(content) != entry["bytes"] or sha256(content) != entry["sha256"]:
            raise ValueError(f"Extracted file changed: {name}")
        if not name.startswith("www/"):
            contents[name] = content
            preserved.append(name)
    if "config/network.json" not in contents or "config/device.json" not in contents:
        raise ValueError("Device network/identity configuration is missing")
    for item in sorted(web.rglob("*")):
        if item.is_file():
            item.resolve().relative_to(web)
            name = "www/" + item.relative_to(web).as_posix()
            safe_path(web.parent, name)
            contents[name] = item.read_bytes()
    if "www/index.html.gz" not in contents or "www/web-resource-manifest.json" not in contents:
        raise ValueError("Expected compressed Web entry and resource manifest")
    # Match the installed pioarduino builder's LittleFS/ESP-IDF parameters.
    fs = LittleFS(block_size=4096, block_count=partition["size"] // 4096,
                  read_size=1, prog_size=1, cache_size=4096, lookahead_size=32,
                  block_cycles=500, name_max=64, disk_version=0x00020001,
                  mount=True)
    for name, content in sorted(contents.items()):
        parent = PurePosixPath(name).parent.as_posix()
        if parent != ".":
            fs.makedirs("/" + parent, exist_ok=True)
        with fs.open("/" + name, "wb") as stream:
            if stream.write(content) != len(content):
                raise ValueError("Incomplete filesystem write")
    allocated_bytes = fs.used_block_count * 4096
    fs.unmount()
    image = bytes(fs.context.buffer)
    if len(image) != partition["size"]:
        raise ValueError("Filesystem image size differs from original partition")
    verifier = LittleFS(block_size=4096, block_count=len(image) // 4096, mount=False)
    verifier.context.buffer = bytearray(image)
    verifier.mount()
    found = set()
    for root, _, files in verifier.walk("/"):
        for name in files:
            relative = str(PurePosixPath(root, name)).lstrip("/")
            with verifier.open("/" + relative, "rb") as stream:
                remounted = stream.read()
            expected = contents.get(relative)
            if (expected is None or len(remounted) != len(expected) or
                    sha256(remounted) != sha256(expected)):
                raise ValueError(f"Remounted file verification failed: {relative}")
            found.add(relative)
    verifier.unmount()
    if found != set(contents):
        raise ValueError("Image does not preserve the selected file set")
    output.parent.mkdir(parents=True, exist_ok=True)
    with output.open("xb") as stream:
        stream.write(image)
    report = dict(backupSha256=manifest["sha256"], filesystemOffset=partition["offset"],
                  filesystemBytes=len(image), allocatedBytes=allocated_bytes,
                  freeBytes=len(image)-allocated_bytes, sha256=sha256(image),
                  preservedFiles=sorted(preserved), files=[
                      dict(path=name, bytes=len(content), sha256=sha256(content))
                      for name, content in sorted(contents.items())])
    with report_path.open("x", encoding="utf-8") as stream:
        json.dump(report, stream, indent=2)
        stream.write("\n")
    print(json.dumps(dict(image=str(output), sha256=report["sha256"],
                          fileCount=len(contents), preservedCount=len(preserved),
                          freeBytes=report["freeBytes"])))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--extracted", required=True)
    parser.add_argument("--web", required=True)
    parser.add_argument("--output", required=True)
    args = parser.parse_args()
    build(args.extracted, args.web, args.output)
