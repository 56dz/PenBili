#!/usr/bin/env python3
"""PenBili 直播服务器转码代理（server-side transcode for live, 720p → 360p）

背景：有道词典笔 X5（A53×4）软解 720p/2500kbps 直播不实时（profile 基准 0.76x@720p），
      播放卡顿。本服务在局域网机器上把直播流转码到 360p/低码率后回供，笔端软解压力大降。

用法：
    python3 tools/live_proxy.py            # 默认端口 2050（转码+弹幕同端口）
    # 需要本机已安装 ffmpeg（PATH 中可执行）

PenBili 侧配合（搜索页 → 直播设置）：
    1) 打开「服务器转码」开关
    2) 填服务器地址：http://<本机IP或域名>:2050（支持 302 重定向入口，如 tools/redirector.js）
    3) 之后点开直播间 → 自动请求  GET /live?u=<B站flv直链>
    4) 未开启/未配置时，直播播放页左下角会提示"未开启服务器转码，播放可能卡顿"
       且弹幕按钮显示"请先开启服务器转码"（直播弹幕也走本服务）

协议（极简）：
    GET /live?u=<url>           → 200 video/x-flv（chunked，ffmpeg stdout 直通）
    GET /danmaku?room=<id>&since=<id>
                                 → 200 JSON {ok,cursor,msgs:[{id,text}]}（直播弹幕代理，
                                    笔端无 WebSocket → HTTP 轮询本服务，服务端代连 B 站 wss 流）
    GET /health                  → 200 ok（探测）
    参数缺失 400 / 白名单外 403 / ffmpeg 不可用 500

安全：
    - 仅允许 B 站 CDN 域名（防 SSRF / 开放代理滥用）
    - 仅监听 --host（默认 0.0.0.0 便于局域网访问；家用可改 127.0.0.1）

转码参数（面向低延迟+低码率；A53 360p 软解实测流畅）：
    -i <flv>  -vf scale=-2:360  -c:v libx264 -preset ultrafast -tune zerolatency
    -b:v 700k -g 30  -c:a aac -b:a 64k  -f flv  pipe:1
"""

import argparse
import base64
import hashlib
import http.cookiejar
import json
import os
import shutil
import socket
import struct
import subprocess
import sys
import threading
import time
import urllib.request
import zlib
from urllib.parse import urlparse, parse_qs, urlencode
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
# PIL 模块级导入（FramePipeline.render 用裸名 Image/ImageDraw——此前只在 __init__ 局部导入
# → render NameError → 每帧异常→疯狂换线→零输出，2026-09-26 实测）
from PIL import Image, ImageDraw, ImageFont

# B 站 CDN 白名单（子域后缀匹配；防 SSRF/开放代理）
ALLOWED_HOST_SUFFIXES = (
    ".bilivideo.com",
    ".bilivideo.cn",
    ".hdslb.com",
    ".akamaized.net",
    ".bilibili.com",
)

FFMPEG = shutil.which("ffmpeg") or "ffmpeg"

# 360p 转码 + 可选弹幕烧帧（服务端融合）：lanes 由笔端档位映射（1/2/4），textfile reload 热更
_FONT_CJK = "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc"
_DM_SPD = [110, 142, 90, 166]  # 泳道速度 px/s——与笔端 player.c drawtext 镜像
_DM_Y = [8, 72, 136, 200]      # 360p 高度内四泳道
# 归一化：零宽/变体选择符/方向控制符 与 连续空白（"视觉同文案"去重 key 用）
import re as _re
_DM_KEY_RE = _re.compile("[​-‍⁠﻿︎️]")
_DM_SPACE_RE = _re.compile(r"\s+")


def _vf_chain(room, lanes):
    base = "scale=-2:360,fps=30"
    if lanes <= 0 or not room:
        return base
    parts = [base]
    for i in range(min(lanes, 4)):
        p = "/tmp/srv_dm_%d_%d.txt" % (room, i)
        # 逗号需 \, 转义（filtergraph 语法）；公式与笔端一致
        parts.append(
            "drawtext=fontfile=%s:textfile=%s:reload=1:"
            "x=w-mod(t*%d\\,w+text_w):y=%d:fontsize=20:fontcolor=white"
            % (_FONT_CJK, p, _DM_SPD[i], _DM_Y[i]))
    return ",".join(parts)


def _text_px(txt, fs=20):
    """text_w 按码点分级估宽（px）。ASCII 0.55fs · emoji 2fs · CJK/全角 fs · 其余 0.6fs。"""
    w = 0
    for ch in txt:
        o = ord(ch)
        if o < 0x80:
            w += fs * 0.55
        elif (0x1F300 <= o <= 0x1FAFF) or (0x2600 <= o <= 0x27BF) or (0x2B00 <= o <= 0x2BFF) \
                or o == 0xFE0F or (0x1F000 <= o <= 0x1F2FF):
            w += fs * 2.0
        elif o >= 0x2E80 or (0x3000 <= o <= 0x303F) or (0xFF00 <= o <= 0xFFEF):
            w += fs
        else:
            w += fs * 0.6
    return int(w)


def _period_seconds(txt, out_w, fs=20):
    """展示期上限 T=(out_w+text_w)/SPD 的像素距离（fallback 用；主路径见相位补偿）。"""
    return int((out_w + _text_px(txt, fs)) * 0.90)


