#!/usr/bin/env python3
"""PenBili 直播转码服务器 v5 —— HLS（点播化分片）输出

== 为什么推倒 v4 ==
v4 是一条 chunked FLV 长连接：客户端把它当"直播流"消费，于是服务端与客户端互相耦合 ——
服务端要蓄水（buf）、首块必须含音频 tag、换线要剥 FLV 头、客户端要墙钟节流/丢音频快进……
每一处都是真机踩坑换来的补丁。
v5 改成 **HLS**：服务端把直播转码成 m3u8 + 定长分片（默认 1s/片、滑动窗口），
客户端只当"点播地址"播放。ffmpeg 的 hls demuxer 负责拉列表/取分片/维持窗口，
笔端点播代码几乎零改动（这是用户指定的方向）。

== 协议 ==
  GET /health
      → 200 {ok, host, lan:[...], load:{sessions, load1, high}, version, max_sessions}
        host = 请求 Host 头。笔端把它缓存为"重定向解析结果"（跳过 301 跳跃）；
        app 启动时后台探测该缓存地址，2s 无响应即重走入口域名重新解析。

  GET /live?room=<id>[&ck=<cookie>][&res=254|360|480][&bv=<码率>][&trans=0|1]
              [&buf=<ms>][&seg=ts|fmp4]
      → 302 Location: http://<Host>/hls/<sid>/index.m3u8
        （ffmpeg 默认跟随 302；终点以 .m3u8 结尾 → hls demuxer 必定识别）
      room  必填，B 站直播间号
      ck    笔端 B 站 cookie（服务端用它拉 720p 源，并按 cookie 哈希隔离用户）
      res   转码输出**高度**（默认 254=匹配笔端屏幕，见下）
      bv    视频码率（默认 700k，钳 300k~2500k）
      trans 1=转码（默认）0=直通（-c copy，服务端零编码开销，分片时长随源关键帧）
      buf   笔端想握的缓冲时长（ms，默认 6000）→ 决定服务端播放列表窗口长度
      seg   分片类型 ts（默认，最稳）| fmp4

  GET /hls/<sid>/index.m3u8    → 播放列表（live，omit_endlist）
  GET /hls/<sid>/<分片文件>     → 分片（video/mp2t 或 video/mp4）

  · sid = sha1(room|ck|res|bv|trans|seg)[:12]
        → **同参数重入复用同一会话**：断线重连/退出再进 = 秒开（不重新起转码）
  · 无任何列表/分片请求超过 IDLE_STOP_S → 停 ffmpeg、清目录、回收会话
  · 并发会话 ≥MAX_SESSIONS → 503
  · 同 cookie 换房间 → 旧会话让位
  · ffmpeg 退出（源站停顿/EOF/换线）→ 自动重取新源地址重开（CDN 直链会过期）
  · 会话目录 <HLS_ROOT>/<sid>/，进程退出后残留由 janitor 清理

== 安全 ==
  cookie 只保留可打印 ASCII（防 header 注入）；sess 目录/分片文件名白名单校验（防穿越）；
  --dev-src-allow 才允许 ?src= 注入非 B 站源（仅供本机自测，默认关闭）。
"""

import argparse
import hashlib
import json
import os
import re
import select
import shutil
import socket
import subprocess
import sys
import threading
import time
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

VERSION = "5.1"
UA = "Mozilla/5.0 (Windows NT 10.0; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/124.0.0.0 Safari/537.36"
FFMPEG = shutil.which("ffmpeg") or "ffmpeg"

HLS_ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "hls_cache")

MAX_SESSIONS = 6           # 并发会话上限（HLS 每会话 1 个 ffmpeg，N5100 实测 3~4 路 480p 吃满）
LOAD_HIGH_SESSIONS = 3      # 会话数达此值即上报"负载高"
IDLE_STOP_S = 25           # 无列表/分片请求超过此秒数 → 回收会话（客户端已退出）
MAX_RESTARTS = 30          # 单会话内 ffmpeg 最大重开次数（换线/源站抖动）
PLAYLIST_WAIT_S = 12       # 首次列表最长等待（客户端 ffmpeg 读超时必须 ≥ 此值）

