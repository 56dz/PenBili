#!/usr/bin/env python3
"""PenBili 直播链路验证探针（服务端/本机通用，不含任何凭据）

用法：
  # 自动找一个正在直播的房间（WBI 签名搜 search_type=live，默认关键词「游戏」）
  python3 tools/live_probe.py --cookie-file /tmp/.pbcookie --base http://127.0.0.1:2050
  # 指定房间
  python3 tools/live_probe.py --cookie-file /tmp/.pbcookie --room 12345 --play-secs 15
  # 只验服务端输出（不取源、不打 B 站）
  python3 tools/live_probe.py --room 12345 --base http://127.0.0.1:2050 --skip-source

做四件事：
  ① 找在播房间（可选，WBI 签名搜索）
  ② 用【笔端 cookie】取源（qn=250 转码源 / qn=80 直通源），报告线路数
  ③ 打服务端 /live，跟随 302 拉播放列表 → 取一个真实存在的分片 → ffprobe 校验音视频轨
  ④ --play-secs 用本机 ffmpeg 直接拉 m3u8 播 N 秒，报告退出码与告警
"""
import argparse
import hashlib
import json
import os
import re
import subprocess
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

UA = ("Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
      "(KHTML, like Gecko) Chrome/124.0.0.0 Safari/537.36")
MIXIN_TABLE = [46, 47, 18, 2, 53, 8, 23, 32, 15, 50, 10, 31, 58, 3, 45, 35, 27, 43, 5, 49,
               33, 9, 42, 19, 29, 28, 14, 39, 12, 38, 41, 13, 37, 48, 7, 16, 24, 55, 40, 61,
               26, 17, 0, 1, 60, 51, 30, 4, 22, 25, 54, 21, 56, 59, 6, 63, 57, 62, 11, 36, 20,
               34, 44, 52]


def _headers(cookie="", referer="https://live.bilibili.com/"):
    h = {"User-Agent": UA, "Referer": referer, "Accept": "application/json, text/plain, */*",
         "Accept-Language": "zh-CN,zh;q=0.9", "Origin": "https://www.bilibili.com"}
    if cookie:
        h["Cookie"] = cookie
    return h


def jget(url, cookie="", referer="https://live.bilibili.com/", timeout=12):
    req = urllib.request.Request(url, headers=_headers(cookie, referer))
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return json.loads(r.read().decode("utf8", "replace"))


def _enc(v):
    return urllib.parse.quote(str(v).replace("!", "").replace("'", "").replace("(", "")
                             .replace(")", "").replace("*", ""), safe="")


def wbi_keys(cookie):
    j = jget("https://api.bilibili.com/x/web-interface/nav", cookie, "https://www.bilibili.com/")
    d = j.get("data") or {}
    wi = d.get("wbi_img") or {}
    ik = (wi.get("img_url") or "").rsplit("/", 1)[-1].split(".")[0]
    sk = (wi.get("sub_url") or "").rsplit("/", 1)[-1].split(".")[0]
    if len(ik) != 32 or len(sk) != 32:
        raise RuntimeError("nav wbi_img 缺失 (code=%s)" % j.get("code"))
    mixin = "".join((ik + sk)[i] for i in MIXIN_TABLE)[:32]
    return mixin


def wbi_query(params, mixin):
    p = dict(params)
    p["wts"] = str(int(time.time()))
    q = "&".join("%s=%s" % (_enc(k), _enc(p[k])) for k in sorted(p))
    return q + "&w_rid=" + hashlib.md5((q + mixin).encode("utf8")).hexdigest()


def search_live(cookie, keyword, page=1):
    mixin = wbi_keys(cookie)
    q = wbi_query({"search_type": "live", "keyword": keyword, "page": page}, mixin)
    j = jget("https://api.bilibili.com/x/web-interface/wbi/search/type?" + q, cookie,
             "https://www.bilibili.com/")
    if j.get("code") != 0:
        raise RuntimeError("search code=%s %s" % (j.get("code"), (j.get("message") or "")[:60]))
    res = (j.get("data") or {}).get("result")
    arr = res if isinstance(res, list) else (res or {}).get("live_room") or []
    out = []
    for it in arr:
        rid = int(it.get("roomid") or 0)
        if rid > 0:
            out.append({"roomid": rid, "title": re.sub("<[^>]+>", "", it.get("title") or ""),
                        "uname": it.get("uname") or "", "online": it.get("online") or 0})
    return out


def fetch_sources(room, cookie, qn, module_dir=None):
    """复用服务端同款实现（保证探针与服务端行为一致）。"""
    if module_dir:
        sys.path.insert(0, module_dir)
    try:
        import live_proxy as lp
        return lp._fetch_live_urls(room, cookie, qn)
    except Exception:
        pass
    # 退化：没装模块就自己实现同一契约
    h = _headers(cookie)
    url = ("https://api.live.bilibili.com/xlive/web-room/v2/index/getRoomPlayInfo"
           "?room_id=%d&qn=%d&protocol=0,1&format=0,1,2&codec=0,1,2&platform=web&ptype=8"
           % (int(room), int(qn)))
    req = urllib.request.Request(url, headers=h)
    with urllib.request.urlopen(req, timeout=12) as r:
        j = json.loads(r.read().decode("utf8", "replace"))
    if j.get("code") != 0:
        raise RuntimeError("getRoomPlayInfo code=%s" % j.get("code"))
    playurl = (((j.get("data") or {}).get("playurl_info") or {}).get("playurl")) or {}
    flv, hls = [], []
    for stream in playurl.get("stream") or []:
        for fmt in stream.get("format") or []:
            for codec in fmt.get("codec") or []:
                if codec.get("codec_name") not in ("h264", "avc"):
                    continue
                base = codec.get("base_url") or ""
                for ui in codec.get("url_info") or []:
                    full = (ui.get("host") or "") + base + (ui.get("extra") or "")
                    if not full.startswith("https://") or full in flv or full in hls:
                        continue
                    (hls if urllib.parse.urlparse(full).path.endswith(".m3u8") else flv).append(full)
    return flv + hls