def _probe_out_width(url):
    """ffprobe 源分辨率 → scale=-2:360 的真实输出宽（偶数）。拿不到回退 640。
    —— period 必须用真宽：固定 640 对 480 宽源高估 33% → 到期清过晚 → mod 回卷多滚一遍
    （"直播弹幕重复两遍"根因，2026-09-26）。"""
    try:
        p = subprocess.run(
            [FFMPEG.replace("ffmpeg", "ffprobe") if FFMPEG.endswith("ffmpeg") else "ffprobe",
             "-v", "error", "-select_streams", "v:0",
             "-show_entries", "stream=width,height", "-of", "csv=p=0", url],
            stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, timeout=6)
        line = p.stdout.read().decode("ascii", "replace").strip().splitlines()
        if line and "," in line[0]:
            w, h = line[0].split(",")[:2]
            w, h = int(float(w)), int(float(h))
            if w > 0 and h > 0:
                out = int(round(360.0 * w / h / 2.0)) * 2
                if out >= 160:
                    return out
    except Exception:
        pass
    return 640


def build_ffmpeg_args(url, room=0, lanes=0, out_w=640):
    return [
        FFMPEG, "-hide_banner", "-loglevel", "error",
        "-user_agent", "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/124.0.0.0 Safari/537.36",
        "-headers", "Referer: https://live.bilibili.com/\r\n",
        "-probesize", "32", "-analyzeduration", "0",
        "-fflags", "+nobuffer", "-flags", "+low_delay",
        "-i", url,
        "-vf", _vf_chain(room, lanes),
        "-c:v", "libx264", "-preset", "ultrafast", "-tune", "zerolatency",
        "-b:v", "700k", "-g", "30", "-keyint_min", "30",
        "-c:a", "aac", "-b:a", "64k",
        "-muxdelay", "0", "-muxpreload", "0",
        "-f", "flv", "pipe:1",
    ]


def allowed(url: str) -> bool:
    """仅放行 B 站直播 CDN 直链（http/https）。"""
    try:
        p = urlparse(url)
    except Exception:
        return False
    if p.scheme not in ("http", "https"):
        return False
    host = (p.hostname or "").lower()
    if not host:
        return False
    return any(host.endswith(sfx) for sfx in ALLOWED_HOST_SUFFIXES)


# ================================================================
# 直播弹幕代理（方案B：笔端无 WebSocket → HTTP 轮询 /danmaku，服务端代连 B 站信息流）
# 零依赖（服务器无 pip）：明文 WS（ws://host:ws_port/sub 免 TLS）+ protover=2 zlib + JSON。
# 协议依据 collect docs/live/message_stream.md（2026-09 权威版）：
#   getDanmuInfo(wbi签名+buvid3) → token/host_list → 认证包 op=7 → 心跳 op=2/30s
#   → 收包 op=5 ver=2 zlib → 16B头+JSON → cmd==DANMU_MSG → info[1]=文本 info[0][1]=mode
# ================================================================
UA = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/124.0.0.0 Safari/537.36"
_WBI_ORDER = [46, 47, 18, 2, 53, 8, 23, 32, 15, 50, 10, 31, 58, 3, 45, 35, 27, 43, 5, 49,
              33, 9, 42, 19, 29, 28, 14, 39, 12, 38, 41, 13, 37, 48, 7, 16, 24, 55, 40, 61,
              26, 17, 0, 1, 60, 51, 30, 4, 22, 25, 54, 21, 56, 59, 6, 63, 57, 62, 11, 36, 20, 34, 44, 52]
_wbi_cache = {"img": "", "sub": "", "at": 0.0}
_wbi_lock = threading.Lock()
_rooms_lock = threading.Lock()
_rooms = {}
_burners = {}          # room → DanmakuBurner（单例：重开直播防新旧双 burner 抢写同 textfile）
_burners_lock = threading.Lock()


def _get_burner(room, lanes, out_w):
    """同房单例：新 /live 进入（重开/重连）先关旧 burner，杜绝双写造成的"多一次"。"""
    with _burners_lock:
        old = _burners.get(room)
        if old is not None and old.is_alive():
            old.shutdown()
        b = DanmakuBurner(room, lanes, out_w)
        _burners[room] = b
        return b


def _wbi_keys():
    with _wbi_lock:
        if _wbi_cache["img"] and time.time() - _wbi_cache["at"] < 300:
            return _wbi_cache["img"], _wbi_cache["sub"]
    req = urllib.request.Request("https://api.bilibili.com/x/web-interface/nav",
                                 headers={"User-Agent": UA, "Referer": "https://live.bilibili.com/"})
    with urllib.request.urlopen(req, timeout=10) as r:
        j = json.loads(r.read().decode("utf8", "replace"))
    data = j.get("data") or {}
    ik = (data.get("wbi_img", {}).get("img_url", "") or "").rsplit("/", 1)[-1].split(".")[0]
    sk = (data.get("wbi_img", {}).get("sub_url", "") or "").rsplit("/", 1)[-1].split(".")[0]
    if not ik or not sk:
        raise RuntimeError("wbi keys empty")
    with _wbi_lock:
        _wbi_cache.update(img=ik, sub=sk, at=time.time())
    return ik, sk


def _wbi_sign(params):
    ik, sk = _wbi_keys()
    nav = "".join((ik + sk)[i] for i in _WBI_ORDER)
    params["wts"] = str(int(time.time()))
    q = urlencode(sorted(params.items()))
    params["w_rid"] = hashlib.md5((q + nav).encode()).hexdigest()
    return params


