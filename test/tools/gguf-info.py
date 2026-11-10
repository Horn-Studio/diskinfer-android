#!/usr/bin/env python3
"""Read-only GGUF metadata/tensor inspector used by the Windows port verification.

Prints one JSON object to stdout: version, tensor count, selected metadata values,
tensor size table and the largest tensor. Never modifies the file.

Usage: python gguf-info.py <model.gguf>
"""
import json
import struct
import sys

# ggml_type -> (block_size, type_size)
GGML_TYPES = {
    0: ("F32", 1, 4),
    1: ("F16", 1, 2),
    2: ("Q4_0", 32, 18),
    3: ("Q4_1", 32, 20),
    6: ("Q5_0", 32, 22),
    7: ("Q5_1", 32, 24),
    8: ("Q8_0", 32, 34),
    9: ("Q8_1", 32, 36),
    10: ("Q2_K", 256, 84),
    11: ("Q3_K", 256, 110),
    12: ("Q4_K", 256, 144),
    13: ("Q5_K", 256, 176),
    14: ("Q6_K", 256, 210),
    15: ("Q8_K", 256, 292),
    16: ("IQ2_XXS", 256, 66),
    17: ("IQ2_XS", 256, 74),
    18: ("IQ3_XXS", 256, 98),
    19: ("IQ1_S", 256, 50),
    20: ("IQ4_NL", 32, 18),
    21: ("IQ3_S", 256, 110),
    22: ("IQ2_S", 256, 82),
    23: ("IQ4_XS", 256, 136),
    24: ("I8", 1, 1),
    25: ("I16", 1, 2),
    26: ("I32", 1, 4),
    27: ("I64", 1, 8),
    28: ("F64", 1, 8),
    29: ("IQ1_M", 256, 56),
    30: ("BF16", 1, 2),
}

# scalar metadata value types: struct format
SCALAR_FMT = {
    0: "<B", 1: "<b", 2: "<H", 3: "<h", 4: "<I", 5: "<i",
    6: "<f", 7: "<?", 10: "<Q", 11: "<q", 12: "<d",
}

INTERESTING = (
    "general.architecture",
    "general.name",
    "general.quantization_version",
    "general.file_type",
    "general.size_label",
)


def read_exact(f, n):
    b = f.read(n)
    if len(b) != n:
        raise EOFError("unexpected end of file")
    return b


def read_u32(f):
    return struct.unpack("<I", read_exact(f, 4))[0]


def read_u64(f):
    return struct.unpack("<Q", read_exact(f, 8))[0]


def read_str(f, u64len):
    n = read_u64(f) if u64len else read_u32(f)
    return read_exact(f, n).decode("utf-8", errors="replace")


def read_value(f, vtype, u64len):
    if vtype in SCALAR_FMT:
        fmt = SCALAR_FMT[vtype]
        return struct.unpack(fmt, read_exact(f, struct.calcsize(fmt)))[0]
    if vtype == 8:
        return read_str(f, u64len)
    if vtype == 9:
        etype = read_u32(f)
        n = read_u64(f) if u64len else read_u32(f)
        vals = [read_value(f, etype, u64len) for _ in range(n)]
        return vals
    raise ValueError("unknown metadata value type %d" % vtype)


def main():
    path = sys.argv[1]
    with open(path, "rb") as f:
        magic = read_exact(f, 4)
        if magic != b"GGUF":
            raise SystemExit("not a GGUF file")
        version = read_u32(f)
        u64len = version >= 3
        tensor_count = read_u64(f) if u64len else read_u32(f)
        kv_count = read_u64(f) if u64len else read_u32(f)

        meta = {}
        for _ in range(kv_count):
            key = read_str(f, u64len)
            vtype = read_u32(f)
            val = read_value(f, vtype, u64len)
            if key in INTERESTING or key.startswith("general."):
                meta[key] = val
            for suffix in (".block_count", ".embedding_length", ".context_length",
                           ".attention.head_count"):
                if key.endswith(suffix):
                    meta[key] = val

        tensors = []
        for _ in range(tensor_count):
            name = read_str(f, u64len)
            n_dims = read_u32(f)
            ne = [read_u64(f) for _ in range(n_dims)]
            ttype = read_u32(f)
            off = read_u64(f)
            tname, blk, tsize = GGML_TYPES.get(ttype, ("UNKNOWN_%d" % ttype, 0, 0))
            n_elem = 1
            for d in ne:
                n_elem *= d
            nbytes = (n_elem // blk) * tsize if blk else 0
            tensors.append({
                "name": name,
                "type": tname,
                "type_id": ttype,
                "ne": ne,
                "n_elem": n_elem,
                "nbytes": nbytes,
                "offset": off,
            })

    largest = max(tensors, key=lambda t: t["nbytes"])
    embed = next((t for t in tensors if t["name"] == "token_embd.weight"), None)
    out = {
        "path": path,
        "size": __import__("os").path.getsize(path),
        "version": version,
        "tensor_count": tensor_count,
        "kv_count": kv_count,
        "metadata": meta,
        "total_tensor_bytes": sum(t["nbytes"] for t in tensors),
        "total_tensor_mib": round(sum(t["nbytes"] for t in tensors) / (1 << 20), 2),
        "largest_tensor": largest["name"],
        "largest_tensor_mib": round(largest["nbytes"] / (1 << 20), 2),
        "largest_tensor_bytes": largest["nbytes"],
        "largest_tensor_type": largest["type"],
        "token_embd": {
            "type": embed["type"],
            "ne": embed["ne"],
            "nbytes": embed["nbytes"],
            "mib": round(embed["nbytes"] / (1 << 20), 2),
        } if embed else None,
        "type_histogram": {},
    }
    hist = {}
    for t in tensors:
        hist[t["type"]] = hist.get(t["type"], 0) + 1
    out["type_histogram"] = hist
    # top 10 tensors by size
    top = sorted(tensors, key=lambda t: t["nbytes"], reverse=True)[:10]
    out["top_tensors"] = [{"name": t["name"], "type": t["type"],
                           "ne": t["ne"], "mib": round(t["nbytes"] / (1 << 20), 2)}
                          for t in top]
    json.dump(out, sys.stdout, indent=2)
    sys.stdout.write("\n")


if __name__ == "__main__":
    main()
