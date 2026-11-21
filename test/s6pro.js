// S6 Pro 分支纯逻辑单测（Node 直接运行：node test/s6pro.js）
// 覆盖 3.5.9 同步进来的新模块（分P/合集、CC 字幕、互动视频、播放控件）
// 以及 S6 Pro 的屏幕几何 / 弹幕移除后的基线约束。
// 上游 X6/X7 的 GStreamer/MPP 原生断言不适用于本分支，未收录。
const assert = require('assert');

let passed = 0;
let failed = 0;
const pending = [];

function test(name, fn) {
  const ok = () => { passed++; console.log('  ok  ' + name); };
  const bad = (e) => { failed++; console.log('FAIL  ' + name + ' -> ' + (e && e.message)); };
  try {
    const r = fn();
    if (r && typeof r.then === 'function') {
      const done = r.then(ok, bad);
      pending.push(Promise.race([
        done,
        new Promise((resolve) => setTimeout(() => {
          failed++;
          console.log('FAIL  ' + name + ' -> TIMEOUT(10s)');
          resolve();
        }, 10000))
      ]));
      return done;
    }
    ok();
  } catch (e) {
    bad(e);
  }
}

async function main() {
  const {
    parsePages, parseSeason, pageIndexOf, episodeIndexOf, fmtDur
  } = await import('../src/services/bili/video_meta.js');
  const {
    normalizeSubtitleSettings, rankSubtitleTracks, pickSubtitleTrack, subtitleFit,
    normalizeSubtitleUrl, parseSubtitleBody, cueAt
  } = await import('../src/services/bili/subtitle.js');
  const {
    parsePlatformAction, normalizeChoice, evalCondition, applyNativeAction,
    normalizeQuestion, segmentEndPlan, parseEdgeInfo, visibleChoices,
    defaultChoice, shouldShowChoices, buildSegmentItem
  } = await import('../src/services/bili/interactive.js');
  const { canAutoHide, formatClock, planBands } = await import('../src/services/play_chrome.js');
  const {
    LOGICAL, PHYSICAL, playAreaFor, playVideoRects, FULL_VIDEO_AREA,
    FULLSCREEN_VIDEO_AREA, FULL_VIDEO_AREA_CC, FULLSCREEN_VIDEO_AREA_CC,
    FP_CC_H, trimToGridRows, seekFillWidth, seekRatioFromX
  } = await import('../src/services/screen.js');
  const { describeBiliCode } = await import('../src/services/bili/client.js');

  /* ---------------- 分P / 合集 ---------------- */
  test('video_meta: parsePages 取 cid/page/part/时长，空 part 用 P 序号', () => {
    const pages = parsePages({
      pages: [
        { cid: 11, page: 1, part: '开场', duration: 100 },
        { cid: 22, page: 2, part: '', duration: 200 },
        { cid: 0, page: 3, part: '坏条目' }
      ]
    });
    assert.deepStrictEqual(pages, [
      { cid: 11, page: 1, title: '开场', durationSec: 100 },
      { cid: 22, page: 2, title: 'P2', durationSec: 200 }
    ]);
  });

  test('video_meta: parseSeason 兼容 bvid 只在 arc / 空 sections 兜底', () => {
    const s = parseSeason({
      ugc_season: {
        id: 9, title: '合集', sections: [
          { title: '正片', episodes: [
            { id: 1, cid: 101, title: '第一集', arc: { bvid: 'BV1xx411c7mD', title: '第一集', duration: 60 } },
            { id: 2, cid: 102, bvid: 'BV1xx411c7mE', title: '', arc: { title: '第二集', duration: 61 } }
          ] }
        ]
      }
    });
    assert.strictEqual(s.count, 2);
    assert.strictEqual(s.sections[0].items[0].bvid, 'BV1xx411c7mD');
    assert.strictEqual(s.sections[0].items[1].title, '第二集');
    const legacy = parseSeason({ ugc_season: { episodes: [{ cid: 1, bvid: 'BV1xx411c7mF', arc: { title: 'X' } }] } });
    assert.strictEqual(legacy.count, 1);
  });

  test('video_meta: pageIndexOf / episodeIndexOf / fmtDur', () => {
    assert.strictEqual(pageIndexOf([{ cid: 1 }, { cid: 2 }], 2), 1);
    assert.strictEqual(pageIndexOf([{ cid: 1 }], 9), -1);
    const season = { sections: [{ items: [{ bvid: 'A' }] }, { items: [{ bvid: 'B' }, { bvid: 'C' }] }] };
    assert.deepStrictEqual(episodeIndexOf(season, 'C'), { si: 1, ii: 1 });
    assert.deepStrictEqual(episodeIndexOf(season, 'Z'), { si: -1, ii: -1 });
    assert.strictEqual(fmtDur(0), '');
    assert.strictEqual(fmtDur(65), '1:05');
    assert.strictEqual(fmtDur(3665), '1:01:05');
  });

  /* ---------------- CC 字幕 ---------------- */
  test('subtitle: 设置归一 + 轨道优先级', () => {
    assert.deepStrictEqual(normalizeSubtitleSettings(null), {
      version: 1, on: false, lan: '', keepOnSegment: true
    });
    const tracks = [
      { lan: 'en', url: 'u1' }, { lan: 'ai-zh', url: 'u2' },
      { lan: 'zh-Hans', url: 'u3' }, { lan: 'zh-Hans', url: 'u3' }
    ];
    const rank = rankSubtitleTracks(tracks, 'en');
    assert.deepStrictEqual(rank.map((t) => t.lan), ['en', 'zh-Hans', 'ai-zh']);
    assert.strictEqual(pickSubtitleTrack(tracks, '').lan, 'zh-Hans');
    assert.strictEqual(pickSubtitleTrack([], ''), null);
  });

  test('subtitle: subtitleFit 覆盖度与起点判定', () => {
    const cues = [{ fromMs: 0, toMs: 92000, text: 'a' }];
    assert.strictEqual(subtitleFit(cues, 100000).ok, true);
    assert.strictEqual(subtitleFit(cues, 100000).dev.toFixed(2), '0.08');
    assert.strictEqual(subtitleFit(cues, 200000).ok, false);
    assert.strictEqual(subtitleFit([], 100000).reason, 'empty');
    const late = [{ fromMs: 60000, toMs: 100000, text: 'a' }];
    assert.strictEqual(subtitleFit(late, 100000).reason, 'late-start');
  });

  test('subtitle: URL 白名单 + body 清洗 + cueAt 边界', () => {
    assert.strictEqual(normalizeSubtitleUrl('//aisubtitle.hdslb.com/a.json').ok, true);
    assert.strictEqual(normalizeSubtitleUrl('https://evil.example/a.json').ok, false);
    assert.strictEqual(normalizeSubtitleUrl('http://i0.hdslb.com/a.json').url.slice(0, 6), 'https:');
    const parsed = parseSubtitleBody({ body: [
      { from: 1.2, to: 3.4, content: '<b>你好</b><br>世界' },
      { from: 0.5, to: 0.6, content: '先' }
    ] });
    assert.strictEqual(parsed.ok, true);
    assert.strictEqual(parsed.cues[0].text, '先');
    assert.strictEqual(parsed.cues[1].text, '你好 世界');
    assert.strictEqual(cueAt(parsed.cues, 600), '先');
    assert.strictEqual(cueAt(parsed.cues, 2000), '你好 世界');
    assert.strictEqual(cueAt(parsed.cues, 9999), '');
  });

  /* ---------------- 互动视频 ---------------- */
  test('interactive: platform_action / choice 归一', () => {
    assert.deepStrictEqual(parsePlatformAction('JUMP 123 456'), { edgeId: 123, cid: 456 });
    assert.deepStrictEqual(parsePlatformAction('bad'), { edgeId: 0, cid: 0 });
    const c = normalizeChoice({ id: 7, platform_action: 'JUMP 7 99', option: ' 选 A ', is_default: 1 });
    assert.strictEqual(c.cid, 99);
    assert.strictEqual(c.option, '选 A');
    assert.strictEqual(c.isDefault, true);
    assert.strictEqual(normalizeChoice({ id: 1 }), null);
    assert.strictEqual(normalizeChoice({ cid: 5, option: ' ' }).auto, true);
  });

  test('interactive: 条件求值与变量赋值', () => {
    assert.strictEqual(evalCondition('$a>=2 && $b==1', { a: 3, b: 1 }), true);
    assert.strictEqual(evalCondition('$a<2', { a: 3 }), false);
    assert.strictEqual(evalCondition('bad expr (', {}), true);
    const v = applyNativeAction('$a=$a+1;$b=2', { a: 4 });
    assert.deepStrictEqual(v, { a: 5, b: 2 });
  });

  test('interactive: 题面窗口 / 片尾计划 / 片段 item', () => {
    assert.strictEqual(shouldShowChoices(92000, 100000, 8000), true);
    assert.strictEqual(shouldShowChoices(91000, 100000, 8000), false);
    const q = normalizeQuestion({ type: 2, duration: 500, pause_video: 1, start_time_r: 300 });
    assert.strictEqual(q.durationMs, 3000);
    assert.strictEqual(q.pauseVideo, true);
    assert.strictEqual(q.leadMs, 3000);
    assert.strictEqual(segmentEndPlan({ isLeaf: 1 }, []).kind, 'leaf');
    assert.strictEqual(segmentEndPlan({ question: { autoJump: true } }, [{ cid: 1 }]).kind, 'auto');
    assert.strictEqual(segmentEndPlan({ question: { pauseVideo: true } }, [{ cid: 1 }]).kind, 'wait');
    const item = buildSegmentItem({ bvid: 'BV1xx411c7mD', title: 't' }, { title: 'n' }, { cid: 9, id: 3 });
    assert.strictEqual(item.useCid, true);
    assert.strictEqual(item.cid, 9);
    assert.strictEqual(item.edgeId, 3);
  });

  test('interactive: parseEdgeInfo / visibleChoices / defaultChoice', () => {
    const node = parseEdgeInfo({
      edge_id: 5, is_leaf: 0,
      story_list: [{ node_id: 1, cid: 777, is_current: 1 }],
      edges: { questions: [{ type: 2, pause_video: 1, start_time_r: 300, choices: [
        { id: 6, cid: 88, option: 'A', condition: '$x==1', is_default: 1 },
        { id: 7, cid: 89, option: 'B', condition: '$x==2', is_hidden: 0 }
      ] }] }
    }, 0);
    assert.strictEqual(node.cid, 777);
    assert.strictEqual(node.choices.length, 2);
    assert.strictEqual(visibleChoices(node, { x: 1 }).length, 1);
    assert.strictEqual(visibleChoices(node, { x: 9 }).length, 2, '条件全不满足时退回全部');
    assert.strictEqual(defaultChoice(node.choices).id, 6);
  });

  /* ---------------- 播放控件 ---------------- */
  test('play_chrome: canAutoHide 只在播放中、控件展开且不忙时计时', () => {
    assert.strictEqual(canAutoHide('playing', { chrome: true }), true);
    assert.strictEqual(canAutoHide('playing', { chrome: false }), false);
    assert.strictEqual(canAutoHide('paused', { chrome: true }), false);
    assert.strictEqual(canAutoHide('playing', { chrome: true, busy: true }), false);
    assert.strictEqual(formatClock(65000), '01:05');
  });

  test('play_chrome: planBands 让出字幕带', () => {
    const b = planBands({ totalH: 390, chrome: true, cc: true, ccFull: true, topH: 0, barH: 0, ccH: 30 });
    assert.strictEqual(b.cc, 30);
    assert.strictEqual(b.video, 360);
  });

  /* ---------------- S6 Pro 几何 ---------------- */
  test('screen: S6 Pro 逻辑/物理常量', () => {
    assert.deepStrictEqual(LOGICAL, { width: 960, height: 390 });
    assert.deepStrictEqual(PHYSICAL, { width: 390, height: 960 });
    assert.strictEqual(FP_CC_H, 30);
  });

  test('screen: playAreaFor 四种栏/字幕组合', () => {
    assert.deepStrictEqual(playAreaFor(false, false), FULL_VIDEO_AREA);
    assert.deepStrictEqual(playAreaFor(false, true), FULL_VIDEO_AREA_CC);
    assert.deepStrictEqual(playAreaFor(true, false), FULLSCREEN_VIDEO_AREA);
    assert.deepStrictEqual(playAreaFor(true, true), FULLSCREEN_VIDEO_AREA_CC);
    assert.strictEqual(FULL_VIDEO_AREA_CC.height, 186);
    assert.strictEqual(FULLSCREEN_VIDEO_AREA_CC.height, 360);
  });

  test('screen: 16:9 在视频带内等比信箱且物理矩形不越出面板', () => {
    const r = playVideoRects(1920, 1080, FULL_VIDEO_AREA);
    assert.strictEqual(r.logical.width, 384);
    assert.strictEqual(r.logical.height, 216);
    assert.strictEqual(r.logical.x, 288);
    const p = r.physical;
    assert.ok(p.x >= 0 && p.y >= 0, '左上角必须在面板内');
    assert.ok(p.x + p.width <= PHYSICAL.width, '右缘越界: ' + JSON.stringify(p));
    assert.ok(p.y + p.height <= PHYSICAL.height, '下缘越界: ' + JSON.stringify(p));
  });

  test('screen: trimToGridRows / seekFillWidth / seekRatioFromX', () => {
    assert.deepStrictEqual(trimToGridRows([1, 2, 3, 4, 5], 2).items, [1, 2, 3, 4]);
    assert.strictEqual(seekFillWidth(0), 0);
    assert.strictEqual(seekFillWidth(1), 443);
    assert.strictEqual(seekFillWidth(2), 443);
    assert.strictEqual(seekRatioFromX(0, 100), 0);
    assert.strictEqual(seekRatioFromX(200, 100), 1);
  });

  /* ---------------- 上游修复 ---------------- */
  test('client: 投币 -401 归因为缺少 buvid', () => {
    const msg = describeBiliCode(-401);
    assert.ok(msg.indexOf('风控') >= 0, msg);
    assert.ok(msg.indexOf('buvid') >= 0, msg);
  });

  await Promise.all(pending);
  console.log('\n' + passed + ' passed, ' + failed + ' failed');
  if (failed) process.exit(1);
}

main().catch((e) => {
  console.error(e);
  process.exit(1);
});