def _danmu_info(room):
    """弹幕流配置（老接口 getConf：零 wbi 签名 → 无 -352 风控；实测 code=0 可用）。
    登录态：读同目录 session.json（{cookie: "..."} 或 {SESSDATA,bili_jct,DedeUserID}），
    无文件则匿名（实测均能 auth，登录态弹幕文本更完整）。
    返回 (token, host_list)；host 字段 port(2243 raw)/ws_port(2244)/wss_port。"""
    cj = http.cookiejar.CookieJar()
    op = urllib.request.build_opener(urllib.request.HTTPCookieProcessor(cj))
    h = {"User-Agent": UA, "Referer": "https://live.bilibili.com/"}
    try:
        op.open(urllib.request.Request("https://www.bilibili.com/", headers=h), timeout=8).read(64)
    except Exception:
        pass
    # 登录 cookie（session.json 与本文件同目录；不入库）；uid 必须与 token 来源一致，
    # 否则服务端秒踢（实测：登录 token + uid=0 → raw closed 循环）
    uid = 0
    try:
        sj = json.load(open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "session.json")))
        if sj.get("cookie"):
            h["Cookie"] = sj["cookie"]
        elif sj.get("SESSDATA"):
            h["Cookie"] = ("SESSDATA=%s; bili_jct=%s; DedeUserID=%s" %
                           (sj.get("SESSDATA", ""), sj.get("bili_jct", ""), sj.get("DedeUserID", "")))
        uid = int(sj.get("DedeUserID") or sj.get("uid") or 0)
    except Exception:
        pass
    url = ("https://api.live.bilibili.com/room/v1/Danmu/getConf?room_id=%d"
           "&platform=pc&player=web&qn=10000" % int(room))
    j = json.loads(op.open(urllib.request.Request(url, headers=h), timeout=10).read().decode("utf8", "replace"))
    d = j.get("data") or {}
    token = d.get("token") or ""
    hosts = d.get("host_server_list") or d.get("server_list") or []
    if not token or not hosts:
        raise RuntimeError("getConf code=%s" % j.get("code"))
    return token, hosts, uid


def _bili_pack(op, body):
    if not isinstance(body, bytes):
        body = body.encode()
    return struct.pack(">I2H2I", 16 + len(body), 16, 0, op, 1) + body


def _bili_unwrap(buf):
    """连续 16B 头包 → [(ver, op, body)] + 残包。"""
    out = []
    while len(buf) >= 16:
        total, hl, ver, op, seq = struct.unpack(">I2H2I", buf[:16])
        if total < 16 or total > 16 * 1024 * 1024 or len(buf) < total:
            break
        out.append((ver, op, buf[16:total]))
        buf = buf[total:]
    return out, buf


def _ws_open(host, port, path, timeout=10):
    s = socket.create_connection((host, port), timeout)
    key = base64.b64encode(os.urandom(16)).decode()
    req = ("GET %s HTTP/1.1\r\nHost: %s:%d\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
           "Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n\r\n" % (path, host, port, key))
    s.sendall(req.encode())
    buf = b""
    while b"\r\n\r\n" not in buf:
        chunk = s.recv(4096)
        if not chunk:
            raise IOError("handshake closed")
        buf += chunk
        if len(buf) > 16384:
            raise IOError("handshake too large")
    if b" 101" not in buf.split(b"\r\n", 1)[0]:
        raise IOError("bad handshake status")
    s.settimeout(35)
    return s


def _ws_send(s, payload, opcode=2):
    if not isinstance(payload, bytes):
        payload = payload.encode()
    mask = os.urandom(4)
    hdr = bytearray([0x80 | (opcode & 0x0F)])
    n = len(payload)
    if n < 126:
        hdr.append(0x80 | n)
    elif n < 65536:
        hdr.append(0x80 | 126)
        hdr += struct.pack(">H", n)
    else:
        hdr.append(0x80 | 127)
        hdr += struct.pack(">Q", n)
    hdr += mask
    masked = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
    s.sendall(bytes(hdr) + masked)


def _ws_recv(s):
    """收完整一帧（含分片合并、ping/pong 掩码处理）→ (opcode, payload)。"""
    def rd(n):
        chunks = []
        left = n
        while left > 0:
            c = s.recv(left)
            if not c:
                raise IOError("ws closed")
            chunks.append(c)
            left -= len(c)
        return b"".join(chunks)

    def unmask(piece, b2):
        if b2 & 0x80:
            mk = rd(4)
            return bytes(b ^ mk[i % 4] for i, b in enumerate(piece))
        return piece

    b1, b2 = rd(2)
    fin = (b1 & 0x80) != 0
    opcode = b1 & 0x0F
    ln = b2 & 0x7F
    if ln == 126:
        ln = struct.unpack(">H", rd(2))[0]
    elif ln == 127:
        ln = struct.unpack(">Q", rd(8))[0]
    data = unmask(rd(ln), b2)
    while not fin:
        b1, b2 = rd(2)
        if (b1 & 0x0F) != 0:
            raise IOError("bad continuation")
        ln = b2 & 0x7F
        if ln == 126:
            ln = struct.unpack(">H", rd(2))[0]
        elif ln == 127:
            ln = struct.unpack(">Q", rd(8))[0]
        data += unmask(rd(ln), b2)
        fin = (b1 & 0x80) != 0
    return opcode, data


