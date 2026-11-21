"""Build the standalone "本地视频" miniapp AMR from a base file-manager AMR.

Usage:
  python pack_standalone.py <base.amr> <replace_dir> <version> <out.amr> \
      [--appid ID] [--appname NAME] [--icon FILE]

Only a lean set of entries is shipped: the app entry, the pages used by the
standalone app, their images and the arm shell JSAPI library.  Everything else
in the base package is dropped.  manifest cert is regenerated for every shipped
file (manifest.json and the icon are exempt, like in the stock packages).
"""
import hashlib
import json
import os
import struct
import sys
import zlib

LOCAL_SIG = 0x04034B50
CD_SIG = 0x02014B50
EOCD_SIG = 0x06054B50
DESC_SIG = 0x08074B50

KEEP = {
    "app.js.bin",
    "index.js.bin",
    "fs.js.bin",
    "VideoPlayer.js.bin",
    "LegacyVideo.js.bin",
    "ShowPicturePage.js.bin",
    "RunScriptPage.js.bin",
    "index-c39e9ced.js.bin",
    "app_icon.png",
    "libs/arm/libjsapi_shell.so",
}
KEEP_PREFIXES = ("images/",)


def parse_amr(raw):
    eocd = raw.rfind(b"PK\x05\x06")
    (_sig, _disk, _cdisk, _n_this, n_total, _csize, cd_off, _clen) = struct.unpack(
        "<IHHHHIIH", raw[eocd:eocd + 22])
    entries = []
    pos = cd_off
    for _ in range(n_total):
        (sig, cver, cneed, cflags, cmethod, cmtime, cmdate, ccrc, ccsize, cusize,
         cnlen, celen, cclen, _dstart, _iattr, cextattr, coffset) = struct.unpack(
            "<IHHHHHHIIIHHHHHII", raw[pos:pos + 46])
        assert sig == CD_SIG, "bad central directory at %d" % pos
        name = raw[pos + 46:pos + 46 + cnlen].decode("utf-8")
        lname_len, lextra_len = struct.unpack("<HH", raw[coffset + 26:coffset + 30])
        data_off = coffset + 30 + lname_len + lextra_len
        entries.append({
            "name": name,
            "crc": ccrc,
            "csize": ccsize,
            "usize": cusize,
            "compressed": raw[data_off:data_off + ccsize],
            "version_made_by": cver,
            "version_needed": cneed,
            "extattr": cextattr,
            "date": cmdate,
            "time": cmtime,
        })
        pos += 46 + cnlen + celen + cclen
    return entries


def inflate_raw(data):
    return zlib.decompressobj(-15).decompress(data)


def deflate_raw(data):
    obj = zlib.compressobj(9, zlib.DEFLATED, -15)
    return obj.compress(data) + obj.flush()


