#!/usr/bin/env python3
"""PenBili 直播流畅度测试框架（服务端 vs 客户端判决，2026-09-27）

在本机（真实客户端路径）拉取转码代理流并量化三层指标：
  ① read-gap   流读取时 >0.4s 无字节的间隙  → 服务端输出断供
  ② 音频包间隔 FLV 音频 tag 时间戳 >0.5s 断点 → 听感上的"卡一下"（本框架的核心指标）
  ③ 视频包间隔 同上                            → 区分"音频单断"还是"整流断"
输出时间窗口（epoch 起止）供与服务端 server.log 的 up start/same-url retry/switch 对齐判决。

用法：
    python tools/live_client_test.py [--seconds 45] [--room 0] [--host 192.168.5.224:2050]
    （--room 0 = 自动探测在播房间；--host 也可换 penbili.560726.best 测域名路径）
"""
import argparse
import json
import os
import struct
import sys
import time
import urllib.request
import urllib.parse
import http.cookiejar

UA = ("Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
      "(KHTML, like Gecko) Chrome/124.0.0.0 Safari/537.36")

_WBI_ORDER = [46, 47, 18, 2, 53, 8, 23, 32, 15, 50, 10, 31, 58, 3, 45, 35, 27, 43, 5, 49,
              33, 9, 42, 19, 29, 28, 14, 39, 12, 38, 41, 13, 37, 48, 7, 16, 24, 55, 40, 61,
              26, 17, 0, 1, 60, 51, 30, 4, 22, 25, 54, 21, 56, 59, 6, 63, 57, 62, 11, 36, 20,
              34, 44, 52]


def wbi_search(op, keyword):
    """带 WBI 签名的搜索（不签名时灵时不灵）。"""
    import hashlib
    j = jget(op, "https://api.bilibili.com/x/web-interface/nav")
    d = j.get("data") or {}
    ik = (d.get("wbi_img", {}).get("img_url", "") or "").rsplit("/", 1)[-1].split(".")[0]
    sk = (d.get("wbi_img", {}).get("sub_url", "") or "").rsplit("/", 1)[-1].split(".")[0]
    if not ik or not sk:
        return None
    nav = "".join((ik + sk)[i] for i in _WBI_ORDER)
    params = {"search_type": "live", "keyword": keyword, "page": "1"}
    params["wts"] = str(int(time.time()))
    q = urllib.parse.urlencode(sorted(params.items()))
    params["w_rid"] = hashlib.md5((q + nav).encode()).hexdigest()
    return jget(op, "https://api.bilibili.com/x/web-interface/wbi/search/type?"
                + urllib.parse.urlencode(params))

CANDIDATES = [22547649, 26966466, 22952362, 4472878, 22686972, 1498471, 8779377]


def make_opener():
    op = urllib.request.build_opener(
        urllib.request.HTTPCookieProcessor(http.cookiejar.CookieJar()))
    return op


def jget(op, u, ref="https://live.bilibili.com/"):
    h = {"User-Agent": UA, "Referer": ref}
    req = urllib.request.Request(u, headers=h)
    with op.open(req, timeout=12) as r:
        return json.loads(r.read().decode("utf8", "replace"))


def pick_room(op, room_arg):
    cands = ([room_arg] if room_arg else []) + list(CANDIDATES)
    # 实时搜索兜底（固定候选可能都已下播；带 WBI 签名）
    try:
        s = wbi_search(op, "原神")
        for r in (((s.get("data") or {}).get("result") or {}).get("live_room") or [])[:6]:
            if r.get("roomid"):
                cands.append(int(r["roomid"]))
        print("search rooms: %s" % cands[:8])
    except Exception as e:
        print("search fallback err: %s" % str(e)[:80])
    for rid in cands:
        if not rid:
            continue
        try:
            info = jget(op, "https://api.live.bilibili.com/room/v1/Room/room_init?id=%d" % rid)
            st = (info.get("data") or {}).get("live_status")
            print("  room %d live_status=%s" % (rid, st))
            if st == 1:
                return int(rid)
        except Exception as e:
            print("  room %d err: %s" % (rid, str(e)[:60]))
    return 0