class RoomFeed:
    """单房间弹幕缓存：后台 WS 线程收 DANMU_MSG；seq 自增（跨重建保留游标）。"""

    def __init__(self, room, msgs=None, seq=0):
        self.room = int(room)
        self.lock = threading.Lock()
        self.msgs = list(msgs) if msgs else []
        self.seq = int(seq)
        self.last_access = time.time()
        self.thread = threading.Thread(target=self._run, daemon=True)
        self.thread.start()

    def push(self, text, color=None):
        text = (text or "").strip()
        if not text:
            return
        with self.lock:
            self.seq += 1
            self.msgs.append({"id": self.seq, "text": text[:80],
                              "color": int(color) if color else 16777215})
            if len(self.msgs) > 400:
                del self.msgs[: len(self.msgs) - 400]

    def since(self, sid):
        self.last_access = time.time()
        with self.lock:
            out = [m for m in self.msgs if m["id"] > sid]
            # code+data 双包装：笔端 normalizeJson 按 code 判业务错、按 parsed.data 取数据
            # （缺 data → r.data=undefined → 上一轮"live dm bad stage=undefined code=0"根因）
            return {"code": 0, "data": {"ok": True, "cursor": self.seq, "msgs": out[:30]}}

    def _run(self):
        while time.time() - self.last_access < 60:
            try:
                token, hosts, uid = _danmu_info(self.room)
                auth = json.dumps({"uid": uid, "roomid": self.room, "protover": 2,
                                   "platform": "web", "type": 2, "key": token})
                # 连接优先 raw TCP 2243（实证通道：35万在线房 22s 收 22 条 DANMU_MSG）；
                # 无 port 才回退明文 ws（HTTP Upgrade + WS 帧）
                raw_host = None
                for h in hosts:
                    if h.get("port"):
                        raw_host = h
                        break
                if raw_host:
                    sock = socket.create_connection((raw_host["host"], int(raw_host["port"])), 10)
                    is_ws = False
                    send = lambda b: sock.sendall(b)             # noqa: E731
                else:
                    ws_host = next((h for h in hosts if h.get("ws_port")), hosts[0])
                    sock = _ws_open(ws_host["host"], int(ws_host.get("ws_port") or 443), "/sub")
                    is_ws = True
                    send = lambda b: _ws_send(sock, b)          # noqa: E731
                sock.settimeout(35)
                send(_bili_pack(7, auth))
                last_hb = time.time()
                pending = b""
                while time.time() - self.last_access < 60:
                    try:
                        if is_ws:
                            op, payload = _ws_recv(sock)
                            chunk = payload if op == 2 else b""
                        else:
                            d = sock.recv(65536)
                            if not d:
                                raise IOError("raw closed")
                            op, chunk = 2, d
                    except socket.timeout:
                        op, chunk = None, b""
                    now = time.time()
                    if now - last_hb > 28:
                        send(_bili_pack(2, "[object Object]"))
                        last_hb = now
                    if op is None:
                        continue
                    if is_ws and op == 9:
                        _ws_send(sock, payload, opcode=10)
                        continue
                    if op != 2 or not chunk:
                        continue
                    pending += chunk
                    pkts, pending = _bili_unwrap(pending)
                    for ver, bop, body in pkts:
                        if ver == 2:
                            try:
                                raw = zlib.decompress(body)
                            except Exception:
                                continue
                            inner, _ = _bili_unwrap(raw)
                            for iv, iop, ibody in inner:
                                self._handle(iop, ibody)
                        else:
                            self._handle(bop, body)
                try:
                    sock.close()
                except Exception:
                    pass
            except Exception as exc:
                sys.stderr.write("[live_proxy] room %s feed: %s\n" % (self.room, exc))
                time.sleep(5)

    def _handle(self, op, body):
        if op != 5 or not body:
            return
        try:
            j = json.loads(body.decode("utf8", "replace"))
        except Exception:
            return
        if j.get("cmd") == "DANMU_MSG":
            info = j.get("info") or []
            text = info[1] if len(info) > 1 else ""
            mode = 0
            color = 16777215
            if info and isinstance(info[0], list):
                if len(info[0]) > 1:
                    mode = info[0][1]
                if len(info[0]) > 3 and isinstance(info[0][3], int):
                    color = info[0][3]
            if text and mode in (1, 2, 3, 4, 5):
                self.push(str(text), color)


def _fetch_live_urls(room):
    """getRoomPlayInfo v2（BiliClient 参考实现）→ 多线路 URL 列表（url_info: host+base_url+extra）。
    /live 断线时轮换线路重拉；失败抛异常由调用方回退（首条 u= 仍由笔端提供）。"""
    url = ("https://api.live.bilibili.com/xlive/web-room/v2/index/getRoomPlayInfo"
           "?room_id=%d&qn=10000&protocol=0,1&format=0,1,2&codec=0,1,2&platform=web&ptype=8" % int(room))
    req = urllib.request.Request(url, headers={"User-Agent": UA, "Referer": "https://live.bilibili.com/"})
    with urllib.request.urlopen(req, timeout=10) as r:
        j = json.loads(r.read().decode("utf8", "replace"))
    if j.get("code") != 0:
        raise RuntimeError("getRoomPlayInfo code=%s" % j.get("code"))
    playurl = (((j.get("data") or {}).get("playurl_info") or {}).get("playurl")) or {}
    urls = []
    for stream in playurl.get("stream") or []:
        for fmt in stream.get("format") or []:
            for codec in fmt.get("codec") or []:
                # 只收 h264：HEVC(minihevc) 线路启动实测失败（no decoder/filtergraph 连锁错误）
                if codec.get("codec_name") not in ("h264", "avc"):
                    continue
                base = codec.get("base_url") or ""
                for ui in codec.get("url_info") or []:
                    full = (ui.get("host") or "") + base + (ui.get("extra") or "")
                    if full.startswith("https://") and full not in urls:
                        urls.append(full)
    if not urls:
        raise RuntimeError("v2 no urls")
    return urls


def _room_feed(room):
    with _rooms_lock:
        f = _rooms.get(room)
        if f is None:
            f = RoomFeed(room)
        elif not f.thread.is_alive():
            if time.time() - f.last_access > 60:
                f = RoomFeed(room)
            else:  # 保留 seq/msg 游标重连
                f = RoomFeed(room, msgs=f.msgs, seq=f.seq)
        _rooms[room] = f
        f.last_access = time.time()
        return f