TRANS_BV = "700k"          # 默认视频码率
TRANS_BV_MIN_K = 300
TRANS_BV_MAX_K = 2500
TRANS_AUDIO = "96k"
# 默认输出高度 = **254**，即视口视频列的高度（逻辑/物理都是 254，1:1 无缩放）。
# 为什么是这个怪数字（2026-10-07 真机实测，别改回 480）：
#   笔端 player.c 的 vf 是 `scale=<out_h>:<out_w>,transpose=2,format=rgb32`，本机软解+缩放+转置+rgb32
#   实测吞吐（5s 真实直播分片，同码率 700k，笔端 ffmpeg）：
#     854x480 → 0.83x（跌破实时，播放永远追不上 → 画面卡、帧率只有 ~4fps）
#     640x360 → 1.03x（零余量）
#     452x254 → 2.3~2.8x（充足）  ← 输出尺寸 == 视口尺寸时 sws_scale 走恒等快路径被跳过
#   而笔的视口就是 452x254（800x254 逻辑屏扣掉两侧 174 栏），再高的分辨率只是被缩回去，纯浪费 CPU。
#   故：服务端按 -2:254 输出（16:9 源 → 正好 452x254），画质无损、余量 3 倍。
TRANS_H = 254              # 默认输出高度（254=匹配屏幕；360/480 仅调试用，真机会卡）
TRANS_H_ALLOWED = (254, 360, 480)
FPS = 30                   # 输出帧率（B 站直播源多为 30fps，逐帧对应避免抖动）
SEG_S = 1                  # 分片时长（秒）；与 -g FPS 对齐 → 每片恰好 1 个关键帧
WINDOW_DEFAULT_MS = 6000   # 默认窗口（ms）
WINDOW_MIN_MS = 2000
WINDOW_MAX_MS = 20000
QN_TRANS = 250             # 转码源：超清 720p（cookie 解锁）
QN_PASSTHRU = 80           # 直通源：流畅（多数房间 ≈360p）

DEV_SRC_ALLOW = False      # --dev-src-allow 打开后允许 ?src= 注入非 B 站源（仅本机自测）


def _kbps(v):
    v = str(v).strip().lower()
    m = re.match(r"^(\d+(?:\.\d+)?)\s*([km]?)", v)
    if not m:
        return 0
    n = float(m.group(1))
    if m.group(2) == "m":
        n *= 1000
    return int(n)


def _load1():
    try:
        return float(open("/proc/loadavg").read().split()[0])
    except Exception:
        return 0.0


def allowed(url):
    """仅放行 B 站 CDN（防 SSRF/开放代理）。"""
    try:
        p = urlparse(url)
    except Exception:
        return False
    if p.scheme not in ("http", "https"):
        return False
    host = (p.hostname or "").lower()
    if not host:
        return False
    return any(host.endswith(s) for s in (
        ".bilivideo.com", ".bilivideo.cn", ".hdslb.com", ".akamaized.net", ".bilibili.com"))


def _sanitize_cookie(ck):
    """只保留可打印 ASCII（防 header 注入；SESSDATA 本就是 URL 编码 ASCII）。"""
    return "".join(ch for ch in (ck or "") if 0x20 <= ord(ch) < 0x7F)[:600]


def _fetch_live_urls(room, cookie="", qn=QN_TRANS):
    """getRoomPlayInfo v2 → h264 线路 URL 列表（flv 优先，m3u8 末选）。
    cookie：笔端登录态 → 解锁高清源，并作为多用户限载的身份。"""
    h = {"User-Agent": UA, "Referer": "https://live.bilibili.com/"}
    if cookie:
        h["Cookie"] = cookie
    url = ("https://api.live.bilibili.com/xlive/web-room/v2/index/getRoomPlayInfo"
           "?room_id=%d&qn=%d&protocol=0,1&format=0,1,2&codec=0,1,2&platform=web&ptype=8"
           % (int(room), int(qn)))
    req = urllib.request.Request(url, headers=h)
    with urllib.request.urlopen(req, timeout=10) as r:
        j = json.loads(r.read().decode("utf8", "replace"))
    if j.get("code") != 0:
        raise RuntimeError("getRoomPlayInfo code=%s" % j.get("code"))
    playurl = (((j.get("data") or {}).get("playurl_info") or {}).get("playurl")) or {}
    flv, hls = [], []
    for stream in playurl.get("stream") or []:
        for fmt in stream.get("format") or []:
            for codec in fmt.get("codec") or []:
                # 只收 h264：HEVC(minihevc) 线路笔端解码启动实测失败
                if codec.get("codec_name") not in ("h264", "avc"):
                    continue
                base = codec.get("base_url") or ""
                for ui in codec.get("url_info") or []:
                    full = (ui.get("host") or "") + base + (ui.get("extra") or "")
                    if not full.startswith("https://") or full in flv or full in hls:
                        continue
                    if urlparse(full).path.endswith(".m3u8"):
                        hls.append(full)
                    else:
                        flv.append(full)
    urls = flv + hls
    if not urls:
        raise RuntimeError("v2 no h264 urls")
    return urls