def build_proxy_url(op, room, host):
    p = jget(op, "https://api.live.bilibili.com/room/v1/Room/playUrl?cid=%d&platform=pc&quality=0&ptype=16" % room)
    flv = ((p.get("data") or {}).get("durl") or [{}])[0].get("url")
    if not flv:
        return ""
    return "http://%s/live?u=%s&room=%d&lanes=4" % (host, urllib.parse.quote(flv, safe=""), room)


def collect_stream(url, seconds, dump_path):
    """拉流：每次 read 记 (t,n)——read 间隔直方图（区分 64KB 攒块假象 vs 真断供）
    + 超 1.2s 的大空隙（块攒 0.64s 是正常的，>1.2s 才是真断供）。"""
    h = {"User-Agent": UA}
    req = urllib.request.Request(url, headers=h)
    t0 = time.time()
    reads = []            # (相对秒, 字节数)
    big_gaps = []         # 真断供（>1.2s）
    total = 0
    resp = urllib.request.urlopen(req, timeout=30)
    with open(dump_path, "wb") as f:
        last_t = t0
        while time.time() - t0 < seconds:
            try:
                chunk = resp.read(65536)
            except Exception as e:
                big_gaps.append((round(last_t - t0, 2), "read-err: %s" % str(e)[:40]))
                break
            now = time.time()
            dt = now - last_t
            if dt > 1.2:
                big_gaps.append((round(last_t - t0, 2), round(dt, 2)))
            last_t = now
            if chunk:
                f.write(chunk)
                total += len(chunk)
                reads.append((round(now - t0, 3), len(chunk)))
    return t0, time.time() - t0, total, reads, big_gaps


def flv_stream_ts(path):
    """解析 FLV，返回 (audio_ts, video_ts, debug)（毫秒）。"""
    a_ts, v_ts = [], []
    dbg = ""
    try:
        with open(path, "rb") as f:
            data = f.read()
    except Exception as e:
        return a_ts, v_ts, "open-err: %s" % e
    if len(data) < 13:
        return a_ts, v_ts, "too short: %d" % len(data)
    if data[0:3] != b"FLV":
        return a_ts, v_ts, "head=%s size=%d" % (data[:8].hex(), len(data))
    data_offset = struct.unpack(">I", data[5:9])[0]
    if not (13 <= data_offset <= 256):
        data_offset = 13
    off = data_offset
    n = len(data)
    bad = 0
    while off + 11 < n:
        ttype = data[off]
        size = (data[off + 1] << 16) | (data[off + 2] << 8) | data[off + 3]
        if size <= 0 or off + 11 + size + 4 > n:
            bad += 1
            if bad > 3:
                break
            off += 1  # 容错步进
            continue
        ts = ((data[off + 4] << 16) | (data[off + 5] << 8) | data[off + 6]) | (data[off + 7] << 24)
        if ttype == 8:
            a_ts.append(ts)
        elif ttype == 9:
            v_ts.append(ts)
        off = off + 11 + size + 4
    dbg = "size=%d off0=%d bad=%d" % (n, data_offset, bad)
    return a_ts, v_ts, dbg


def ts_gaps(ts_list, threshold_ms=500):
    """时间戳序列中 >threshold 的断点 [(秒, 间隔ms)]。"""
    out = []
    for i in range(1, len(ts_list)):
        d = ts_list[i] - ts_list[i - 1]
        if d > threshold_ms:
            out.append((round(ts_list[i - 1] / 1000.0, 2), d))
    return out