def main():
    args = sys.argv[1:]
    if len(args) < 4:
        print(__doc__)
        return 2
    base_amr, replace_dir, version, out_amr = args[:4]
    appid = "8001999000000001"
    appname = "本地视频"
    icon = "app_icon.png"
    full = False
    rest = args[4:]
    i = 0
    while i < len(rest):
        if rest[i] == "--appid" and i + 1 < len(rest):
            appid = rest[i + 1]
            i += 2
        elif rest[i] == "--appname" and i + 1 < len(rest):
            appname = rest[i + 1]
            i += 2
        elif rest[i] == "--icon" and i + 1 < len(rest):
            icon = rest[i + 1]
            i += 2
        elif rest[i] == "--full":
            # 保留底包的全部功能页，只剔除 arm64 库（设备是 armv7，装不下两份）。
            full = True
            i += 1
        else:
            print("unknown argument: " + rest[i])
            return 2

    raw_base = open(base_amr, "rb").read()
    entries = parse_amr(raw_base)

    overrides = {}
    for root_dir, _dirs, files in os.walk(replace_dir):
        for name in files:
            path = os.path.join(root_dir, name)
            relative = os.path.relpath(path, replace_dir).replace(os.sep, "/")
            overrides[relative] = open(path, "rb").read()

    shipped = []
    for entry in entries:
        name = entry["name"]
        if name == "manifest.json":
            continue
        if name in overrides:
            payload = overrides[name]
        elif full:
            if name.startswith("libs/arm64"):
                continue
            payload = inflate_raw(entry["compressed"])
        elif name in KEEP or name.startswith(KEEP_PREFIXES):
            payload = inflate_raw(entry["compressed"])
        else:
            continue
        shipped.append((entry, payload))

    base_names = set(entry["name"] for entry in entries)
    extra_names = sorted(name for name in overrides
                         if name not in base_names and name != "manifest.json")
    if extra_names:
        # 新增文件（例如 libs/arm/libjsapi_picpinch.so）：借用一个现有 .so
        # 的 zip 元数据（时间戳/版本/权限），内容用替换目录里的。
        template = next((entry for entry in entries
                         if entry["name"].endswith(".so")), entries[0])
        for name in extra_names:
            extra = dict(template)
            extra["name"] = name
            shipped.append((extra, overrides[name]))
        print("added extra entries:", ", ".join(extra_names))

    manifest_entry = next(e for e in entries if e["name"] == "manifest.json")
    base_manifest = json.loads(inflate_raw(manifest_entry["compressed"]).decode("utf-8"))
    manifest = {
        "appName": appname,
        "version": version,
        "appid": appid,
        "icon": icon,
        "quickjs": base_manifest.get("quickjs", {"version": "20200705", "bigNum": False}),
        "cert": {},
    }

    out = bytearray()
    central = bytearray()
    for entry, payload in shipped:
        name = entry["name"]
        compressed = deflate_raw(payload)
        usize = len(payload)
        crc = zlib.crc32(payload) & 0xFFFFFFFF
        if name not in ("manifest.json", icon):
            manifest["cert"][name] = {
                "size": len(payload),
                "md5": hashlib.md5(payload).hexdigest(),
            }
        name_b = name.encode("utf-8")
        offset = len(out)
        out += struct.pack("<IHHHHHIIIHH", LOCAL_SIG, entry["version_needed"], 0x0808,
                           8, entry["time"], entry["date"], 0, 0, 0, len(name_b), 0)
        out += name_b
        out += compressed
        out += struct.pack("<IIII", DESC_SIG, crc, len(compressed), usize)
        central += struct.pack("<IHHHHHHIIIHHHHHII", CD_SIG, entry["version_made_by"],
                               entry["version_needed"], 0x0808, 8, entry["time"],
                               entry["date"], crc, len(compressed), usize,
                               len(name_b), 0, 0, 0, 0, entry["extattr"], offset)
        central += name_b

    manifest_bytes = json.dumps(manifest, indent=2, ensure_ascii=False).encode("utf-8")
    name_b = b"manifest.json"
    compressed = deflate_raw(manifest_bytes)
    usize = len(manifest_bytes)
    crc = zlib.crc32(manifest_bytes) & 0xFFFFFFFF
    offset = len(out)
    out += struct.pack("<IHHHHHIIIHH", LOCAL_SIG, manifest_entry["version_needed"], 0x0808,
                       8, manifest_entry["time"], manifest_entry["date"], 0, 0, 0,
                       len(name_b), 0)
    out += name_b
    out += compressed
    out += struct.pack("<IIII", DESC_SIG, crc, len(compressed), usize)
    central += struct.pack("<IHHHHHHIIIHHHHHII", CD_SIG, manifest_entry["version_made_by"],
                           manifest_entry["version_needed"], 0x0808, 8,
                           manifest_entry["time"], manifest_entry["date"],
                           crc, len(compressed), usize, len(name_b), 0, 0, 0, 0,
                           manifest_entry["extattr"], offset)
    central += name_b

    cd_offset = len(out)
    out += central
    total_entries = len(shipped) + 1
    out += struct.pack("<IHHHHIIH", EOCD_SIG, 0, 0, total_entries, total_entries,
                       len(central), cd_offset, 0)
    open(out_amr, "wb").write(bytes(out))

    print("wrote %s: %s %s v%s, %d entries, %d bytes" % (
        out_amr, appname, appid, version, total_entries, len(out)))
    for name in [e["name"] for e, _ in shipped] + ["manifest.json"]:
        print("  " + name)

    import zipfile
    with zipfile.ZipFile(out_amr) as zf:
        bad = zf.testzip()
        print("testzip:", bad or "ok")


if __name__ == "__main__":
    sys.exit(main())