# ---------------- 会话注册表（多用户隔离 / 限载 / 会话复用） ----------------
_sess_lock = threading.Lock()
_sessions = {}  # sid -> dict


def load_status():
    with _sess_lock:
        n = len(_sessions)
    cpus = os.cpu_count() or 4
    l1 = _load1()
    return {"sessions": n, "load1": round(l1, 2),
            "high": bool(n >= LOAD_HIGH_SESSIONS or l1 > cpus * 0.9)}


def _sid_of(room, ck, res, bv, trans, seg):
    """会话 id：同参数恒定 → 客户端重连/退出再进复用同一会话（秒开）。"""
    raw = "%d|%s|%d|%s|%d|%s" % (room, ck or "", res, bv, trans, seg)
    return hashlib.sha1(raw.encode("utf8")).hexdigest()[:12]


def _user_of(ck):
    return hashlib.sha1(ck.encode("utf8")).hexdigest()[:10] if ck else "anon"


def _stop_sessions(pred, why):
    """按条件停止会话（让位/回收）。"""
    with _sess_lock:
        hits = [s for s in _sessions.values() if not s["stop"].is_set() and pred(s)]
    for s in hits:
        s["stop"].set()
    if hits:
        sys.stderr.write("[hls] stop %d session(s): %s\n" % (len(hits), why))


def _stop_user_sessions(user, keep_sid=None):
    _stop_sessions(lambda s: s["user"] == user and s["sid"] != keep_sid, "user takeover")