# ================================================================
# 帧级弹幕合成（2026-09-26 重写——drawtext 周期模型的三个观感问题在模型内不可能发生）：
#   上游 ffmpeg 解码出 rgb24 原始帧 → PIL 按【帧对象生命周期】画弹幕 → 下游 ffmpeg 编码回 flv
#   · 每条弹幕：start_frame 固定（下一帧从 x=W 右边缘进）→ x=W-(n-start)*SPD/30 → 出屏即弃
#   · 无 mod 周期 → 无回卷重复；位置逐帧精确 → 无"中间刷新/中间消失"；font.getlength 精确宽
#   · 性能实测（N5100）：2 条中文弹幕 1.5ms/帧（预算 33.3ms）
# 音频：上游 -c:a copy 进 aac FIFO → 下游 mux（不经过 PIL）；音频轨直播必有（上游失败=换线可见）
# ================================================================
def _audio_bridge(fifo_a, fifo_b, stop_evt):
    """音频中继桥（换线不断流的核心）：
    上游写 fifo_a → 本桥读出 → 写 fifo_b → 下游读 fifo_b。
    **桥全程持有 fifo_b 写端**：上游断（a EOF）时只关 a 重开等下一条上游，b 永不关闭
    → 下游音频输入永不见 EOF → 下游 ffmpeg 不退出 → 笔端 ffmpeg audio_alive 不死
    → 换线窗口从"音频死亡+8s恢复"降为"音频空隙1~3s（环余量吸收）+视频冻结1~3s"。"""
    fa = None
    try:
        fb = open(fifo_b, "wb", buffering=0)
    except Exception as exc:
        sys.stderr.write("[live_proxy] bridge open b: %s\n" % exc)
        return
    try:
        while not stop_evt.is_set():
            if fa is None:
                try:
                    fa = open(fifo_a, "rb")  # 阻塞等上游写端配对
                except Exception:
                    if stop_evt.is_set():
                        break
                    time.sleep(0.2)
                    continue
            try:
                d = fa.read(4096)
            except Exception:
                d = b""
            if not d:
                try:
                    fa.close()
                except Exception:
                    pass
                fa = None        # 上游断：重开 a 等新上游；b 保持打开（下游音频静默但不 EOF）
                time.sleep(0.15)
                continue
            try:
                fb.write(d)
            except Exception:
                break             # 下游死
    finally:
        if fa is not None:
            try:
                fa.close()
            except Exception:
                pass
        try:
            fb.close()            # 仅最终收尾才关 b（→ 下游音频 EOF → 下游退出）
        except Exception:
            pass


class FramePipeline:
    _FPS = 30
    _SPD = [110, 142, 90, 166]
    _Y = [8, 72, 136, 200]

    def __init__(self, room, lanes, out_w):
        from PIL import Image, ImageDraw, ImageFont
        self.Image = Image
        self.ImageDraw = ImageDraw
        self.room = int(room)
        self.lanes = max(0, min(4, int(lanes)))
        self.W = int(out_w) or 640
        self.H = 360
        self.font = ImageFont.truetype(_FONT_CJK, 20)
        self.state = [None] * 4          # lane → {text, start, w, color}（出屏即 None）
        self.pending = []                # 已去重、待上屏
        self.seen = {}                   # 场次去重（归一化+NFKC key）
        self.cursor = 0                  # RoomFeed 消费游标
        self.frame_n = 0
        self._next_lane = 0
        self._last_refill = 0.0

    # ---- 数据面：RoomFeed → pending（场次去重/归一化/宽度精确测量） ----
    def refill(self):
        if self.room <= 0:
            return
        feed = _room_feed(self.room)
        with feed.lock:
            fresh = [m for m in feed.msgs if m["id"] > self.cursor]
            if feed.msgs:
                self.cursor = max(self.cursor, feed.msgs[-1]["id"])
        now = time.time()
        for m in fresh:
            txt = (m.get("text") or "").strip()[:40]
            if not txt:
                continue
            key = _DM_KEY_RE.sub("", txt)
            key = _DM_SPACE_RE.sub(" ", key).strip()
            try:
                key = key.normalize("NFKC")
            except Exception:
                pass
            if key in self.seen:
                continue
            if len(self.pending) >= 16:
                break
            self.seen[key] = now
            w = self.font.getlength(txt)  # 精确文本宽（估宽问题在帧模型中消失）
            self.pending.append({"text": txt, "w": w,
                                 "color": int(m.get("color") or 16777215)})
        # seen 护栏：>5000 只淘汰 30min 前的
        if len(self.seen) > 5000:
            for k, at in list(self.seen.items()):
                if now - at > 1800:
                    self.seen.pop(k, None)
            if len(self.seen) > 6000:
                for k in list(self.seen)[:600]:
                    self.seen.pop(k, None)

    # ---- 渲染面：一帧进一帧出（对象生命周期；帧号驱动无全局时钟依赖） ----
    def render(self, rgb):
        img = Image.frombytes("RGB", (self.W, self.H), rgb)
        d = ImageDraw.Draw(img)
        n = self.frame_n
        lim = min(max(self.lanes, 1), 4)
        for i in range(lim):
            st = self.state[i]
            if st is None:
                continue
            x = self.W - int((n - st["start"]) * self._SPD[i] / float(self._FPS))
            if x < -st["w"]:
                self.state[i] = None  # 出屏即弃（一次性生命周期，永不回卷）
                continue
            c = st["color"]
            d.text((x, self._Y[i]), st["text"], font=self.font,
                   fill=((c >> 16) & 255, (c >> 8) & 255, c & 255),
                   stroke_width=1, stroke_fill=(0, 0, 0))  # 1px 黑边：纯色背景可读（与笔端 drawtext borderw=1 对齐）
        # 空闲泳道补 pending（start=下一帧 → x=W 右边缘整齐入屏）
        if self.pending and self.lanes > 0:
            tried = 0
            while self.pending and tried < lim:
                if self.state[self._next_lane] is None:
                    st = self.pending.pop(0)
                    st["start"] = n + 1
                    self.state[self._next_lane] = st
                    self._next_lane = (self._next_lane + 1) % lim
                else:
                    self._next_lane = (self._next_lane + 1) % lim
                tried += 1
        # 数据补充节流（每 15 帧 ≈0.5s 查一次 RoomFeed）
        if n % 15 == 0:
            self.refill()
        self.frame_n += 1
        return img.tobytes()


