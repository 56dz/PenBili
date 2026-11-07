#!/usr/bin/env python3
"""解析 tools/probe-x7p.sh 采集到的真机事实，给出 bili_device_profile 建议值。

只做本地文件解析，不连设备、不写设备。输出：
  suggested-profile.json / REPORT.md
"""
import json
import os
import re
import sys


def read(path):
    try:
        with open(path, 'r', encoding='utf-8', errors='replace') as f:
            return f.read()
    except OSError:
        return ''


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else '.'
    cfg_text = read(os.path.join(out, 'cfg.json'))
    summary = read(os.path.join(out, 'drm-summary.txt'))
    modetest = read(os.path.join(out, 'modetest-planes.txt'))
    plugins = read(os.path.join(out, 'gst-plugins.txt'))
    env = read(os.path.join(out, 'env.txt'))

    cfg = {}
    try:
        cfg = json.loads(cfg_text)
    except Exception:
        # 有些固件 cfg.json 带注释或尾逗号，尽力抽 screen 段
        m = re.search(r'"screen"\s*:\s*\{([^}]*)\}', cfg_text, re.S)
        if m:
            for k, v in re.findall(r'"(\w+)"\s*:\s*(-?\d+)', m.group(1)):
                cfg.setdefault('screen', {})[k] = int(v)
    screen = cfg.get('screen', {}) if isinstance(cfg, dict) else {}

    width = int(screen.get('width') or 936)
    height = int(screen.get('height') or 280)
    xoffset = int(screen.get('xoffset') or 0)
    yoffset = screen.get('yoffset')
    if yoffset is None:
        yoffset = max(0, (480 - height) // 2)
    yoffset = int(yoffset)
    direction = int(screen.get('direction') if screen.get('direction') is not None else 270)

    # DRM 窗口：谁在 ACTIVE，谁可能是 UI 合成层
    wins = re.findall(r'^\s*((?:Esmart|Cluster|Smart)\d+-win\d+)\s*:\s*(\w+)', summary, re.M)
    active = [n for n, s in wins if s.upper() == 'ACTIVE']
    ui = [n for n in active if re.match(r'(Esmart0|Cluster0|Smart0)-win', n)]
    overlay = [n for n in active if n not in ui]

    # 面板行方向的原点偏移：多出来的行（960-936=24）怎么分由合成器决定。
    # cfg.json 的 xoffset 就是「UI 带在面板行方向的起点」，是最可靠的来源；
    # 拿不到字段时退回 0（X6 Pro 实测就是 0，960 行正好占满）。
    row_offset = 0
    if 0 <= xoffset <= 100:
        row_offset = xoffset

    # DRM 摘要里各窗口的 dst 几何（Rockchip 会打 `dst: pos:(x,y) rect:(w,h)`
    # 或 `dst x y w h` 两种格式）。把 UI 合成层的 dst 抽出来，用来交叉验证
    # row_offset —— UI 带的起始行必须等于它，否则视频平面会整体偏移。
    ui_dst = {}
    cur = None
    for line in summary.splitlines():
        m = re.match(r'^\s*((?:Esmart|Cluster|Smart)\d+-win\d+)\s*:', line)
        if m:
            cur = m.group(1)
            continue
        if cur is None:
            continue
        if 'dst' in line:
            nums = re.findall(r'-?\d+', line.split('dst', 1)[1])
            if len(nums) >= 4:
                ui_dst[cur] = [int(x) for x in nums[:4]]

    # UI 层 dst 的第二个数字就是「UI 带在面板行方向的起点」，比 cfg.json 的
    # xoffset 更直接（cfg 里那个字段部分固件写 0 但实际居中）。有就用它。
    for name in (ui or []):
        d = ui_dst.get(name)
        if d and 0 <= d[1] <= 100:
            row_offset = d[1]
            break

    # modetest 的 plane id（第一列纯数字）。只作候选，不自动指定：
    # 不同固件的编号含义不同；X6 Pro 已经验证过 76 是带缩放器的 Esmart1-win0。
    plane_ids = []
    for line in modetest.splitlines():
        m = re.match(r'^\s*(\d{2,4})\s+\d+\s+\d+\s', line)
        if m and m.group(1) not in plane_ids:
            plane_ids.append(m.group(1))

    missing = []
    for line in plugins.splitlines():
        parts = line.split(None, 1)
        if len(parts) == 2 and not parts[1].strip():
            missing.append(parts[0])
    present = [p.split()[0] for p in plugins.splitlines() if p.split() and len(p.split()) > 1]

    profile = {
        'name': 'x7p',
        'width': width,
        'height': height,
        'panelWidth': 480,
        'panelXOffset': yoffset,
        'panelYOffset': xoffset,
        'direction': direction,
        # 【2026-11-07 真机回归】不要再建议 -1：
        # 不写 plane-id 时 kmssink 会挑到没有缩放能力的 Smart 平面，1080p/720p
        # 的 buffer 塞不进 234x416 的窗口，driver 只能裁一块贴上去 —— 真机表现
        # 就是「画面贴在底部、被压扁」。RK3562 上带缩放器的是 Esmart，
        # X6 Pro 验证过的 id 是 76（Esmart1-win0），同 SoC 同 DT 沿用。
        # 若下面 modetest 的候选里根本没有 76，或播放时该窗口不 ACTIVE，
        # 再从候选里换一个 Esmart 的 id 试。
        'planeId': 76,
        'rowOffset': row_offset,
        'transpose': 2
    }
    with open(os.path.join(out, 'suggested-profile.json'), 'w', encoding='utf-8') as f:
        json.dump(profile, f, ensure_ascii=False, indent=2)

    lines = [
        '# X7 Pro 播放器取证报告',
        '',
        '> 由 tools/probe-x7p.sh 生成。全部采集命令只读，未改动设备文件。',
        '',
        '## 1. 屏幕 / 几何',
        '',
        '| 项 | 值 |',
        '| --- | --- |',
        '| cfg screen.width（视口宽 = 面板行数） | %d |' % width,
        '| cfg screen.height（视口高 = 逻辑带高） | %d |' % height,
        '| cfg screen.yoffset（面板列偏移） | %d |' % yoffset,
        '| cfg screen.xoffset（面板行偏移） | %d |' % xoffset,
        '| direction | %d |' % direction,
        '| setViewPort(960) 缩放系数 | %.4f |' % (width / 960.0),
        '',
        'X6 Pro 对照：960x266 / yoffset=107 / plane 76。**只要上表与上一行不同，',
        '旧版写死的三个值就一定错**，黑屏是预期结果。',
        '',
        '## 2. DRM 平面',
        '',
        '- summary 中 ACTIVE 窗口：%s' % (', '.join(active) if active else '（读不到）'),
        '- 判为 UI 合成层：%s' % (', '.join(ui) if ui else '（无）'),
        '- 判为可叠视频层：%s' % (', '.join(overlay) if overlay else '（无）'),
        '- modetest 报出的 plane id 候选：%s' % (', '.join(plane_ids[:20]) if plane_ids else '（不可用）'),
        '',
        '- 各窗口 dst 几何：%s' % (json.dumps(ui_dst, ensure_ascii=False) if ui_dst else '（摘要里没有 dst 行）'),
        '- 建议 rowOffset：%d' % row_offset,
        '',
        '**planeId 建议用 76，不要用 -1。** 2026-11-07 真机回归：-1 会让 kmssink',
        '自选，挑到没有缩放能力的 Smart 平面后，视频 buffer 只能被裁一块贴上去，',
        '表现是「画面贴在底部、被压扁」。RK3562 上带缩放器的是 Esmart，X6 Pro',
        '验证过 76（Esmart1-win0）；X7 Pro 同 SoC 同 DT，沿用。若 modetest 候选里',
        '没有 76，或播放时该窗口始终不 ACTIVE，再换一个 Esmart 的 id 试。',
        '',
        'rowOffset 是「UI 带在面板行方向的起点」：面板 480x960，视口只有 936 行时，',
        '多出的 24 行若被合成器居中分配，UI 就从第 12 行开始 —— 视频平面是面板',
        '绝对坐标，不补这 12 行画面就会整体偏移。上面 `各窗口 dst 几何` 里 UI 层',
        '的 y 就是权威值。',
        '',
        '## 3. GStreamer 插件',
        '',
        '- 探测到：%s' % (', '.join(present) if present else '（无输出）'),
        '- 缺失/为空：%s' % (', '.join(missing) if missing else '（无）'),
        '',
        '硬解链路必需：`mppvideodec`、`kmssink`、`h264parse`（直播还要 `flvdemux`、',
        '点播还要 `matroskademux`）。缺任一个都会让视频分支静默建不起来 → 黑屏。',
        '',
        '## 4. 运行环境',
        '',
        '```',
        env.strip()[:1500],
        '```',
        '',
        '## 5. 把参数写进应用',
        '',
        '默认构建已经内置 X7 Pro 档（936x280 / offset 100 / plane 76 / row 0），',
        '大多数情况不需要手工写。真机数值与上表不同时，在 DevTools（9307）里执行：',
        '',
        '```js',
        "$falcon.jsapi.storage.setStorage({ key: 'bili_device_profile',",
        "  data: JSON.stringify(%s) })" % json.dumps(profile, ensure_ascii=False),
        '```',
        '',
        '然后 `miniapp_cli start <appid> diag` 进自检页，第一行会显示生效的 profile、',
        'native planeId 与 KMS 里 ACTIVE 的窗口名。',
        '',
        '## 6. 还是黑屏时按这个顺序查',
        '',
        '1. 自检页第一行 `native plane=?` —— 若显示 `?` 或 `PLAYER_UNSUPPORTED`，',
        '   说明 `libjsapi_player2.so` 根本没被 PMS 装载（检查包内 SKU 目录是否是',
        '   设备实际选取的那个；本仓库构建脚本已同时铺 `libs/`、`libs/arm64/`、',
        '   `libs/arm64-y02_1/`）。',
        '2. 装包后 `adb shell grep -a "\\[device\\]" /userdata/applog/*.log` 看 profile 行，',
        '   确认机型判成 x7p 且 panelOffset 与 cfg.yoffset 一致。',
        '3. 播放中再抓一次 drm-summary，看视频窗口是否 ACTIVE、`buf[0] addr` 非 0、',
        '   `dst pos/rect` 是否落在 `%d..%d` 列内。' % (yoffset, yoffset + height),
        '4. 若窗口 ACTIVE 但画面被 UI 盖住：X7 Pro 的合成层可能 zpos 高于视频平面，',
        '   需要保持 `.page-live` 根节点与 `<hole>` 区域全透明（本仓库已是这样），',
        '   并确认没有其它小程序/浏览器进程占着 overlay 平面。',
        '5. `gst-plugins.txt` 缺插件则不能用 player2 的硬解链路，须改用该机型',
        '   自带的播放后端（回退方案见 docs/x7pro-adaptation.md）。',
        ''
    ]
    with open(os.path.join(out, 'REPORT.md'), 'w', encoding='utf-8') as f:
        f.write('\n'.join(lines))

    print('  视口 %dx%d  列偏移 %d  方向 %d' % (width, height, yoffset, direction))
    print('  ACTIVE 窗口: %s' % (', '.join(active) or '(none)'))
    print('  缺失插件: %s' % (', '.join(missing) or '(none)'))


if __name__ == '__main__':
    main()