def http_get(url, timeout=10):
    req = urllib.request.Request(url, headers={"User-Agent": UA})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return r.status, dict(r.headers), r.read()


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return None


def resolve_live(base, room, cookie, extra=""):
    """打 /live，返回 302 的 Location（播放列表地址）。"""
    url = "%s/live?room=%d&ck=%s%s" % (base.rstrip("/"), room, urllib.parse.quote(cookie, safe=""), extra)
    op = urllib.request.build_opener(NoRedirect)
    try:
        op.open(urllib.request.Request(url, headers={"User-Agent": UA}), timeout=15)
        raise RuntimeError("expected 302, got 2xx")
    except urllib.error.HTTPError as e:
        if e.code in (301, 302, 303, 307, 308):
            return e.headers.get("Location")
        raise


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cookie-file", default="", help="含 B 站 cookie 的文件（如 /tmp/.pbcookie）")
    ap.add_argument("--room", type=int, default=0)
    ap.add_argument("--keyword", default="游戏")
    ap.add_argument("--qn", type=int, default=250, help="取源清晰度：250=超清(转码源) 80=流畅(直通源)")
    ap.add_argument("--base", default="http://127.0.0.1:2050", help="服务端地址")
    ap.add_argument("--module-dir", default="", help="live_proxy.py 所在目录（复用其取源实现）")
    ap.add_argument("--skip-source", action="store_true")
    ap.add_argument("--play-secs", type=float, default=0, help=">0 时用 ffmpeg 拉 m3u8 播 N 秒")
    ap.add_argument("--wait-seg", type=float, default=25, help="等播放列表/分片就绪的秒数")
    args = ap.parse_args()

    cookie = ""
    if args.cookie_file:
        cookie = open(args.cookie_file, encoding="utf-8").read().strip()
    print("== cookie: %s (len=%d)" % ("yes" if cookie else "no", len(cookie)))

    room = args.room
    if not room:
        print("== 搜索在播房间 keyword=%s" % args.keyword)
        try:
            rooms = search_live(cookie, args.keyword)
        except Exception as e:
            print("   搜索失败:", str(e)[:120]); return 2
        for it in rooms[:8]:
            print("   %-10d %-32s %-14s online=%s" % (it["roomid"], it["title"][:32], it["uname"][:14], it["online"]))
        if not rooms:
            print("   无结果"); return 2
        for it in rooms:
            room = it["roomid"]; break
    print("== room = %d" % room)

    if not args.skip_source:
        try:
            urls = fetch_sources(room, cookie, args.qn, args.module_dir or None)
            print("== 取源 qn=%d 线路=%d" % (args.qn, len(urls)))
            for u in urls[:3]:
                p = urllib.parse.urlparse(u)
                print("   %s%s%s" % (p.scheme + "://", p.netloc, p.path[:60]))
        except Exception as e:
            print("== 取源失败:", str(e)[:160])
            if not cookie:
                print("   （可能需要 cookie：试 --cookie-file）")

    loc = resolve_live(args.base, room, cookie)
    print("== /live → %s" % loc)
    seg = None
    deadline = time.time() + args.wait_seg
    last = ""
    while time.time() < deadline:
        try:
            st, hd, body = http_get(loc, timeout=8)
        except Exception as e:
            last = str(e)[:80]; time.sleep(1); continue
        txt = body.decode("utf8", "replace")
        if "#EXTINF" in txt:
            base_url = loc.rsplit("/", 1)[0]
            want = [l.strip() for l in txt.splitlines() if l.strip().endswith(".ts") or l.strip().endswith(".m4s")]
            for name in want:
                try:
                    s2, h2, b2 = http_get(base_url + "/" + name, timeout=8)
                    seg = (name, s2, len(b2), b2)
                    break
                except Exception as e:
                    last = "%s -> %s" % (name, str(e)[:60])
            print("== 播放列表就绪：%d 条；首个可取分片=%s" % (len(want), seg[0] if seg else "无"))
            print("--- 列表头 ---")
            print("\n".join(txt.splitlines()[:10]))
            break
        last = txt[:80]
        time.sleep(1)
    if not seg:
        print("== 未取到分片（last=%s）" % last); return 3
    name, st, n, b2 = seg
    tmp = "/tmp/.probe_seg" + os.path.splitext(name)[1]
    open(tmp, "wb").write(b2)
    print("== 分片 %s HTTP %s %dB → %s" % (name, st, n, tmp))
    for exe in ("ffprobe", "/usr/bin/ffprobe"):
        try:
            r = subprocess.run([exe, "-hide_banner", "-i", tmp], capture_output=True, text=True, timeout=30)
            for line in (r.stdout + r.stderr).splitlines():
                if "Stream #" in line or "Duration" in line:
                    print("   " + line.strip())
            break
        except Exception:
            continue
    if args.play_secs > 0:
        print("== ffmpeg 拉 m3u8 播 %.0fs（模拟笔端消费）" % args.play_secs)
        cmd = ["ffmpeg", "-nostdin", "-hide_banner", "-loglevel", "warning",
               "-i", loc, "-t", str(args.play_secs), "-f", "null", "-"]
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=args.play_secs + 40)
        warn = [l for l in (r.stdout + r.stderr).splitlines() if l.strip()]
        print("   退出码=%s 告警行=%d" % (r.returncode, len(warn)))
        for l in warn[:6]:
            print("   ! " + l[:150])
    print("== OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