# ---------------- ffmpeg 命令（HLS 输出） ----------------
def _hls_cmd(url, cookie, res, bv, trans, seg, out_dir, window_ms, src_lavfi=False):
    """单 ffmpeg：解码 → 缩放/定率 → x264 → HLS 分片（滑动窗口 + 幂等分片名）。

    关键取舍（全部来自 v4 真机实证，逐条保留原因）：
      · probesize 不能小（256KB/1s）：32KB 时约 1/3 概率探不到音频流 → 整段无声。
      · 视频码率必须 VBV 锁上限：x264 -b:v 只是平均目标，高运动画面实测超发 2 倍。
      · 音频**绝不能**加裸 -maxrate：无配套 -bufsize 时 ffmpeg 会静默丢弃整条音频流。
      · preset veryfast（非 ultrafast）：ultrafast 低码率块效应极重（"马赛克"）。
      · 不加 -skip_loop_filter：跳去块滤波会把源块效应带进重编码。
      · 不加 -muxdelay/-muxpreload 0：会让首个音频包迟到甚至整段缺失。
    """
    hdr = "Referer: https://live.bilibili.com/\r\n"
    if cookie:
        hdr += "Cookie: " + cookie + "\r\n"
    base = [FFMPEG, "-nostdin", "-hide_banner", "-loglevel", "error",
            "-fflags", "+nobuffer", "-flags", "+low_delay"]
    # HTTP-输入专属选项（探测窗口/超时/重连）——lavfi 自测输入不能带，否则会被当成未知输入选项
    http_in = [
        "-user_agent", UA,
        "-headers", hdr,
        "-probesize", "262144", "-analyzeduration", "1000000",
        "-rw_timeout", "10000000",
        # v5：上游断流时让 ffmpeg 自己重连（HLS 输出不怕 dts 断点，重连代价低）
        "-reconnect", "1", "-reconnect_streamed", "1", "-reconnect_delay_max", "5",
    ]
    if src_lavfi:
        # 仅本机自测（--dev-src-allow）：src=lavfi:<spec>[||<spec>...] → 每个 spec 一个 lavfi 输入，
        # 便于造"视频+音频"双轨（如 testsrc2||sine）。生产路径永远只有 -i <B站CDN url>。
        common = base
        in_args = []
        spec = url[len("lavfi:"):] if url.startswith("lavfi:") else url
        for part in spec.split("||"):
            # -re：按原生帧率读入。否则 lavfi 源跑得比实时快得多，分片飞速产出并滑出窗口，
            # 与真实直播（实时源）行为不一致，自测会误判成"分片 404"。
            in_args += ["-re", "-f", "lavfi", "-i", part]
    else:
        common = base + http_in
        in_args = ["-i", url]
    n_seg = max(3, min(24, int(round(window_ms / 1000.0 / SEG_S)) + 2))
    seg_type = "fmp4" if seg == "fmp4" else "mpegts"
    seg_ext = "m4s" if seg_type == "fmp4" else "ts"
    out = [
        "-f", "hls",
        "-hls_time", str(SEG_S),
        "-hls_init_time", "0.5",
        "-hls_list_size", str(n_seg),
        "-hls_segment_type", seg_type,
        "-hls_segment_filename", os.path.join(out_dir, "seg%05d." + seg_ext),
        "-hls_start_number_source", "epoch",   # 分片号唯一 → 重启后客户端不会命中旧缓存
        "-hls_allow_cache", "0",
        "-hls_flags", "delete_segments+omit_endlist+temp_file",
        os.path.join(out_dir, "index.m3u8"),
    ]
    if seg_type == "fmp4":
        out = out[:1] + ["-hls_fmp4_init_filename", "init.mp4"] + out[1:]
    if trans == 0:
        # 直通：零编码开销（源分辨率须在笔端软解预算内，调用方已判定）
        return common + ["-threads", "1"] + in_args + ["-c", "copy", "-bsf:a", "aac_adtstoasc"] + out
    return common + [
        "-threads", "3",
    ] + in_args + [
        "-vf", "scale=-2:%d,fps=%d" % (res, FPS),
        "-c:v", "libx264", "-preset", "veryfast", "-tune", "zerolatency",
        "-pix_fmt", "yuv420p", "-b:v", bv,
        "-maxrate", bv, "-bufsize", "%dk" % (_kbps(bv) * 2),
        # GOP 与分片对齐：fps 帧一个关键帧 + 关场景切换 → 每片时长稳定、可独立解码
        "-g", str(FPS), "-keyint_min", str(FPS), "-sc_threshold", "0",
        "-c:a", "aac", "-ar", "44100", "-b:a", TRANS_AUDIO,
    ] + out


def _spawn(cmd, log_path):
    log = open(log_path, "ab", buffering=0)
    try:
        p = subprocess.Popen(cmd, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=log)
    except Exception:
        log.close()
        raise
    return p, log


def _run_session(sess):
    """会话主循环：起 ffmpeg → 等退出 → 换新源地址重开（CDN 直链会过期）。
    HLS 输出天然容忍重开：播放列表被重写，客户端按需继续拉分片。"""
    d = sess["dir"]
    while not sess["stop"].is_set():
        url = sess["src"]
        if not url:
            try:
                cands = [u for u in _fetch_live_urls(sess["room"], sess["ck"], sess["qn"]) if allowed(u)]
                if not cands:
                    raise RuntimeError("no stream url")
                url = cands[0]
                sess["src"] = url
            except Exception as exc:
                sys.stderr.write("[hls] sid=%s playurl fail: %s\n" % (sess["sid"], str(exc)[:140]))
                if sess["stop"].wait(3.0):
                    break
                sess["restarts"] += 1
                if sess["restarts"] > MAX_RESTARTS:
                    sess["failed"] = True
                    break
                continue
        cmd = _hls_cmd(url, sess["ck"], sess["res"], sess["bv"], sess["trans"], sess["seg"],
                       d, sess["window_ms"], src_lavfi=sess.get("lavfi", False))
        try:
            proc, log = _spawn(cmd, os.path.join(d, "ffmpeg.log"))
        except Exception as exc:
            sys.stderr.write("[hls] sid=%s spawn fail: %s\n" % (sess["sid"], str(exc)[:140]))
            sess["failed"] = True
            break
        sess["proc"] = proc
        sys.stderr.write("[hls] sid=%s ffmpeg pid=%d room=%d res=%d bv=%s trans=%d seg=%s win=%dms\n"
                         % (sess["sid"], proc.pid, sess["room"], sess["res"], sess["bv"],
                            sess["trans"], sess["seg"], sess["window_ms"]))
        while proc.poll() is None and not sess["stop"].is_set():
            time.sleep(0.5)
        try:
            log.close()
        except Exception:
            pass
        if sess["stop"].is_set():
            try:
                proc.kill()
                proc.wait(timeout=5)
            except Exception:
                pass
            break
        # ffmpeg 自己退出：源站断流/换线/URL 过期 → 取新地址重开
        sess["restarts"] += 1
        sess["src"] = ""            # 强制重取（CDN 直链会过期）
        sys.stderr.write("[hls] sid=%s ffmpeg exit rc=%s → restart #%d\n"
                         % (sess["sid"], proc.returncode, sess["restarts"]))
        if sess["restarts"] > MAX_RESTARTS:
            sess["failed"] = True
            break
        if sess["stop"].wait(1.5):
            break
    sys.stderr.write("[hls] sid=%s session thread end (restarts=%d)\n" % (sess["sid"], sess["restarts"]))


