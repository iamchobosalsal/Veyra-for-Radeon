"""Read-only CUDA container inventory; does not extract or modify kernel data.

This establishes what the installed files contain, not that an AMD translator
exists or that every dynamically loaded kernel has been observed.
"""
import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import struct


def inventory(path):
    data = path.read_bytes();position = 0;containers = [];rejected = 0
    while (position := data.find(b"\x50\xed\x55\xba", position)) >= 0:
        start = position;position += 4
        if start + 16 > len(data): continue
        _, version, header, size = struct.unpack_from("<IHHQ", data, start)
        if version != 1 or header != 16 or size > len(data) - start - 16: continue
        cursor = start + 16;end = cursor + size;entries = []
        try:
            while cursor < end:
                if cursor + 64 > end: raise ValueError()
                kind, ver, head, length = struct.unpack_from("<HHIQ", data, cursor)
                if kind not in (1, 2) or ver != 0x101 or not 64 <= head <= 4096 or head % 8 or not length or cursor + head + length > end:
                    raise ValueError()
                sm = struct.unpack_from("<I", data, cursor + 28)[0]
                entries.append(("ptx" if kind == 1 else "cubin", sm));cursor += head + length
            if not entries: raise ValueError()
            containers.append(entries);position = end
        except ValueError: rejected += 1
    counts = Counter(entry for container in containers for entry in container)
    return {"file": path.name, "sha256": hashlib.sha256(data).hexdigest(),
            "fatbinContainers": len(containers),
            "entryCounts": {f"{kind}_sm{sm}": count for (kind, sm), count in sorted(counts.items())},
            "rejectedCandidates": rejected, "binaryModified": False,
            "amdExecutionTested": False}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--runtime", required=True, type=Path)
    parser.add_argument("--report", required=True, type=Path)
    args = parser.parse_args()
    result = [inventory(args.runtime / name) for name in ("nvVFXVideoFrameGeneration.dll", "NVCVImage.dll")]
    with args.report.open("x", encoding="utf-8") as out: json.dump(result, out, indent=2)
    print(json.dumps(result))


if __name__ == "__main__": main()
