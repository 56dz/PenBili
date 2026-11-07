#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""从 .falcon_ 构建目录组装最终 AMR（WSL / Linux qjsc 路线）。

## 为什么需要它

aiot-vue-cli 的 `-q`（qjsc）在 Windows 上会调 32 位 qjsc.exe，写
`<name>.js.c` 时报 `Permission denied`（本机实测，PowerShell 与 WSL interop
都一样），而**不带 -q** 打包出来的是 `.js` 源码，设备虽然能加载但与发布包
的 `.js.bin` 形态不同。

本脚本用 CLI 自带的 **Linux 版 qjsc**（`assets/bin/linux/qjsc20200705`）在 WSL
里把 `.falcon_/*.js` 编译成 QuickJS 字节码 `.js.bin`，再重算 manifest 的
size/md5 并打包。产物与官方 `-q -p` 流程逐条等价。

cert 重算的规则借鉴 `direct/tools/mk_amr.py` 的 audit/derive：**改了包内任何
文件都必须重算 cert**，否则安装期校验不过。

## 用法

    python3 tools/pack_falcon.py                     # 默认 .falcon_ → <appid>.<ver>.amr
    python3 tools/pack_falcon.py --out X.amr
    python3 tools/pack_falcon.py --qjsc /path/to/qjsc20200705
    python3 tools/pack_falcon.py --audit-only X.amr # 只校验证书

