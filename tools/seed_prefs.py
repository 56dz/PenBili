#!/usr/bin/env python3
"""笔端 storage 合并式种子工具（**不抹登录态**）

笔端 storage = 一个 JSON 文件，顶层 key 即 storage 键（值是字符串化的 JSON）：
  /userdisk/miniapp/data/mini_app/pkg/<appid>/data/sharedpreferences/preferences.json

用法：
  python tools/seed_prefs.py --show
  python tools/seed_prefs.py --set 'bili_autotest={"enabled":true,"liveOnly":true,"liveRoom":<房间号>}'
  python tools/seed_prefs.py --set 'bvp_live={"version":1,"addr":"http://192.168.1.100:2050"}'
  python tools/seed_prefs.py --delete bili_autotest
  # 改完必须杀掉小程序再启动（存活实例不会重挂载）：
  #   adb shell "kill -9 $(pidof miniapp)" ; adb shell miniapp_cli install /tmp/x.amr ; adb shell miniapp_cli start <appid> --index
"""
import argparse
import json
import os
import subprocess
import sys

APPID = "8002026091900004"
REMOTE = "/userdisk/miniapp/data/mini_app/pkg/%s/data/sharedpreferences/preferences.json" % APPID
ADB = os.environ.get("ADB", r"D:\Program Files\YoudaoPenToolbox\adb.exe")


def adb(args, stdin=None):
    r = subprocess.run([ADB] + args, capture_output=True, stdin=stdin)
    return r.returncode, r.stdout.decode("utf-8", "replace"), r.stderr.decode("utf-8", "replace")


def read_prefs():
    code, out, err = adb(["shell", "cat " + REMOTE])
    if code != 0 or not out.strip():
        print("读取失败:", err[:200])
        sys.exit(1)
    out = out.strip()
    try:
        return json.loads(out), out
    except Exception as e:
        print("prefs JSON 解析失败:", e)
        print(out[:300])
        sys.exit(1)


def write_prefs(obj):
    tmp = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".deploy", "_prefs_new.json")
    tmp = os.path.abspath(tmp)
    os.makedirs(os.path.dirname(tmp), exist_ok=True)
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(obj, f, ensure_ascii=False)
    code, out, err = adb(["push", tmp, "/tmp/_prefs_new.json"])
    if code != 0:
        print("push 失败:", err[:200])
        sys.exit(1)
    code, out, err = adb(["shell", "cp /tmp/_prefs_new.json " + REMOTE + " && rm -f /tmp/_prefs_new.json && wc -c " + REMOTE])
    print("写入:", out.strip() or err.strip())
    try:
        os.remove(tmp)
    except Exception:
        pass


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--show", action="store_true")
    ap.add_argument("--set", action="append", default=[], help="key=<json 或裸字符串>")
    ap.add_argument("--delete", action="append", default=[], dest="dels")
    a = ap.parse_args()
    obj, _ = read_prefs()
    changed = False
    # 先应用到内存（--show 与 --set 可同时用：改完再打印结果）
    for kv in a.set:
        if "=" not in kv:
            print("跳过（缺 =）:", kv)
            continue
        k, val = kv.split("=", 1)
        val = val.strip()
        if not (val.startswith("{") or val.startswith("[") or val.startswith('"')):
            val = json.dumps(val, ensure_ascii=False)   # 裸字符串按 JSON 字符串存
        json.loads(val)                                 # 校验合法
        obj[k.strip()] = val
        print("set", k.strip(), "->", val[:90])
        changed = True
    for k in a.dels:
        obj.pop(k.strip(), None)
        print("del", k)
        changed = True
    if changed:
        write_prefs(obj)
    if a.show or not changed:
        print("---- 当前 storage ----")
        for k, v in obj.items():
            s = v if isinstance(v, str) else str(v)
            print("%-16s len=%-6d %s" % (k, len(s), s[:100]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