def _teardown(sess):
    sess["stop"].set()
    p = sess.get("proc")
    if p is not None:
        try:
            p.kill()
            p.wait(timeout=5)
        except Exception:
            pass
    with _sess_lock:
        _sessions.pop(sess["sid"], None)
    try:
        shutil.rmtree(sess["dir"], ignore_errors=True)
    except Exception:
        pass


def ensure_session(room, ck, res, bv, trans, seg, window_ms, src=None, lavfi=False):
    """取（或建）会话；返回 sid。"""
    sid = _sid_of(room, ck, res, bv, trans, seg)
    with _sess_lock:
        s = _sessions.get(sid)
        if s is not None:
            s["last_hit"] = time.time()
            return sid, s
    if len(_sessions) >= MAX_SESSIONS:
        return None, None
    # 同一 cookie 换房间 → 旧会话让位（重连/切房间时旧连接常是半开的僵尸）
    user = _user_of(ck)
    _stop_user_sessions(user, keep_sid=sid)
    d = os.path.join(HLS_ROOT, sid)
    shutil.rmtree(d, ignore_errors=True)     # 清掉上次崩溃残留，保证新会话列表干净
    os.makedirs(d, exist_ok=True)
    sess = {
        "sid": sid, "user": user, "room": room, "ck": ck, "res": res, "bv": bv,
        "trans": trans, "seg": seg, "window_ms": window_ms,
        "qn": QN_TRANS if trans == 1 else QN_PASSTHRU,
        "dir": d, "stop": threading.Event(), "proc": None, "started": time.time(),
        "last_hit": time.time(), "restarts": 0, "failed": False,
        "src": src or "", "lavfi": lavfi,
    }
    with _sess_lock:
        _sessions[sid] = sess
    threading.Thread(target=_run_session, args=(sess,), daemon=True, name="hls-" + sid).start()
    return sid, sess