前置：先跑 `aiot-vue-cli ... -c -p` 生成 .falcon_（不要带 -q）。
"""
import argparse
import hashlib
import json
import os
import re
import subprocess
import sys
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEF_FALCON = os.path.join(ROOT, '.falcon_')
DEF_QJSC_CANDIDATES = [
    os.path.join(ROOT, 'node_modules', 'aiot-vue-cli', 'cli-libs',
                 'aiot-qjsc-tool', 'assets', 'bin', 'linux', 'qjsc20200705'),
    '/mnt/c/Users/lenovo/Desktop/direct/undertale-pen/node_modules/aiot-vue-cli/'
    'cli-libs/aiot-qjsc-tool/assets/bin/linux/qjsc20200705',
    '/c/Users/lenovo/Desktop/direct/undertale-pen/node_modules/aiot-vue-cli/'
    'cli-libs/aiot-qjsc-tool/assets/bin/linux/qjsc20200705',
]

# 这些文件**不登记 cert**（与 CLI 的 manifest.generate 行为一致），但
# app_icon.png 仍然必须进包：manifest 的 icon 字段引用它，缺了图标会丢。
CERT_SKIP = {'manifest.json', 'app_icon.png'}


def md5(b):
    return hashlib.md5(b).hexdigest()


def find_qjsc(explicit=None):
    if explicit:
        return explicit if os.path.isfile(explicit) else None
    for p in DEF_QJSC_CANDIDATES:
        if os.path.isfile(p):
            return p
    return None


def module_specifiers(falcon_dir, js_files):
    """复现 aiot-vue-cli 的 share.internalModules（falcon-module 插件收集的所有
    未被 rollup 内联的 import id）。

    CLI 的 qjsc 步骤会给每个模块加 `-M <id>`，让 qjsc 把它当**外部 C 模块**，
    否则 qjsc 编译 diag.js 时会去文件系统找 ./client-xxxx.js 并报
    `ReferenceError: could not load module filename ...`。
    """
    specs = set()
    pats = [
        re.compile(r'''\bfrom\s*["']([^"']+)["']'''),
        re.compile(r'''^\s*import\s*["']([^"']+)["']''', re.M),
        re.compile(r'''import\(\s*["']([^"']+)["']\s*\)'''),
    ]
    for js in js_files:
        with open(js, 'r', encoding='utf-8', errors='replace') as f:
            text = f.read()
        for pat in pats:
            for m in pat.finditer(text):
                s = m.group(1).strip()
                if s:
                    specs.add(s)
    # CLI 的 compileAll((share.internalModules||[]).concat(['vue','falcon-vue-render']))
    specs.update({'vue', 'falcon-vue-render'})
    return sorted(specs)


def compile_js(qjsc, js_path, bin_path, specs):
    """qjsc -m -M <外部模块>... -N <cname> -o x.js.c -c x.js → .js.bin"""
    c_path = js_path + '.c'
    cname = 'qjsc_convert_c_v_1'
    args = [qjsc, '-m']
    for s in specs:
        args += ['-M', s]
    args += ['-N', cname, '-o', os.path.basename(c_path), '-c',
             os.path.basename(js_path)]
    r = subprocess.run(args, cwd=os.path.dirname(os.path.abspath(js_path)),
                       capture_output=True, text=True, timeout=120)
    if r.returncode != 0:
        print('  x qjsc 失败 %s: %s' % (os.path.basename(js_path),
                                       (r.stderr or r.stdout).strip()[:200]))
        return False
    if not os.path.isfile(c_path):
        print('  x qjsc 没生成 %s' % c_path)
        return False
    try:
        with open(c_path, 'r', encoding='utf-8', errors='replace') as f:
            cpp = f.read()
        m = re.search(r'const uint8_t ' + re.escape(cname) +
                      r'\[\d+\] = \{([^}]+)\}', cpp)
        if not m:
            print('  x %s 里找不到字节码数组' % c_path)
            return False
        vals = []
        for item in m.group(1).replace('\r', '').replace('\n', '').split(','):
            s = item.strip()
            if not s.lower().startswith('0x'):
                continue
            vals.append(int(s[2:], 16))
        if not vals:
            print('  x 字节码数组为空')
            return False
        with open(bin_path, 'wb') as f:
            f.write(bytes(vals))
    finally:
        try:
            os.unlink(c_path)
        except OSError:
            pass
    return True


def collect(falcon_dir, qjsc):
    """返回 [(rel_path, bytes)]，manifest 单独处理。

    顺序必须与 CLI 的 qjsc.js 一致：**先把所有 .js 编译完，最后统一删 .js**。
    边编译边删会让后面的 diag.js 找不到它 import 的 ./client-xxxx.js。
    """
    for base, dirs, files in os.walk(falcon_dir):
        for name in files:
            if name.endswith('.js.c') or name.endswith('.js.bin'):
                os.unlink(os.path.join(base, name))

    js_files = []
    entries = []
    for base, dirs, files in os.walk(falcon_dir):
        dirs.sort()
        for name in sorted(files):
            full = os.path.join(base, name)
            rel = os.path.relpath(full, falcon_dir).replace('\\', '/')
            if name.endswith('.js'):
                js_files.append(full)
                continue
            if rel == 'manifest.json':   # 单独写，避免 zip 里出现两条
                continue
            with open(full, 'rb') as f:
                entries.append((rel, f.read()))

    specs = module_specifiers(falcon_dir, js_files)
    print('    外部模块 %d 个: %s' % (len(specs), '、'.join(specs[:12]) +
                                   (' …' if len(specs) > 12 else '')))

    compiled = []
    for js_path in sorted(js_files):
        rel = os.path.relpath(js_path, falcon_dir).replace('\\', '/')
        bin_rel = rel + '.bin'
        bin_path = js_path + '.bin'
        if not compile_js(qjsc, js_path, bin_path, specs):
            return None
        with open(bin_path, 'rb') as f:
            data = f.read()
        compiled.append((js_path, bin_path, bin_rel, data))
    for js_path, bin_path, bin_rel, data in compiled:
        os.unlink(js_path)
        os.unlink(bin_path)
        entries.append((bin_rel, data))
    return entries


def build(falcon_dir, out, qjsc):
    mf_path = os.path.join(falcon_dir, 'manifest.json')
    if not os.path.isfile(mf_path):
        print('x 找不到 %s —— 先跑 aiot-vue-cli -c -p' % mf_path)
        return False
    with open(mf_path, 'r', encoding='utf-8') as f:
        mf = json.load(f)

    print('==> 编译 JS → QuickJS 字节码（qjsc=%s）' % qjsc)
    entries = collect(falcon_dir, qjsc)
    if entries is None:
        return False
    entries.sort(key=lambda kv: (kv[0] != 'manifest.json', kv[0]))

    cert = {n: {'size': len(d), 'md5': md5(d)}
            for n, d in entries if n not in CERT_SKIP}
    mf['cert'] = cert
    manifest = json.dumps(mf, ensure_ascii=False, indent=2).encode('utf-8')

    tmp = out + '.part'
    with zipfile.ZipFile(tmp, 'w', zipfile.ZIP_DEFLATED) as z:
        # manifest 放最前（与打包器一致）
        z.writestr('manifest.json', manifest)
        for n, d in entries:
            z.writestr(n, d)
    os.replace(tmp, out)

    print('==> %s (%d B, %d 条目, cert %d, version %s)' %
          (os.path.basename(out), os.path.getsize(out), len(entries) + 1,
           len(cert), mf.get('version')))
    js = [n for n, _ in entries if n.endswith('.js')]
    bins = [n for n, _ in entries if n.endswith('.js.bin')]
    libs = [n for n, _ in entries if n.startswith('libs/')]
    print('    .js 源码残留 %d（应为 0） / .js.bin %d / libs 条目 %d' %
          (len(js), len(bins), len(libs)))
    if js:
        print('x 包里还有 .js 源码，形态与原发布包不一致')
        return False
    sku = sorted({n.rsplit('/', 1)[0] for n in libs})
    print('    libs 目录：%s' % ('、'.join(sku) or '（无）'))
    ok, lines = audit(out)
    for line in lines:
        print('    ' + line)
    return ok and not js


def audit(path):
    z = zipfile.ZipFile(path)
    names = set(z.namelist())
    if 'manifest.json' not in names:
        return False, ['x 包里没有 manifest.json']
    mf = json.loads(z.read('manifest.json').decode('utf-8'))
    cert = mf.get('cert', {}) or {}
    bad = []
    for n, c in sorted(cert.items()):
        if n not in names:
            bad.append('缺文件 %s' % n)
            continue
        d = z.read(n)
        if len(d) != c.get('size') or md5(d) != c.get('md5'):
            bad.append('size/md5 不符 %s' % n)
    return (not bad), (['v cert 与包内实体逐条一致（%d 条）' % len(cert)] if not bad
                       else ['x ' + b for b in bad])


def main():
    ap = argparse.ArgumentParser(description='从 .falcon_ 组装 .js.bin 形态的 AMR')
    ap.add_argument('--falcon-dir', default=DEF_FALCON)
    ap.add_argument('--out', default=None)
    ap.add_argument('--qjsc', default=None)
    ap.add_argument('--audit-only', metavar='AMR', default=None)
    a = ap.parse_args()

    if a.audit_only:
        ok, lines = audit(a.audit_only)
        print('=== 审计 %s ===' % a.audit_only)
        for line in lines:
            print('  ' + line)
        return 0 if ok else 1

    qjsc = find_qjsc(a.qjsc)
    if not qjsc:
        print('x 找不到 Linux 版 qjsc20200705，请用 --qjsc 指定')
        return 1
    try:
        os.chmod(qjsc, 0o755)
    except OSError:
        pass

    out = a.out
    if not out:
        with open(os.path.join(a.falcon_dir, 'manifest.json'), encoding='utf-8') as f:
            mf = json.load(f)
        ver = (mf.get('version') or '0_0_0').replace('.', '_')
        out = os.path.join(ROOT, '%s.%s.amr' % (mf.get('appid') or 'app', ver))
    return 0 if build(a.falcon_dir, out, qjsc) else 1


if __name__ == '__main__':
    sys.exit(main())