class DanmakuBurner(threading.Thread):
    """服务端融合烧帧写线程：RoomFeed 取新弹幕 → 泳道分配（忙闲估算 + 15s 文案冷却）
    → 写 textfile（ffmpeg drawtext reload=1 热更）。跟随 /live 连接生命周期。"""

    def __init__(self, room, lanes, out_w=640):
        super().__init__(daemon=True)
        self.room = int(room)
        self.lanes = max(1, min(4, int(lanes)))
        self.out_w = int(out_w) or 640  # scale 后真实输出宽（period 用，防估错回卷）
        self.paths = ["/tmp/srv_dm_%d_%d.txt" % (self.room, i) for i in range(self.lanes)]
        self._stop = False
        self._cursor = 0
        self._lane_free = [0.0] * self.lanes
        self._text_at = {}
        self.media_t0 = None  # 流媒体时间基准（handler 在首个 chunk 输出时 set_t0）
        for p in self.paths:
            try:
                open(p, "w").write(" ")
            except Exception:
                pass
        self.start()

    def set_t0(self, when):
        if self.media_t0 is None:
            self.media_t0 = when

    def run(self):
        while not self._stop:
            time.sleep(0.4)
            try:
                feed = _room_feed(self.room)
                with feed.lock:
                    fresh = [m for m in feed.msgs if m["id"] > self._cursor]
                    if feed.msgs:
                        self._cursor = max(self._cursor, feed.msgs[-1]["id"])
                now = time.time()
                # 到期泳道清除：drawtext 的 mod 回卷是无限循环，不写空则同一条永久反复入屏
                # （"很多重复弹幕"的服务端主因）
                for i in range(self.lanes):
                    if self._lane_free[i] and now >= self._lane_free[i]:
                        try:
                            open(self.paths[i], "w").write(" ")
                        except Exception:
                            pass
                        self._lane_free[i] = 0.0
                for m in fresh:
                    txt = (m.get("text") or "").strip()[:40]
                    if not txt:
                        continue
                    # 归一化 key：零宽/连续空白 + NFKC（全角→半角/兼容字形统一）——
                    # "视觉同文案"字符串不等的漏网；显示仍用原文
                    key = _DM_KEY_RE.sub("", txt)
                    key = _DM_SPACE_RE.sub(" ", key).strip()
                    try:
                        key = key.normalize("NFKC")
                    except Exception:
                        pass
                    # 场次级同文案仅一次（DFM duplicateMerging 思路）
                    if key in self._text_at:
                        continue
                    lane = -1
                    for i in range(self.lanes):
                        if now >= self._lane_free[i]:
                            lane = i
                            break
                    if lane < 0:
                        continue  # 全泳道忙：丢弃（无排队，近似笔端忙闲语义）
                    try:
                        with open(self.paths[lane], "w") as fh:
                            fh.write(txt)
                    except Exception:
                        continue
                    # 相位补偿清除（"中间出现/中间消失/两遍"同根：mod 写入相位 m0 随机，固定
                    # 0.9T 对真实显示窗 T-m0 不对齐）→ m0=mod(media_t*SPD, T)，
                    # 清除=出屏时刻(T-m0)/SPD 前 0.35s（吸收 t0 估算与 0.4s 轮询误差）
                    tw_px = _text_px(txt)
                    Tpx = self.out_w + tw_px
                    if self.media_t0 is not None:
                        mt = max(0.0, now - self.media_t0)
                        m0 = (mt * _DM_SPD[lane]) % Tpx
                        clear_after = max(0.5, (Tpx - m0) / _DM_SPD[lane] - 0.35)
                    else:  # 流未输出首个 chunk 前的保守 fallback
                        clear_after = (Tpx * 0.9) / _DM_SPD[lane]
                    self._lane_free[lane] = now + clear_after
                    self._text_at[key] = now
                # 护栏：只淘汰 30 分钟前的（新鲜的删=同文案再推时复活=重复）；
                # 极端超限才按插入序砍 10% 兜底防内存
                if len(self._text_at) > 5000:
                    for k, at in list(self._text_at.items()):
                        if now - at > 1800:
                            self._text_at.pop(k, None)
                    if len(self._text_at) > 6000:
                        for k in list(self._text_at)[:600]:
                            self._text_at.pop(k, None)
            except Exception as exc:
                sys.stderr.write("[live_proxy] burner %s: %s\n" % (self.room, exc))

    def shutdown(self):
        self._stop = True
        for p in self.paths:
            try:
                open(p, "w").write(" ")
            except Exception:
                pass


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):  # 安静日志（只留异常）
        sys.stderr.write("[live_proxy] " + (fmt % args) + "\n")

    def _code(self, code, ctype, body: bytes):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        try:
            parsed = urlparse(self.path)
            if parsed.path == "/health":
                self._code(200, "text/plain", b"ok")
                return
            if parsed.path == "/danmaku":
                qs = parse_qs(parsed.query)
                room = int((qs.get("room") or ["0"])[0] or 0)
                since = int((qs.get("since") or ["0"])[0] or 0)
                if room <= 0:
                    self._code(400, "text/plain", b"missing room")
                    return
                try:
                    feed = _room_feed(room)
                    body = json.dumps(feed.since(since), ensure_ascii=False).encode("utf8")
                    self._code(200, "application/json", body)
                except Exception as exc:
                    body = json.dumps({"code": -1, "data": {"ok": False, "error": str(exc)[:160]}}).encode("utf8")
                    self._code(500, "application/json", body)
                return
            if parsed.path != "/live":
                self._code(404, "text/plain", b"not found")
                return
            qs = parse_qs(parsed.query)
            url = (qs.get("u") or [""])[0]
            room = int((qs.get("room") or ["0"])[0] or 0)
            lanes = int((qs.get("lanes") or ["0"])[0] or 0)
            if not url and room <= 0:
                self._code(400, "text/plain", b"missing u/room")
                return
            # 线路表：笔端 u= 为首条 + getRoomPlayInfo v2 多线路（断线轮换用）
            urls = []
            if url and allowed(url):
                urls.append(url)
            if room > 0:
                try:
                    for u2 in _fetch_live_urls(room):
                        if allowed(u2) and u2 not in urls:
                            urls.append(u2)
                    sys.stderr.write("[live_proxy] v2 urls room=%s n=%d\n" % (room, len(urls)))
                except Exception as exc:
                    sys.stderr.write("[live_proxy] v2 urls room=%s: %s\n" % (room, exc))
            if not urls:
                self._code(403, "text/plain", b"no allowed stream url")
                return

            import select as _select
            out_w = _probe_out_width(urls[0]) if room > 0 else 640
            sys.stderr.write("[live_proxy] pipeline room=%s lanes=%d out_w=%d\n" % (room, lanes, out_w))
            fp = FramePipeline(room, lanes, out_w) if (room > 0 and lanes > 0) else None
            stop_evt = threading.Event()
            gid = int(time.time() * 1000) % 100000000
            g = {
                "up": None,
                "fifo_a": "/tmp/pen_upa_%d.fifo" % gid,   # 上游音频 → 桥
                "fifo_b": "/tmp/pen_dnba_%d.fifo" % gid,   # 桥 → 下游（桥全程持写端=永不 EOF）
            }
            frame_sz = out_w * 360 * 3

            def kill_up():
                if g["up"] is not None:
                    if g["up"].poll() is None:
                        g["up"].kill()
                        g["up"].wait()
                    g["up"] = None

            def start_up(src):
                """（热切）只换上游：下游/桥/HTTP 全程常驻——换线不再重启下游 = 不再触发
                笔端 ffmpeg 音频 EOF（audio_alive 死亡）——换线窗口只剩视频冻结 1~2s。"""
                kill_up()
                g["up"] = subprocess.Popen([
                    FFMPEG, "-hide_banner", "-loglevel", "error", "-y",
                    "-user_agent", "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/124.0.0.0 Safari/537.36",
                    "-headers", "Referer: https://live.bilibili.com/\r\n",
                    "-probesize", "32", "-analyzeduration", "0",
                    "-fflags", "+nobuffer", "-flags", "+low_delay",
                    "-i", src,
                    "-vf", "scale=-2:360,fps=30", "-pix_fmt", "rgb24",
                    "-map", "0:v:0", "-f", "rawvideo", "pipe:1",
                    "-map", "0:a:0", "-c:a", "copy", "-f", "adts", g["fifo_a"],
                ], stdout=subprocess.PIPE)

            # 双 FIFO：上游写A；桥把 A 中继到 B 且**全程持有 B 写端** → 下游音频输入永不 EOF
            try:
                os.mkfifo(g["fifo_a"], 0o600)
                os.mkfifo(g["fifo_b"], 0o600)
            except FileExistsError:
                pass
            # 下游常驻（只起一次）：输入0=音频FIFO B（桥持有写端即时配对）、输入1=视频 pipe
            down = subprocess.Popen([
                FFMPEG, "-hide_banner", "-loglevel", "error",
                "-i", g["fifo_b"],
                "-f", "rawvideo", "-pix_fmt", "rgb24",
                "-video_size", "%dx%d" % (out_w, 360), "-framerate", "30", "-i", "pipe:0",
                "-map", "1:v:0", "-map", "0:a:0",
                "-c:v", "libx264", "-preset", "ultrafast", "-tune", "zerolatency",
                # -pix_fmt yuv420p 关键：不指定时 x264 吃 RGB24 会编成 High 4:4:4——
                # 444 解码开销 2x 且 A53 软解支持极差 = 笔端 0.7x 慢放+周期 underrun（2026-09-27 实测定案）
                "-pix_fmt", "yuv420p",
                "-b:v", "700k", "-g", "30", "-keyint_min", "30",
                # 音频重采样 44100（不能 copy！CDN 源是 48000，笔端 durl 探测到 44100 按 176.4B/ms
                # 换算 → aps 比真实外放快 8.8% → 视频超前累积=音画不同步，2026-09-27 定案）
                "-c:a", "aac", "-ar", "44100", "-b:a", "96k",
                "-muxdelay", "0", "-muxpreload", "0",
                "-f", "flv", "pipe:1",
            ], stdin=subprocess.PIPE, stdout=subprocess.PIPE)
            bridge = threading.Thread(target=_audio_bridge,
                                      args=(g["fifo_a"], g["fifo_b"], stop_evt), daemon=True)
            bridge.start()

            # —— 帧泵线程 B：上游帧 → (PIL) → 下游 stdin；**只热切上游** ——
            def frame_pump():
                idx = 0
                spawns = 0
                reresolve = 2
                same_retry = set()  # 已同线重试过的 idx（CDN 5XX 多为瞬时）
                while not stop_evt.is_set() and spawns < 8:
                    try:
                        start_up(urls[idx])
                    except Exception as exc:
                        sys.stderr.write("[live_proxy] start_up: %s\n" % exc)
                        break
                    spawns += 1
                    sys.stderr.write("[live_proxy] up start idx=%d/%d\n" % (idx, len(urls)))
                    upstream_dead = False
                    frames_n = 0
                    t_fps = time.time()
                    try:
                        while not stop_evt.is_set():
                            r, _, _ = _select.select([g["up"].stdout], [], [], 8.0)
                            if not r:
                                upstream_dead = True  # 8s 无帧=上游卡死
                                break
                            data = b""
                            while len(data) < frame_sz and not stop_evt.is_set():
                                chunk = g["up"].stdout.read(frame_sz - len(data))
                                if not chunk:
                                    upstream_dead = True
                                    break
                                data += chunk
                            if upstream_dead or len(data) < frame_sz:
                                break
                            out = fp.render(data) if fp is not None else data
                            down.stdin.write(out)
                            frames_n += 1
                            now_fps = time.time()
                            if now_fps - t_fps >= 5.0:
                                sys.stderr.write("[live_proxy] pump_fps=%.1f frames=%d idx=%d\n"
                                                 % (frames_n / (now_fps - t_fps), frames_n, idx))
                                t_fps = now_fps
                                frames_n = 0
                    except (BrokenPipeError, ConnectionResetError, ValueError):
                        break  # 下游死（不可恢复）
                    except Exception as exc:
                        sys.stderr.write("[live_proxy] frame loop: %s\n" % exc)
                        upstream_dead = True
                    if stop_evt.is_set():
                        break
                    kill_up()  # 只杀上游（下游/桥不动）
                    if not upstream_dead:
                        break
                    # 瞬时断先同线重试；再换线；耗尽 re-resolve
                    if idx not in same_retry:
                        same_retry.add(idx)
                        sys.stderr.write("[live_proxy] upstream dead → same-url retry idx=%d\n" % idx)
                        time.sleep(1.2)
                        continue
                    idx += 1
                    if idx >= len(urls):
                        if reresolve > 0 and room > 0:
                            reresolve -= 1
                            try:
                                for u2 in _fetch_live_urls(room):
                                    if allowed(u2) and u2 not in urls:
                                        urls.append(u2)
                            except Exception as exc:
                                sys.stderr.write("[live_proxy] re-resolve: %s\n" % exc)
                            idx = 0
                        else:
                            break
                    if idx >= len(urls):
                        break
                    sys.stderr.write("[live_proxy] upstream dead → switch idx=%d/%d\n" % (idx, len(urls)))
                    time.sleep(1)
                if not stop_evt.is_set():
                    # 放弃：让 A 泵收到下游 EOF 收尾（桥 stop 后关 B → 下游音频 EOF → 退出）
                    stop_evt.set()

            def teardown():
                stop_evt.set()
                kill_up()
                bridge.join(timeout=2)          # 桥退出时 close(fifo_b) → 下游音频 EOF
                if down.poll() is None:
                    down.kill()
                    down.wait()
                for f in (g["fifo_a"], g["fifo_b"]):
                    try:
                        os.unlink(f)
                    except Exception:
                        pass

            # 发头（200 + chunked + 禁缓冲）
            self.send_response(200)
            self.send_header("Content-Type", "video/x-flv")
            self.send_header("X-Accel-Buffering", "no")
            self.send_header("Cache-Control", "no-transform, no-store")
            self.send_header("Transfer-Encoding", "chunked")
            self.end_headers()

            pump = threading.Thread(target=frame_pump, daemon=True)
            pump.start()
            try:
                # —— HTTP 泵（handler 线程 A）：下游 stdout → chunked（下游常驻全程一个） ——
                while not stop_evt.is_set():
                    try:
                        chunk = down.stdout.read(65536)  # 64KB 大块
                    except Exception:
                        chunk = None
                    if not chunk:
                        if stop_evt.is_set() or not pump.is_alive():
                            break
                        time.sleep(0.1)
                        continue
                    self.wfile.write(b"%x\r\n" % len(chunk))
                    self.wfile.write(chunk)
                    self.wfile.write(b"\r\n")
                    self.wfile.flush()
                if not stop_evt.is_set():
                    try:
                        self.wfile.write(b"0\r\n\r\n")
                        self.wfile.flush()
                    except Exception:
                        pass
            except (BrokenPipeError, ConnectionResetError):
                pass  # 客户端断开（切页/退出播放）→ 正常
            finally:
                teardown()
                pump.join(timeout=3)
        except Exception as exc:  # 任何异常不断连不清线程
            sys.stderr.write("[live_proxy] error: %s\n" % exc)
            try:
                self._code(500, "text/plain", b"internal error")
            except Exception:
                pass


def main():
    ap = argparse.ArgumentParser(description="PenBili live transcode proxy")
    ap.add_argument("--host", default="0.0.0.0", help="listen host (default 0.0.0.0)")
    ap.add_argument("--port", type=int, default=2050, help="listen port (default 2050: 转码+弹幕同端口)")
    args = ap.parse_args()
    if not shutil.which("ffmpeg"):
        print("[live_proxy] FATAL: ffmpeg not found in PATH", file=sys.stderr)
        sys.exit(1)
    srv = ThreadingHTTPServer((args.host, args.port), Handler)
    print("[live_proxy] listening on http://%s:%d  (ffmpeg: %s)" % (args.host, args.port, FFMPEG))
    print("[live_proxy] PenBili 直播设置中填写: http://<本机IP>:%d" % args.port)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        print("\n[live_proxy] bye")


if __name__ == "__main__":
    main()