def main():
    ap = argparse.ArgumentParser(description="PenBili live stream fluency test")
    ap.add_argument("--seconds", type=int, default=45)
    ap.add_argument("--room", type=int, default=0)
    ap.add_argument("--host", default="192.168.5.224:2050",
                    help="proxy host:port 或域名(如 penbili.560726.best)")
    args = ap.parse_args()

    op = make_opener()
    room = pick_room(op, args.room)
    if not room:
        print("FAIL: no live room")
        sys.exit(1)
    url = build_proxy_url(op, room, args.host)
    if not url:
        print("FAIL: no flv url")
        sys.exit(1)
    dump = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".deploy",
                        "live_probe.flv")
    os.makedirs(os.path.dirname(dump), exist_ok=True)
    print("room=%d host=%s seconds=%d" % (room, args.host, args.seconds))
    print("T_START_EPOCH=%.0f" % time.time())

    t0, dur, total, reads, big_gaps = collect_stream(url, args.seconds, dump)

    # read 间隔分布（区分 64KB 攒块假象 vs 真断供）
    intervals = []
    for i in range(1, len(reads)):
        intervals.append(round(reads[i][0] - reads[i - 1][0], 3))
    print("\n== read interval stats (n=%d) ==" % len(intervals))
    if intervals:
        iv = sorted(intervals)
        print("  min=%.3fs p50=%.3fs p90=%.3fs max=%.3fs avg=%.3fs"
              % (iv[0], iv[len(iv) // 2], iv[int(len(iv) * 0.9)], iv[-1],
                 sum(iv) / len(iv)))
        over1 = [x for x in intervals if x > 1.0]
        print("  >1.0s reads: %d (真断供信号)" % len(over1))

    print("\n== big gaps (>1.2s = 真断供) ==")
    if big_gaps:
        for g in big_gaps:
            print("  @%.1fs gap=%.2fs %s" % (g[0], g[1] if isinstance(g[1], float) else 0, g[1]))
    else:
        print("  none (stream continuous at read level)")

    # FLV 解析（带 debug）
    a_ts, v_ts, dbg = flv_stream_ts(dump)
    print("\n== FLV parse: audio=%d video=%d %s" % (len(a_ts), len(v_ts), dbg))
    a_gaps = ts_gaps(a_ts, 500)
    v_gaps = ts_gaps(v_ts, 500)
    print("\n== audio ts gaps >500ms (听感卡顿) ==")
    if a_gaps:
        for g in a_gaps[:20]:
            print("  @%.1fs interval=%dms" % (g[0], g[1]))
        if len(a_gaps) > 20:
            print("  ... total %d" % len(a_gaps))
    else:
        print("  none (audio continuous)")
    print("\n== video ts gaps >500ms ==")
    if v_gaps:
        for g in v_gaps[:20]:
            print("  @%.1fs interval=%dms" % (g[0], g[1]))
        if len(v_gaps) > 20:
            print("  ... total %d" % len(v_gaps))
    else:
        print("  none (video continuous)")

    print("\n== summary ==")
    print("total=%dKB dur=%.1fs avg=%.0fKB/s" % (total // 1024, dur, (total / 1024) / max(dur, 0.1)))
    verdict = []
    if big_gaps:
        verdict.append("真断供 %d 次(>1.2s) -> 服务端输出/网络" % len(big_gaps))
    if a_gaps and not v_gaps:
        verdict.append("AUDIO-ONLY gaps -> 音频路径(桥/FIFO)")
    if a_gaps and v_gaps:
        verdict.append("whole-stream gaps -> server/network")
    if not a_gaps and not v_gaps and not big_gaps:
        verdict.append("ALL SMOOTH in this window -> if pen still stutters = pen-side (decode/clock/BT)")
    print("VERDICT: " + (" / ".join(verdict) if verdict else "see above"))
    print("T_START_EPOCH=%.0f T_END_EPOCH=%.0f  (align with server log)" % (t0, time.time()))
    print("dump=%s" % dump)


if __name__ == "__main__":
    main()