def _janitor():
    """回收空闲会话（客户端退出/切走后不再有列表与分片请求）。"""
    while True:
        time.sleep(5)
        now = time.time()
        with _sess_lock:
            idle = [s for s in _sessions.values() if now - s["last_hit"] > IDLE_STOP_S]
        for s in idle:
            sys.stderr.write("[hls] sid=%s idle %.0fs → teardown\n" % (s["sid"], now - s["last_hit"]))
            _teardown(s)


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):
        sys.stderr.write("[hls_proxy] " + (fmt % args) + "\n")

    def _code(self, code, ctype, body: bytes, extra=None):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Access-Control-Allow-Origin", "*")
        for k, v in (extra or {}).items():
            self.send_header(k, v)
        self.end_headers()
        self.wfile.write(body)

    def _redirect(self, loc):
        self.send_response(302)
        self.send_header("Location", loc)
        self.send_header("Content-Length", "0")
        self.send_header("Cache-Control", "no-store")
        self.end_headers()

    def do_POST(self):
        # 笔端探测走异步 native POST（jsapi.http 对非 B 站域不可达；native getBinary 是同步调用会
        # 阻塞 JS 线程 → ANR）→ /health 复用 POST 路由。body 忽略但须排干（keep-alive 正确性）。
        try:
            n = int(self.headers.get("Content-Length") or 0)
        except (TypeError, ValueError):
            n = 0
        if n > 0:
            try:
                self.rfile.read(n)
            except Exception:
                pass
        self.do_GET()

    def do_GET(self):
        try:
            parsed = urlparse(self.path)
            path = parsed.path

            # ---------------- /health：探测 + 重定向解析（host）+ 负载 ----------------
            if path == "/health":
                self._health(parsed)
                return

            # ---------------- /live：解析参数 → 建/复用会话 → 302 到播放列表 ----------------
            if path == "/live":
                self._live(parsed)
                return

            # ---------------- /hls/<sid>/index.m3u8 与分片 ----------------
            m = re.match(r"^/hls/([0-9a-f]{12})/(.+)$", path)
            if m:
                self._hls_file(m.group(1), m.group(2))
                return

            self._code(404, "text/plain", b"not found")
        except Exception as exc:
            sys.stderr.write("[hls_proxy] error: %s\n" % exc)
            try:
                self._code(500, "text/plain", b"internal error")
            except Exception:
                pass

    # ---------------- handlers ----------------
    def _health(self, parsed):
        cands, seen = [], set()
        port = self.server.server_address[1]
        try:  # 默认出口 IP（UDP connect 不实际发包）
            s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            s.connect(("223.5.5.5", 80))
            ip = s.getsockname()[0]
            s.close()
            if ip.startswith(("192.168.", "10.", "172.")) and ip not in seen:
                cands.append(ip)
                seen.add(ip)
        except Exception:
            pass
        try:
            for info in socket.getaddrinfo(socket.gethostname(), None, socket.AF_INET):
                ip = info[4][0]
                if ip.startswith(("192.168.", "10.", "172.")) and ip not in seen:
                    cands.append(ip)
                    seen.add(ip)
        except Exception:
            pass
        body = json.dumps({
            "ok": True,
            "version": VERSION,
            "host": self.headers.get("Host", ""),   # 笔端缓存为重定向解析结果
            "lan": ["http://%s:%d" % (ip, port) for ip in cands],
            "load": load_status(),
            "max_sessions": MAX_SESSIONS,
        }).encode("utf8")
        self._code(200, "application/json", body, {"Cache-Control": "no-store"})

    def _live(self, parsed):
        qs = parse_qs(parsed.query)
        src = None
        lavfi = False
        if DEV_SRC_ALLOW and (qs.get("src") or [""])[0]:
            src = (qs.get("src") or [""])[0]
            lavfi = src.startswith("lavfi:")
        try:
            room = int((qs.get("room") or ["0"])[0] or 0)
        except (TypeError, ValueError):
            room = 0
        if room <= 0 and not src:
            self._code(400, "application/json", json.dumps({"error": "missing room"}).encode())
            return
        ck = _sanitize_cookie((qs.get("ck") or [""])[0])
        trans = 0 if (qs.get("trans") or ["1"])[0] in ("0", "false", "off") else 1
        seg = "fmp4" if (qs.get("seg") or ["ts"])[0] == "fmp4" else "ts"
        bv = (qs.get("bv") or [TRANS_BV])[0].strip().lower() or TRANS_BV
        mm = re.match(r"^(\d+)([km]?)$", bv)
        if mm:
            bv = "%dk" % max(TRANS_BV_MIN_K, min(TRANS_BV_MAX_K,
                                              int(mm.group(1)) * (1000 if mm.group(2) == "m" else 1)
                                              // (1000 if mm.group(2) == "m" else 1)))
        else:
            bv = TRANS_BV
        try:
            res = int((qs.get("res") or [str(TRANS_H)])[0])
        except (TypeError, ValueError):
            res = TRANS_H
        if res not in TRANS_H_ALLOWED:
            res = TRANS_H
        try:
            window_ms = int((qs.get("buf") or [str(WINDOW_DEFAULT_MS)])[0])
        except (TypeError, ValueError):
            window_ms = WINDOW_DEFAULT_MS
        window_ms = max(WINDOW_MIN_MS, min(WINDOW_MAX_MS, window_ms))

        sid, sess = ensure_session(room, ck, res, bv, trans, seg, window_ms, src=src, lavfi=lavfi)
        if sid is None:
            sys.stderr.write("[hls] 503 busy sessions=%d\n" % len(_sessions))
            self._code(503, "application/json",
                       json.dumps({"error": "server busy", "load": load_status()}).encode())
            return
        host = self.headers.get("Host") or ("%s:%d" % (self.server.server_address[0],
                                                      self.server.server_address[1]))
        loc = "http://%s/hls/%s/index.m3u8" % (host, sid)
        sys.stderr.write("[hls] /live room=%d res=%d bv=%s trans=%d seg=%s win=%dms ck=%s sid=%s load=%s\n"
                         % (room, res, bv, trans, seg, window_ms,
                            ("yes" if ck else "no"), sid, json.dumps(load_status())))
        self._redirect(loc)

    def _hls_file(self, sid, name):
        if ".." in name or name.startswith("/"):
            self._code(400, "text/plain", b"bad name")
            return
        with _sess_lock:
            sess = _sessions.get(sid)
        if sess is None:
            # 会话已回收/过期：让客户端重走 /live（重定向链会重新建会话）
            self._code(404, "text/plain", b"session gone; reopen /live")
            return
        sess["last_hit"] = time.time()
        if name.endswith(".m3u8"):
            if not re.match(r"^[A-Za-z0-9_.-]+\.m3u8$", name):
                self._code(400, "text/plain", b"bad playlist name")
                return
            path = os.path.join(sess["dir"], "index.m3u8")
            t0 = time.time()
            while not os.path.exists(path):
                if sess["failed"] or sess["stop"].is_set():
                    self._code(503, "application/json",
                               json.dumps({"error": "stream failed", "room": sess["room"]}).encode())
                    return
                if time.time() - t0 > PLAYLIST_WAIT_S:
                    self._code(504, "application/json", b'{"error":"playlist timeout"}')
                    return
                time.sleep(0.2)
            try:
                with open(path, "rb") as f:
                    body = f.read()
            except Exception:
                self._code(503, "text/plain", b"playlist unreadable")
                return
            self._code(200, "application/vnd.apple.mpegurl", body,
                       {"Cache-Control": "no-store", "X-Accel-Buffering": "no"})
            return
        # 分片（ts / m4s / init.mp4）
        if not re.match(r"^([A-Za-z0-9_.-]+\.(ts|m4s)|init\.mp4)$", name):
            self._code(400, "text/plain", b"bad segment name")
            return
        path = os.path.join(sess["dir"], name)
        if not os.path.exists(path):
            # 已被滑动窗口删除 / 刚被重开覆盖：让客户端按播放列表重试（不要断连）
            self._code(404, "text/plain", b"segment gone")
            return
        try:
            with open(path, "rb") as f:
                body = f.read()
        except Exception:
            self._code(404, "text/plain", b"segment unreadable")
            return
        ctype = "video/mp2t" if name.endswith(".ts") else "video/mp4"
        self._code(200, ctype, body, {"Cache-Control": "no-store"})
        sys.stderr.write("[hls] seg sid=%s %s %dB\n" % (sid, name, len(body)))


def _src_of_lavfi(spec):
    return spec


def main():
    global DEV_SRC_ALLOW
    ap = argparse.ArgumentParser(description="PenBili live transcode server (v5 / HLS)")
    ap.add_argument("--host", default="0.0.0.0")
    ap.add_argument("--port", type=int, default=2050)
    ap.add_argument("--max-sessions", type=int, default=MAX_SESSIONS)
    ap.add_argument("--dev-src-allow", action="store_true",
                    help="允许 /live?src= 注入非 B 站源（仅本机自测；默认关闭）")
    args = ap.parse_args()
    if not shutil.which("ffmpeg"):
        print("[hls_proxy] FATAL: ffmpeg not found in PATH", file=sys.stderr)
        sys.exit(1)
    globals()["MAX_SESSIONS"] = args.max_sessions
    DEV_SRC_ALLOW = bool(args.dev_src_allow)
    os.makedirs(HLS_ROOT, exist_ok=True)
    threading.Thread(target=_janitor, daemon=True, name="hls-janitor").start()
    srv = ThreadingHTTPServer((args.host, args.port), Handler)
    print("[hls_proxy] v%s listening on http://%s:%d (ffmpeg=%s, max_sessions=%d, hls_root=%s)"
          % (VERSION, args.host, args.port, FFMPEG, MAX_SESSIONS, HLS_ROOT))
    print("[hls_proxy] 笔端「直播设置」填写服务器地址: http://<本机IP>:%d" % args.port)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        print("\n[hls_proxy] bye")


if __name__ == "__main__":
    main()
