// 纯逻辑单测（Node 直接运行：node test/run.js）
// 覆盖：MD5 对拍/向量、WBI 官方 worked example、net 归一化、client 四段错误语义、
//       playurl 阶梯与 URL 校验、player 参数映射与 UA/Referer 校验、屏幕几何、
//       probe 编排（成功/失败/取消）、storage schema。
const assert = require('assert');
const crypto = require('crypto');

let passed = 0;
let failed = 0;
const pending = [];

function test(name, fn) {
  const ok = () => {
    passed++;
    console.log('  ok  ' + name);
  };
  const bad = (e) => {
    failed++;
    console.log('FAIL  ' + name + ' -> ' + (e && e.message));
  };
  try {
    const r = fn();
    // 异步用例：登记进 pending，由 main 收尾统一等待后再汇总退出；
    // 单用例 10s 看门狗 —— 挂起必须显式报 TIMEOUT，不能被 process.exit 静默吞掉。
    if (r && typeof r.then === 'function') {
      const done = r.then(ok, bad);
      pending.push(
        Promise.race([
          done,
          new Promise((resolve) =>
            setTimeout(() => {
              failed++;
              console.log('FAIL  ' + name + ' -> TIMEOUT(10s)');
              resolve();
            }, 10000)
          )
        ])
      );
      return done;
    }
    ok();
  } catch (e) {
    bad(e);
  }
}

async function main() {
  const { md5Hex } = await import('../src/services/bili/md5.js');
  const { MIXIN_TABLE, deriveMixinKey, buildSignedQuery, encodeValue } = await import('../src/services/bili/wbi.js');
  const { unwrapResponse, tryParseJson, toRuntimeHeader, httpGet } = await import('../src/services/net.js');
  const {
    createClient,
    apiHeaders,
    cookieFromSession,
    describeBiliCode,
    buildQuery,
    DEFAULT_UA,
    REFERER
  } = await import('../src/services/bili/client.js');
  const {
    buildPlayurlParams,
    buildPlayurlQuery,
    validateStreamUrl,
    parseHtml5Response,
    parseDashResponse,
    isAllowedHost,
    hostOf,
    dimsForQn,
    QN_LADDER,
    DASH_QN_LADDER
  } = await import('../src/services/bili/playurl.js');
  const {
    buildOpenArgs,
    isSafeInput,
    isSafeUserAgent,
    isSafeReferer,
    describeError,
    hasModule,
    openSession,
    resetPlayerModuleCache
  } = await import('../src/services/player.js');
  const {
    fitLogicalRect,
    probeVideoRects,
    logicalToPhysicalPoint,
    isRectInsidePanel,
    playVideoRects,
    BAR_WIDTH
  } = await import('../src/services/screen.js');
  const { runProbe, STEPS } = await import('../src/services/bili/probe.js');
  const { normalizeStoredValue, normalizeSession, normalizeSettings, DEFAULT_TARGET } = await import(
    '../src/services/storage.js'
  );
  const feedMod = await import('../src/services/feed.js');
  const {
    normalizeVideo,
    normalizeSearch,
    normalizeCover,
    formatDuration,
    formatCount,
    parseDuration,
    stripHtml,
    appendDeduped,
    fetchRecommended,
    fetchPopular,
    searchVideos,
    describeListError,
    RCMD_PAGE_SIZE,
    MAX_ITEMS
  } = feedMod;
  const { normalizeHistory, pushHistory, HISTORY_VERSION, MAX_HISTORY, formatHistoryTime } = await import(
    '../src/services/history.js'
  );
  const {
    openVideo,
    resolveVideoUrl,
    togglePlay,
    seekBy,
    readStatus,
    closeSession,
    describePlayError,
    ensureMixin,
    SEEK_STEP_MS
  } = await import('../src/services/play_session.js');
  const { fetchReplies, addReply, parseReplies, normalizeReply } = await import('../src/services/bili/reply.js');
  const { parseDanmakuSeg, toBytes, segUrl, segCount, isDanmakuBytes } = await import('../src/services/bili/danmaku.js');
  const fsx = require('fs');
  const pathx = require('path');
  const fixture = (n) => JSON.parse(fsx.readFileSync(pathx.join(__dirname, '..', 'api-mock', 'fixtures', n), 'utf8'));

  const nodeMd5 = (s) => crypto.createHash('md5').update(s, 'utf8').digest('hex');

  // ---------------- md5 ----------------
  test('md5: 空串向量', () => {
    assert.strictEqual(md5Hex(''), 'd41d8cd98f00b204e9800998ecf8427e');
  });
  test('md5: "abc" 向量', () => {
    assert.strictEqual(md5Hex('abc'), '900150983cd24fb0d6963f7d28e17f72');
  });
  test('md5: fox 向量', () => {
    assert.strictEqual(md5Hex('The quick brown fox jumps over the lazy dog'), '9e107d9d372bb6826bd81d3542a419d6');
  });
  test('md5: 对拍 node crypto（含 55/56/64 填充边界与 UTF-8）', () => {
    const samples = [
      'bar=514&foo=114&wts=1702204169&zab=1919810',
      '中文测试混合ABC',
      'x'.repeat(1000),
      'a'.repeat(55),
      'a'.repeat(56),
      'a'.repeat(63),
      'a'.repeat(64)
    ];
    for (const s of samples) assert.strictEqual(md5Hex(s), nodeMd5(s), JSON.stringify(s.slice(0, 12)));
  });

  // ---------------- wbi ----------------
  test('wbi: MIXIN_TABLE 是 0..63 的置换', () => {
    assert.strictEqual(MIXIN_TABLE.length, 64);
    assert.strictEqual(new Set(MIXIN_TABLE).size, 64);
    for (let i = 0; i < MIXIN_TABLE.length; i++) assert.ok(MIXIN_TABLE[i] >= 0 && MIXIN_TABLE[i] <= 63);
  });
  test('wbi: 官方 worked example 完整复现', () => {
    const q = buildSignedQuery({ foo: 114, bar: 514, zab: 1919810 }, 'ea1db124af3c7062474693fa704f4ff8', 1702204169);
    assert.strictEqual(q, 'bar=514&foo=114&wts=1702204169&zab=1919810&w_rid=8f6f2b5b3d485fe1886cec6a0be8c5d4');
  });
  test('wbi: deriveMixinKey 输出 32 位 hex', () => {
    const k = deriveMixinKey('74e6d6d06a3a1f1e7a7a66b7de92c16f', '56a5b7c8e9fa0b1c2d3e4f5a6b7c8d9e');
    assert.ok(/^[0-9a-f]{32}$/.test(k));
    assert.strictEqual(deriveMixinKey('', '').length, 0);
  });
  test("wbi: 值剔除 !'()* 再编码", () => {
    assert.strictEqual(encodeValue("a'b*c!d(e)f"), 'abcdef');
    assert.strictEqual(encodeValue('x y'), 'x%20y');
  });
  test('wbi: 与参数顺序无关、wts 变则签名变', () => {
    const a = buildSignedQuery({ bvid: 'BV1xx', qn: 32 }, 'a'.repeat(32), 1700000000);
    const b = buildSignedQuery({ qn: 32, bvid: 'BV1xx' }, 'a'.repeat(32), 1700000000);
    assert.strictEqual(a, b);
    const c = buildSignedQuery({ bvid: 'BV1xx', qn: 32 }, 'a'.repeat(32), 1700000001);
    assert.notStrictEqual(a, c);
    assert.ok(a.indexOf('&w_rid=') > 0);
    assert.ok(a.indexOf('wts=1700000000') > 0);
  });

  // ---------------- net ----------------
  test('net: unwrapResponse 成功路径默认 200', () => {
    const r = unwrapResponse({ result: '{"a":1}' });
    assert.strictEqual(r.statusCode, 200);
    assert.strictEqual(r.body, '{"a":1}');
  });
  test('net: unwrapResponse error:3 解析 resCode', () => {
    const r = unwrapResponse({ error: 3, result: { errorMessage: 'curl: (28) resCode:504' } });
    assert.strictEqual(r.statusCode, 504);
    assert.ok(r.errorMessage.indexOf('resCode') >= 0);
  });
  test('net: tryParseJson 只解析 JSON 形态文本', () => {
    assert.deepStrictEqual(tryParseJson('{"x":1}'), { x: 1 });
    assert.strictEqual(tryParseJson('not json'), 'not json');
    assert.strictEqual(tryParseJson(''), null);
    assert.strictEqual(tryParseJson('[1,2]').length, 2);
  });
  test('net: toRuntimeHeader 只保留非空字符串值', () => {
    assert.deepStrictEqual(toRuntimeHeader({ 'User-Agent': 'x', Empty: '', n: 3 }), { 'User-Agent': 'x' });
    assert.strictEqual(toRuntimeHeader({}), null);
    assert.strictEqual(toRuntimeHeader(null), null);
  });
  test('net: 无 $falcon 时 httpGet 以 NET_UNAVAILABLE 拒绝', async () => {
    await assert.rejects(() => httpGet('https://x/'), /NET_UNAVAILABLE/);
  });

  // ---------------- client ----------------
  const okGet = (body, status) => async () => ({ statusCode: status || 200, body: JSON.stringify(body) });

  test('client: nav code=-101 仍给出 wbi 密钥', async () => {
    const c = createClient({
      get: okGet({
        code: -101,
        data: {
          wbi_img: {
            img_url: 'https://i0.hdslb.com/bfs/wbi/74e6d6d06a3a1f1e7a7a66b7de92c16f.png',
            sub_url: 'https://i0.hdslb.com/bfs/wbi/56a5b7c8e9fa0b1c2d3e4f5a6b7c8d9e.png'
          }
        }
      })
    });
    const r = await c.fetchWbiKeys();
    assert.strictEqual(r.ok, true);
    assert.strictEqual(r.code, -101);
    assert.strictEqual(r.imgKey, '74e6d6d06a3a1f1e7a7a66b7de92c16f');
    assert.strictEqual(r.subKey, '56a5b7c8e9fa0b1c2d3e4f5a6b7c8d9e');
  });
  test('client: -352 → stage=api + 风控文案 + v_voucher 标记', async () => {
    const c = createClient({ get: okGet({ code: -352, message: 'risk', data: { v_voucher: 'v1' } }) });
    const r = await c.fetchView('BV1ZCeb6NEyM');
    assert.strictEqual(r.ok, false);
    assert.strictEqual(r.stage, 'api');
    assert.strictEqual(r.code, -352);
    assert.ok(r.message.indexOf('风控') >= 0);
    assert.ok(r.message.indexOf('v_voucher') >= 0);
  });
  test('client: HTTP 非 2xx → stage=http（body 不可得也自洽）', async () => {
    const c = createClient({ get: async () => ({ statusCode: 412, body: '', errorMessage: 'curl: (0)' }) });
    const r = await c.fetchView('BV1ZCeb6NEyM');
    assert.strictEqual(r.stage, 'http');
    assert.strictEqual(r.status, 412);
  });
  test('client: 非 JSON → stage=parse', async () => {
    const c = createClient({ get: async () => ({ statusCode: 200, body: '<html>x</html>' }) });
    const r = await c.fetchView('BV1ZCeb6NEyM');
    assert.strictEqual(r.stage, 'parse');
  });
  test('client: transport 异常 → stage=transport 透传消息', async () => {
    const c = createClient({
      get: async () => {
        throw new Error('NET_TIMEOUT: 请求超时(15000ms)');
      }
    });
    const r = await c.fetchView('BV1ZCeb6NEyM');
    assert.strictEqual(r.stage, 'transport');
    assert.ok(r.message.indexOf('NET_TIMEOUT') >= 0);
  });
  test('client: 请求头必含 UA+Referer，Cookie 可回填', async () => {
    let seen = null;
    const c = createClient({
      get: async (url, o) => {
        seen = o.headers;
        return { statusCode: 200, body: '{"code":0,"data":{}}' };
      }
    });
    await c.fetchView('BV1ZCeb6NEyM');
    assert.strictEqual(seen['User-Agent'], DEFAULT_UA);
    assert.strictEqual(seen.Referer, REFERER);
    assert.ok(!('Cookie' in seen));
    c.setCookie(cookieFromSession({ version: 1, buvid3: 'B3-1', buvid4: 'B4-1' }));
    await c.fetchView('BV1ZCeb6NEyM');
    assert.strictEqual(seen.Cookie, 'buvid3=B3-1; buvid4=B4-1');
  });
  test('client: fetchView 宽高解析（dimension/rotate 互换/pages 兜底/缺 cid 报错）', async () => {
    const c1 = createClient({ get: okGet({ code: 0, data: { cid: 42, aid: 7, title: 'T', duration: 90, dimension: { width: 1920, height: 1080, rotate: 0 } } }) });
    const v = await c1.fetchView('BV1ZCeb6NEyM');
    assert.strictEqual(v.cid, 42);
    assert.strictEqual(v.duration, 90);
    assert.strictEqual(v.width, 1920);
    assert.strictEqual(v.height, 1080);
    // rotate=90 → 显示宽高互换（与 ffmpeg autorotate 转正后的方向一致）
    const c2 = createClient({ get: okGet({ code: 0, data: { cid: 43, duration: 10, dimension: { width: 1920, height: 1080, rotate: 90 } } }) });
    const v2 = await c2.fetchView('BV1ZCeb6NEyM');
    assert.strictEqual(v2.width, 1080);
    assert.strictEqual(v2.height, 1920);
    // 主体无 dimension → pages[0].dimension
    const c3 = createClient({ get: okGet({ code: 0, data: { cid: 44, duration: 5, pages: [{ dimension: { width: 720, height: 1280, rotate: 0 } }] } }) });
    const v3 = await c3.fetchView('BV1ZCeb6NEyM');
    assert.strictEqual(v3.width, 720);
    assert.strictEqual(v3.height, 1280);
    // 全缺失 → 16:9 兜底（不炸、不给0）
    const c4 = createClient({ get: okGet({ code: 0, data: { cid: 45, duration: 5 } }) });
    const v4 = await c4.fetchView('BV1ZCeb6NEyM');
    assert.strictEqual(v4.width, 640);
    assert.strictEqual(v4.height, 360);
    // 缺 cid 报错
    const c5 = createClient({ get: okGet({ code: 0, data: { cid: 0 } }) });
    const v5 = await c5.fetchView('BV1ZCeb6NEyM');
    assert.strictEqual(v5.ok, false);
  });
  test('client: buildQuery 字符串透传、对象编码', () => {
    assert.strictEqual(buildQuery('a=1&b=%20x'), 'a=1&b=%20x');
    assert.strictEqual(buildQuery({ a: 1, b: 'x y' }), 'a=1&b=x%20y');
    assert.strictEqual(buildQuery(null), '');
  });
  test('client: describeBiliCode 收录/未收录码', () => {
    assert.ok(describeBiliCode(-12345).indexOf('-12345') >= 0);
    assert.ok(describeBiliCode(-412).indexOf('412') >= 0);
    assert.ok(describeBiliCode(-101).indexOf('未登录') >= 0);
    assert.ok(describeBiliCode(62002, true).indexOf('v_voucher') >= 0);
  });
  test('client: apiHeaders 基本形状', () => {
    const h = apiHeaders('');
    assert.ok(h['User-Agent'].indexOf('Mozilla') === 0);
    assert.strictEqual(h.Referer, 'https://www.bilibili.com/');
    assert.ok(!('Cookie' in h));
    assert.strictEqual(apiHeaders('c=1').Cookie, 'c=1');
  });

  // ---------------- playurl ----------------
  test('playurl: html5 360p 参数族，阶梯永不含 720p', () => {
    const p = buildPlayurlParams({ bvid: 'BV1ZCeb6NEyM', cid: 42, qn: 32 });
    assert.strictEqual(p.fnval, 1);
    // 主路径 DASH：fnval=16 显式覆盖；回退路径默认 1
    assert.strictEqual(buildPlayurlParams({ bvid: 'BV1ZCeb6NEyM', cid: 42, qn: 16, fnval: 16 }).fnval, 16);
    assert.strictEqual(p.platform, 'html5');
    assert.strictEqual(p.high_quality, 1);
    assert.strictEqual(p.fourk, 0);
    assert.ok(QN_LADDER.every((q) => q < 64));
    assert.deepStrictEqual(QN_LADDER, [32, 16]);
    // DASH 只走 id16（640x360）：dash id32=852x480 把 A53 打到 0.7x 实时（2h 长片实测 underrun 1.4s 循环）
    assert.deepStrictEqual(DASH_QN_LADDER, [16]);
    // DASH 参数集：fnval=16 且**不带 platform=html5**（服务端见 html5 强制降级 durl，真机回退日志坐实）
    const dp = buildPlayurlParams({ bvid: 'BV1ZCeb6NEyM', cid: 42, qn: 16, fnval: 16 });
    assert.strictEqual(dp.fnval, 16);
    assert.strictEqual(dp.platform, undefined);
    assert.strictEqual(dp.high_quality, undefined);
  });
  test('playurl: buildPlayurlQuery 拒绝坏 bvid/cid/密钥', () => {
    assert.strictEqual(buildPlayurlQuery({ bvid: 'xx', cid: 1 }, 'a'.repeat(32), 1).ok, false);
    assert.strictEqual(buildPlayurlQuery({ bvid: 'BV1ZCeb6NEyM', cid: 0 }, 'a'.repeat(32), 1).ok, false);
    assert.strictEqual(buildPlayurlQuery({ bvid: 'BV1ZCeb6NEyM', cid: 1 }, 'short', 1).ok, false);
    const good = buildPlayurlQuery({ bvid: 'BV1ZCeb6NEyM', cid: 1 }, 'a'.repeat(32), 1700000000);
    assert.strictEqual(good.ok, true);
    assert.ok(good.query.indexOf('platform=html5') > 0);
    assert.ok(good.query.indexOf('w_rid=') > 0);
  });
  test('playurl: URL 硬校验（https/无空白/无../≤1000）', () => {
    const good = 'https://upos-sz-mirrorcos.bilivideo.com/upgcxcode/1/2/v.mp4?deadline=1&platform=html5&upsig=abc';
    const ok = validateStreamUrl(good);
    assert.strictEqual(ok.ok, true);
    assert.strictEqual(ok.host, 'upos-sz-mirrorcos.bilivideo.com');
    assert.strictEqual(ok.allowlisted, true);
    assert.strictEqual(validateStreamUrl('http://x/v.mp4').ok, false);
    assert.strictEqual(validateStreamUrl('https://x/a b.mp4').ok, false);
    assert.strictEqual(validateStreamUrl('https://x/' + 'a'.repeat(1000)).ok, false);
    assert.strictEqual(validateStreamUrl('https://x/\u0001.mp4').ok, false);
    assert.strictEqual(validateStreamUrl('https://x/../v.mp4').ok, false);
    assert.strictEqual(validateStreamUrl('').ok, false);
    assert.strictEqual(validateStreamUrl(null).ok, false);
    const unknown = validateStreamUrl('https://cdn.example-host.org/v.mp4');
    assert.strictEqual(unknown.ok, true);
    assert.strictEqual(unknown.allowlisted, false);
  });
  test('playurl: hostOf / isAllowedHost 边界（不误配表外域）', () => {
    assert.strictEqual(hostOf('https://A.Bilivideo.com:443/x'), 'A.Bilivideo.com:443');
    assert.strictEqual(isAllowedHost('bilivideo.com'), true);
    assert.strictEqual(isAllowedHost('upos.bilivideo.com'), true);
    assert.strictEqual(isAllowedHost('evilbilivideo.com'), false);
    assert.strictEqual(isAllowedHost(''), false);
  });
  test('playurl: parseDashResponse（qn池 + avc1优先 + 最低档音频 + timelength）', () => {
    const fx = fixture('playurl_dash.json');
    const r = parseDashResponse({ ok: true, code: 0, data: fx.data }, 16);
    assert.strictEqual(r.ok, true, r.message);
    assert.strictEqual(r.qn, 16);
    // id16 池有 hevc(147670) 与 avc1(230931) 两条 → 必选 avc1（A53 软解避开 HEVC）
    assert.strictEqual(r.codecs.indexOf('avc1'), 0, r.codecs);
    assert.strictEqual(r.bandwidth, 230931);
    assert.strictEqual(r.audioBandwidth, 65716, '音频取最低档 66kbps');
    // URL 选快域：main 是 mcdn:8082（实测5-6s慢节点）→ 必须切到 backup 的 bilivideo.com:443（0.3s）
    assert.ok(r.videoUrl.indexOf('cn-gdjm-cm-01-01.bilivideo.com') > 0, r.videoUrl.slice(0, 80));
    assert.ok(r.audioUrl.indexOf('cn-gdjm-cm-01-01.bilivideo.com') > 0, r.audioUrl.slice(0, 80));
    assert.ok(r.videoUrl.indexOf('mountaintoys') < 0, '非白名单域必须淘汰');
    assert.ok(r.videoUrl.indexOf(':8082') < 0, '烂端口域不得当选');
    assert.strictEqual(r.durationMs, 280128, 'timelength 直接给权威时长');
    const r32 = parseDashResponse({ ok: true, code: 0, data: fx.data }, 32);
    assert.strictEqual(r32.codecs.indexOf('avc1'), 0);
    assert.strictEqual(r32.bandwidth, 355354);
    // 无 dash 结构 → 回退信号（durl 在 → hasDurl）
    const no = parseDashResponse({ ok: true, code: 0, data: { durl: [{ url: 'https://x/v.mp4' }] } }, 16);
    assert.strictEqual(no.ok, false);
    assert.strictEqual(no.hasDurl, true);
    assert.strictEqual(parseDashResponse({ ok: true, code: 0, data: {} }, 16).hasDurl, false);
    // 无音频轨（纯无声稿件）
    const mute = parseDashResponse({ ok: true, code: 0, data: { dash: { video: fx.data.dash.video, audio: [] } } }, 16);
    assert.strictEqual(mute.ok, true);
    assert.strictEqual(mute.audioUrl, '');
    // 传输/请求失败
    assert.strictEqual(parseDashResponse({ ok: false, stage: 'transport', message: 'x' }).stage, 'transport');
    assert.strictEqual(parseDashResponse(null, 16).ok, false);
  });
  test('playurl: parseHtml5Response 正常/无 durl/请求失败', () => {
    const good = parseHtml5Response({
      ok: true,
      code: 0,
      data: { quality: 32, durl: [{ url: 'https://c/v.mp4', length: 123 }], accept_quality: [32, 16] }
    });
    assert.strictEqual(good.ok, true);
    assert.strictEqual(good.sizeBytes, 123);
    assert.strictEqual(good.quality, 32);
    assert.strictEqual(parseHtml5Response({ ok: true, code: 0, data: { durl: [] } }).ok, false);
    assert.strictEqual(parseHtml5Response({ ok: false, stage: 'api', code: -412, message: 'x' }).stage, 'api');
    assert.strictEqual(parseHtml5Response(null).ok, false);
  });
  test('playurl: dimsForQn', () => {
    assert.deepStrictEqual(dimsForQn(32), { width: 640, height: 360 });
    assert.deepStrictEqual(dimsForQn(16), { width: 640, height: 360 });
    assert.deepStrictEqual(dimsForQn(64), { width: 1280, height: 720 });
  });

  // ---------------- player adapter ----------------
  test('player: buildOpenArgs 顺序与钳制（14 个位置参数，含 DASH 第二输入）', () => {
    const args = buildOpenArgs({
      input: 'https://x/v.mp4',
      input2: 'https://x/a.m4s',
      startMs: -5,
      durationMs: 1234.6,
      fps: 0,
      audio: false,
      transpose: 1,
      rect: { x: 0.4, y: 174, width: 254, height: 452 },
      audioDevice: '',
      userAgent: 'UA',
      referer: 'https://r/'
    });
    assert.deepStrictEqual(args, [
      // fps=0 → 回落 24（与 native 侧 `fps<1||fps>60 → 24` 一致）
      'https://x/v.mp4', 0, 1235, 24, 0, 1, 0, 174, 254, 452, '', 'UA', 'https://r/', 'https://x/a.m4s'
    ]);
    // input2 缺省 → 末位空串（native 据此走单文件 0:a:0）
    const solo = buildOpenArgs({ input: 'https://x/v.mp4', startMs: 0, durationMs: 0, fps: 30, rect: { x: 0, y: 0, width: 1, height: 1 } });
    assert.strictEqual(solo.length, 14);
    assert.strictEqual(solo[13], '');
  });
  test('player: isSafeInput 拒绝空白/../超长/非 http(s)', () => {
    assert.strictEqual(isSafeInput('https://x/v.mp4'), true);
    assert.strictEqual(isSafeInput('/abs/path.mp4'), true);
    assert.strictEqual(isSafeInput('https://x/a b'), false);
    assert.strictEqual(isSafeInput('/a/../etc'), false);
    assert.strictEqual(isSafeInput('https://x/' + 'a'.repeat(1100)), false);
    assert.strictEqual(isSafeInput('ftp://x/v'), false);
    assert.strictEqual(isSafeInput(123), false);
  });
  test('player: UA/Referer 校验与 native 规则一致', () => {
    assert.strictEqual(isSafeUserAgent(DEFAULT_UA), true);
    assert.strictEqual(isSafeUserAgent(''), true);
    assert.strictEqual(isSafeUserAgent(null), true);
    assert.strictEqual(isSafeUserAgent('a'.repeat(256)), false);
    assert.strictEqual(isSafeUserAgent('bad\u0007ua'), false);
    assert.strictEqual(isSafeUserAgent('非ascii夏'), false);
    assert.strictEqual(isSafeReferer('https://www.bilibili.com/'), true);
    assert.strictEqual(isSafeReferer('https://a b/'), false);
    assert.strictEqual(isSafeReferer('x'.repeat(513)), false);
    assert.strictEqual(isSafeReferer(''), true);
  });
  test('player: 模块缺失 → PLAYER_NATIVE_MISSING（预期 fallback）', async () => {
    resetPlayerModuleCache();
    assert.strictEqual(await hasModule(), false);
    const r = await openSession({
      input: 'https://x/v.mp4',
      rect: { x: 0, y: 0, width: 2, height: 2 },
      userAgent: DEFAULT_UA
    });
    assert.strictEqual(r.ok, false);
    assert.strictEqual(r.code, 'PLAYER_NATIVE_MISSING');
  });
  test('player: describeError 收录 PLAYER_BAD_HEADERS', () => {
    assert.ok(describeError('PLAYER_BAD_HEADERS').indexOf('请求头') >= 0);
  });

  // ---------------- screen ----------------
  test('screen: 360p 左列矩形（与 profile 真机证据 y174/254x452 一致）', () => {
    const r = probeVideoRects(640, 360);
    assert.deepStrictEqual(r.logical, { x: 0, y: 0, width: 452, height: 254 });
    assert.deepStrictEqual(r.physical, { x: 0, y: 174, width: 254, height: 452 });
    assert.strictEqual(isRectInsidePanel(r.physical), true);
  });
  test('screen: fitLogicalRect 信箱适配', () => {
    const r = fitLogicalRect(1920, 1080);
    assert.strictEqual(r.width, 452);
    assert.strictEqual(r.height, 254);
    const v = fitLogicalRect(360, 640); // 竖屏稿件
    assert.ok(v.width < v.height);
  });
  test('screen: direction=270 四角与越界钳制', () => {
    assert.deepStrictEqual(logicalToPhysicalPoint({ x: 0, y: 0 }, 270), { x: 253, y: 0 });
    assert.deepStrictEqual(logicalToPhysicalPoint({ x: 799, y: 0 }, 270), { x: 253, y: 799 });
    assert.deepStrictEqual(logicalToPhysicalPoint({ x: -5, y: 9999 }, 270), { x: 0, y: 0 });
  });
  test('screen: 越界/非法矩形被拒', () => {
    assert.strictEqual(isRectInsidePanel({ x: 250, y: 0, width: 10, height: 10 }), false);
    assert.strictEqual(isRectInsidePanel({ x: -1, y: 0, width: 10, height: 10 }), false);
    assert.strictEqual(isRectInsidePanel({ x: 0, y: 0, width: 0, height: 10 }), false);
    assert.strictEqual(isRectInsidePanel(null), false);
  });

  // ---------------- storage schema ----------------
  test('storage: normalizeSession 版本/类型容错', () => {
    assert.strictEqual(normalizeSession(null).buvid3, '');
    assert.strictEqual(normalizeSession({ version: 99, buvid3: 'x' }).buvid3, '');
    const s = normalizeSession({ version: 1, buvid3: 'B3', buvid4: 'B4', updatedAt: 5 });
    assert.strictEqual(s.buvid3, 'B3');
    assert.strictEqual(s.buvid4, 'B4');
    assert.strictEqual(normalizeSession({ version: 1, buvid3: 123 }).buvid3, '');
  });
  test('storage: normalizeSettings 非法 bvid 回落默认', () => {
    assert.strictEqual(normalizeSettings(null).bvid, DEFAULT_TARGET.bvid);
    assert.strictEqual(normalizeSettings({ version: 1, bvid: 'bad id' }).bvid, DEFAULT_TARGET.bvid);
    assert.strictEqual(normalizeSettings({ version: 1, bvid: 'BV1ZCeb6NEyM' }).bvid, 'BV1ZCeb6NEyM');
  });
  test('storage: normalizeStoredValue 各形态', () => {
    assert.strictEqual(normalizeStoredValue({ data: 'x' }), 'x');
    assert.strictEqual(normalizeStoredValue({ value: 'y' }), 'y');
    assert.strictEqual(normalizeStoredValue({ data: 3 }), '');
    assert.strictEqual(normalizeStoredValue('z'), 'z');
    assert.strictEqual(normalizeStoredValue(null), '');
  });

  // ---------------- probe 编排 ----------------
  function fakeWorld(overrides) {
    const events = [];
    const calls = { open: 0, stop: 0, release: 0, status: 0 };
    const world = {
      events: events,
      calls: calls,
      openedWith: null,
      player: {
        hasModule: async () => true,
        openSession: async (opts) => {
          calls.open++;
          world.openedWith = opts;
          return { ok: true, state: 'playing', outWidth: 254, outHeight: 452, fps: 30 };
        },
        status: async () => {
          calls.status++;
          return { ok: true, state: 'playing', frames: 30 * calls.status, positionMs: 1000 * calls.status };
        },
        stop: async () => {
          calls.stop++;
          return { ok: true };
        },
        release: async () => {
          calls.release++;
          return { ok: true };
        }
      },
      client: {
        fetchWbiKeys: async () => ({ ok: true, code: -101, imgKey: '7'.repeat(32), subKey: '8'.repeat(32), ms: 10 }),
        fetchFinger: async () => ({ ok: true, code: 0, buvid3: '3-abc', buvid4: '4-abc', ms: 5 }),
        fetchView: async () => ({ ok: true, code: 0, cid: 42027451985, aid: 1, title: '测试标题', duration: 300, ms: 8 }),
        fetchPlayurl: async () => ({
          ok: true,
          code: 0,
          data: {
            quality: 32,
            durl: [{ url: 'https://upos-x.bilivideo.com/a/v.mp4?deadline=1&upsig=zz', length: 2 * 1048576 }],
            accept_quality: [32, 16]
          },
          ms: 12
        })
      },
      hasNet: () => true,
      target: { bvid: 'BV1ZCeb6NEyM' },
      mediaUa: DEFAULT_UA,
      mediaReferer: REFERER,
      nowSec: () => 1700000000,
      sleep: async () => {},
      onSession: (s) => {
        world.session = s;
      },
      cancelled: () => false,
      onStep: (u) => events.push(u)
    };
    if (overrides) Object.assign(world, overrides);
    return world;
  }

  test('probe: 全链路通过（fake transport/player），UA/Referer 送达播放器', async () => {
    const w = fakeWorld();
    const r = await runProbe(w);
    assert.strictEqual(r.ok, true);
    assert.strictEqual(r.qn, 32);
    assert.ok(r.frames > 0);
    assert.strictEqual(w.calls.open, 1);
    assert.strictEqual(w.openedWith.userAgent, DEFAULT_UA);
    assert.strictEqual(w.openedWith.referer, REFERER);
    assert.strictEqual(w.openedWith.rect.width, 254);
    assert.strictEqual(w.openedWith.rect.height, 452);
    const fails = w.events.filter((e) => e.state === 'fail');
    assert.strictEqual(fails.length, 0, JSON.stringify(fails));
    const okIds = w.events.filter((e) => e.state === 'ok').map((e) => e.id);
    ['net', 'wbi', 'view', 'playurl', 'native', 'play', 'watch'].forEach((id) => {
      assert.ok(okIds.indexOf(id) >= 0, 'missing ok: ' + id);
    });
    assert.strictEqual(w.session.buvid3, '3-abc');
    // 日志卫生：步骤详情严禁出现签名 URL 片段
    w.events.forEach((e) => {
      assert.ok(!/upsig|deadline=/.test(e.detail || ''), 'detail leaks url: ' + e.detail);
    });
  });

  test('probe: playurl 失败 → 后续 skip、failedId 正确、不碰播放器', async () => {
    const w = fakeWorld({
      client: {
        fetchWbiKeys: async () => ({ ok: true, code: -101, imgKey: '7'.repeat(32), subKey: '8'.repeat(32), ms: 1 }),
        fetchFinger: async () => ({ ok: false, stage: 'http', message: 'x' }),
        fetchView: async () => ({ ok: true, code: 0, cid: 42, aid: 1, title: 't', duration: 10, ms: 1 }),
        fetchPlayurl: async () => ({ ok: false, stage: 'api', code: -412, message: '风控(-412)：请求被拦截' })
      }
    });
    const r = await runProbe(w);
    assert.strictEqual(r.ok, false);
    assert.strictEqual(r.failedId, 'playurl');
    const byId = {};
    w.events.forEach((e) => {
      byId[e.id] = e;
    });
    assert.strictEqual(byId.finger.state, 'skip');
    assert.strictEqual(byId.native.state, 'skip');
    assert.strictEqual(byId.watch.state, 'skip');
    assert.strictEqual(w.calls.open, 0);
  });

  test('probe: 取消后不再回调 onStep，且已拉起的播放被停掉', async () => {
    let cancelled = false;
    const w = fakeWorld();
    w.sleep = async () => {
      cancelled = true; // watch 第一次回读时取消
    };
    w.cancelled = () => cancelled;
    let count = 0;
    const inner = w.onStep;
    w.onStep = (u) => {
      count++;
      inner(u);
    };
    const r = await runProbe(w);
    assert.strictEqual(r.cancelled, true);
    const frozen = count;
    await new Promise((res) => setImmediate(res));
    assert.strictEqual(count, frozen);
    assert.ok(w.calls.stop >= 1);
    assert.ok(w.calls.release >= 1);
  });

  test('probe: watch 无帧 → failedId=watch 且停掉播放', async () => {
    const w = fakeWorld();
    w.player.status = async () => ({ ok: true, state: 'playing', frames: 0, positionMs: 0 });
    const r = await runProbe(w);
    assert.strictEqual(r.ok, false);
    assert.strictEqual(r.failedId, 'watch');
    assert.ok(w.calls.stop >= 1);
    assert.ok(w.calls.release >= 1);
  });

  test('probe: net 通道缺失立即失败', async () => {
    const w = fakeWorld({ hasNet: () => false });
    const r = await runProbe(w);
    assert.strictEqual(r.failedId, 'net');
    assert.strictEqual(w.calls.open, 0);
  });

  test('probe: STEPS label 唯一（面板渲染依赖）', () => {
    assert.ok(STEPS.length >= 8);
    const labels = STEPS.map((s) => s.label);
    assert.strictEqual(new Set(labels).size, labels.length);
    STEPS.forEach((s) => assert.ok(s.id && s.label));
  });

  // ---------------- feed（fixtures = 开发机真响应裁剪）----------------
  test('feed: normalizeCover 协议归一（// 与 hdslb https → http）', () => {
    assert.strictEqual(normalizeCover('//i0.hdslb.com/a.jpg'), 'http://i0.hdslb.com/a.jpg');
    assert.strictEqual(normalizeCover('https://i1.hdslb.com/b.jpg'), 'http://i1.hdslb.com/b.jpg');
    assert.strictEqual(normalizeCover('http://i1.hdslb.com/c.jpg'), 'http://i1.hdslb.com/c.jpg');
    assert.strictEqual(normalizeCover('https://example.org/x.png'), 'https://example.org/x.png');
    assert.strictEqual(normalizeCover(''), '');
  });
  test('feed: formatDuration / formatCount', () => {
    assert.strictEqual(formatDuration(0), '');
    assert.strictEqual(formatDuration(65), '01:05');
    assert.strictEqual(formatDuration(3671), '1:01:11');
    assert.strictEqual(formatCount(0), '');
    assert.strictEqual(formatCount(9999), '9999');
    assert.strictEqual(formatCount(12345), '1.2万');
    assert.strictEqual(formatCount(123456789), '1.2亿');
  });
  test('feed: rcmd fixture → ≥10 条合规视频（bvid/标题/cover 归一）', () => {
    const fx = fixture('rcmd.json');
    const items = [];
    fx.data.item.forEach((raw) => {
      const it = normalizeVideo(raw, 'rcmd');
      if (it) items.push(it);
    });
    // fixture 为真响应裁剪（保留 12 条），断言按裁剪量
    assert.ok(items.length >= 10, 'n=' + items.length);
    items.forEach((it) => {
      assert.ok(/^BV[0-9A-Za-z]{10}$/.test(it.bvid));
      assert.ok(it.title.length > 0);
      assert.ok(!(it.cover.indexOf('https://') === 0 && it.cover.indexOf('hdslb') >= 0), 'hdslb 不应为 https: ' + it.cover);
    });
  });
  test('feed: popular fixture + 非法条目丢弃', () => {
    const fx = fixture('popular.json');
    const good = fx.data.list.map((x) => normalizeVideo(x, 'hot')).filter(Boolean);
    assert.ok(good.length >= 10, 'n=' + good.length);
    assert.strictEqual(normalizeVideo({ bvid: 'bad' }, 'x'), null);
    assert.strictEqual(normalizeVideo(null), null);
  });
  test('feed: search normalize（剥 HTML / 时长解析 / 直播卡滤除）', () => {
    const s = normalizeSearch({
      bvid: 'BV15Eec6UERy',
      title: '<em class="keyword">测试</em>视频',
      author: 'UP主',
      pic: '//i1.hdslb.com/bfs/archive/x.jpg',
      duration: '59:39',
      play: 12345
    });
    assert.ok(s);
    assert.strictEqual(s.kind, 'video');
    assert.strictEqual(s.title, '测试视频');
    assert.strictEqual(s.up, 'UP主');
    assert.strictEqual(s.durationSec, 3579);
    assert.strictEqual(s.view, 12345);
    assert.strictEqual(s.cover, 'http://i1.hdslb.com/bfs/archive/x.jpg');
    assert.strictEqual(s.cid, 0, '搜索无 cid，靠播放链无条件 view 补齐');
    assert.strictEqual(s.source, 'search');
    // 直播卡（带 roomid）直接滤除
    assert.strictEqual(normalizeSearch({ bvid: 'BV1ZCeb6NEyM', roomid: 123 }), null);
    // 实体 / 多段时长 / 与 formatDuration 互逆
    assert.strictEqual(stripHtml('A&amp;B'), 'A&B');
    assert.strictEqual(parseDuration('1:02:03'), 3723);
    assert.strictEqual(parseDuration(90), 90);
    assert.strictEqual(parseDuration('bad'), 0);
    assert.strictEqual(formatDuration(parseDuration('59:39')), '59:39');
    assert.strictEqual(formatDuration(parseDuration('1:02:03')), '1:02:03');
  });
  test('feed: fetchRecommended 注入 transport（不碰网络，URL+ps 断言）', async () => {
    const fx = fixture('rcmd.json');
    let seenUrl = '';
    const c = createClient({
      get: async (u) => {
        seenUrl = u;
        return { statusCode: 200, body: JSON.stringify(fx) };
      }
    });
    const r = await fetchRecommended(c);
    assert.strictEqual(r.ok, true);
    // fixture 为真响应裁剪（12 条）
    assert.ok(r.items.length >= 10, 'n=' + r.items.length);
    assert.ok(seenUrl.indexOf('/x/web-interface/index/top/feed/rcmd') > 0, seenUrl);
    assert.ok(seenUrl.indexOf('ps=' + RCMD_PAGE_SIZE) > 0, seenUrl);
    assert.strictEqual(r.noMore, true);
  });
  test('feed: searchVideos 全链（WBI 签名 URL + fixture 解析 + 参数防御）', async () => {
    const fx = fixture('search.json');
    let seenUrl = '';
    const c = createClient({
      get: async (u) => {
        seenUrl = u;
        return { statusCode: 200, body: JSON.stringify(fx) };
      }
    });
    const r = await searchVideos(c, 'a'.repeat(32), '测试', 1);
    assert.strictEqual(r.ok, true, r.message);
    assert.ok(r.items.length >= 5, 'n=' + r.items.length);
    assert.ok(seenUrl.indexOf('/x/web-interface/wbi/search/type') > 0, seenUrl);
    assert.ok(seenUrl.indexOf('w_rid=') > 0, seenUrl);
    assert.ok(seenUrl.indexOf('keyword=') > 0, seenUrl);
    r.items.forEach((it) => {
      assert.strictEqual(it.kind, 'video');
      assert.ok(/^BV[0-9A-Za-z]{10}$/.test(it.bvid));
      assert.ok(it.title.indexOf('<') < 0, it.title);
    });
    // 参数防御：mixinKey 非法 / 空关键词
    assert.strictEqual((await searchVideos(c, 'short', 'x', 1)).ok, false);
    assert.strictEqual((await searchVideos(c, 'a'.repeat(32), '  ', 1)).ok, false);
    // 分页越界语义由 numPages 决定（fixture numPages>=1 → page1 不是最后页）
    if (r.noMore === false) assert.ok(r.page === 1);
  });
  test('feed: fetchPopular no_more 语义 + 空列表即无更多', async () => {
    const fx = fixture('popular.json');
    const c1 = createClient({ get: async () => ({ statusCode: 200, body: JSON.stringify(fx) }) });
    const r1 = await fetchPopular(c1, 1);
    assert.strictEqual(r1.ok, true);
    assert.strictEqual(r1.noMore, false); // fixture 未带 no_more → 还能翻
    const c2 = createClient({
      get: async () => ({ statusCode: 200, body: '{"code":0,"data":{"list":[],"no_more":true}}' })
    });
    const r2 = await fetchPopular(c2, 3);
    assert.strictEqual(r2.ok, true);
    assert.strictEqual(r2.noMore, true);
  });
  test('feed: describeListError 四段文案', () => {
    assert.strictEqual(describeListError({ ok: false, stage: 'transport', message: 'NET_TIMEOUT: x' }).indexOf('网络失败'), 0);
    assert.ok(describeListError({ ok: false, stage: 'api', code: -352, message: '风控' }).indexOf('-352') >= 0);
    assert.strictEqual(describeListError({ ok: true }), '');
  });
  test('feed: appendDeduped 去重 + 封顶', () => {
    const a = [{ bvid: 'BV1ZCeb6NEyM' }, { bvid: 'BVaaaaaaaaaa' }];
    const b = [{ bvid: 'BVaaaaaaaaaa' }, { bvid: 'BVbbbbbbbbbb' }];
    const r = appendDeduped(a, b);
    assert.strictEqual(r.added, 1);
    assert.strictEqual(r.items.length, 3);
    assert.strictEqual(r.capped, false);
    const big = [];
    for (let i = 0; i < MAX_ITEMS + 5; i++) big.push({ bvid: 'BV' + String(10000000000 + i) });
    const capped = appendDeduped([], big);
    assert.strictEqual(capped.capped, true);
    assert.strictEqual(capped.items.length, MAX_ITEMS);
  });
  // （live_playinfo.json fixture 保留作技术留档：直播已从 UI 移除，720p 软解不实时）

  // ---------------- reply 评论（读/写） ----------------
  test('reply: fixture 全链读取（解析/分页/请求形态）', async () => {
    const fx = fixture('reply.json');
    let seenUrl = '';
    const c = createClient({
      get: async (u) => {
        seenUrl = u;
        return { statusCode: 200, body: JSON.stringify(fx) };
      }
    });
    const r = await fetchReplies(c, 117320605239627, 1);
    assert.strictEqual(r.ok, true, r.message);
    assert.ok(r.items.length >= 1, 'n=' + r.items.length);
    assert.strictEqual(r.count, 3319);
    assert.strictEqual(r.pn, 1);
    assert.strictEqual(r.noMore, false, '首页 size<size<count 不该到底');
    assert.ok(seenUrl.indexOf('/x/v2/reply?') > 0, seenUrl);
    assert.ok(seenUrl.indexOf('type=1') > 0);
    assert.ok(seenUrl.indexOf('oid=117320605239627') > 0);
    assert.ok(seenUrl.indexOf('sort=2') > 0);
    const it = r.items[0];
    assert.ok(it.rpid > 0 && it.name.length > 0 && it.message.length > 0);
    assert.strictEqual(typeof it.likes, 'number');
    assert.strictEqual(it.rcount, 7, 'fixture 首条 7 条回复');
  });
  test('reply: normalizeReply 边界 + parseReplies 失败透传', () => {
    assert.strictEqual(normalizeReply(null), null);
    assert.strictEqual(normalizeReply({}), null);
    assert.strictEqual(normalizeReply({ rpid: 1, content: { message: '  ' } }), null);
    const ok = normalizeReply({ rpid: 9, mid: 7, member: { uname: '甲' }, content: { message: '你好\n世界' }, like: 3, ctime: 1700000000 });
    assert.strictEqual(ok.name, '甲');
    assert.strictEqual(ok.message, '你好\n世界');
    assert.strictEqual(ok.likes, 3);
    assert.strictEqual(parseReplies({ ok: false, stage: 'transport', message: 'x' }).ok, false);
    assert.strictEqual(parseReplies(null).ok, false);
  });
  test('reply: addReply 参数防御 + 成功/失败链（csrf/form/postForm 注入）', async () => {
    const c = createClient({ post: async () => ({ statusCode: 200, body: '{"code":0,"data":{"rpid":555}}' }) });
    assert.strictEqual((await addReply(c, 0, 'x', 'csrf')).stage, 'param');
    assert.strictEqual((await addReply(c, 1, '   ', 'csrf')).stage, 'param');
    assert.strictEqual((await addReply(c, 1, 'x'.repeat(1001), 'csrf')).stage, 'param');
    assert.strictEqual((await addReply(c, 1, 'x', '')).stage, 'param');
    let seenUrl = '';
    let seenBody = '';
    const c2 = createClient({
      post: async (u, b) => {
        seenUrl = u;
        seenBody = b;
        return { statusCode: 200, body: '{"code":0,"data":{"rpid":555}}' };
      }
    });
    const okR = await addReply(c2, 42, '好看 了不起', 'JCT-abc');
    assert.strictEqual(okR.ok, true, okR.message);
    assert.strictEqual(okR.rpid, 555);
    assert.strictEqual(seenUrl, 'https://api.bilibili.com/x/v2/reply/add');
    assert.ok(seenBody.indexOf('oid=42') >= 0, seenBody);
    assert.ok(seenBody.indexOf('type=1') >= 0);
    assert.ok(seenBody.indexOf('csrf=JCT-abc') >= 0);
    assert.ok(seenBody.indexOf(encodeURIComponent('好看 了不起')) >= 0, seenBody);
    assert.ok(seenBody.indexOf('platform=web') >= 0);
    const c3 = createClient({ post: async () => ({ statusCode: 200, body: '{"code":-111,"message":"csrf 校验失败"}' }) });
    const bad = await addReply(c3, 42, 'x', 'JCT-abc');
    assert.strictEqual(bad.ok, false);
    assert.strictEqual(bad.code, -111);
    // 未收录码 → describeBiliCode 兜底 'B站 code=-111'（含码值；-111 人话收录待真机命中补）
    assert.ok(bad.message.indexOf('-111') >= 0, bad.message);
  });

  // ---------------- danmaku 真实弹幕（protobuf） ----------------
  test('danmaku: fixture 全量解码 + wire 判定 + 分包参数', () => {
    const raw = fsx.readFileSync(pathx.join(__dirname, '..', 'api-mock', 'fixtures', 'dm_seg.bin'));
    const bytes = new Uint8Array(raw);
    assert.strictEqual(isDanmakuBytes(bytes), true, '0x0A 头放行');
    const r = parseDanmakuSeg(bytes);
    assert.strictEqual(r.ok, true, r.reason);
    // 炮姐分包1：5863 条原始 → mode 7（高级 862 条）被过滤
    assert.ok(r.items.length >= 4500, 'n=' + r.items.length);
    assert.ok(r.items.length <= 5863);
    let mx = -1;
    for (const it of r.items) {
      assert.ok(it.p >= 0 && it.text.length > 0);
      if (it.p > mx) mx = it.p;
    }
    assert.ok(mx < 360000, 'progress 限定 6min 分包 mx=' + mx);
    assert.ok(r.items.some((it) => it.text.indexOf('前方高能') >= 0), '中文 UTF-8 解码');
    // 坏输入：HTML/JSON 头被 wire 判定排除；field4 状态包（0x22）放行给解析器双保险
    assert.strictEqual(parseDanmakuSeg(null).ok, false);
    assert.strictEqual(parseDanmakuSeg(new Uint8Array([0x3c, 0x21])).ok, false);
    assert.strictEqual(isDanmakuBytes(new Uint8Array([0x3c, 0x21])), false, '<头 wire4 排除');
    assert.strictEqual(isDanmakuBytes(new Uint8Array([0x7b])), false, '{头 wire3 排除');
    assert.strictEqual(isDanmakuBytes(new Uint8Array([0x22, 0x04, 0x00, 0xc0, 0xfc, 0x15])), true, 'field4 状态包放行');
    // 空 elems 状态包（真机 193 字节形态）→ 0 条不报错（空段续拉由调用方负责）
    const stat = parseDanmakuSeg(new Uint8Array([0x22, 0x04, 0x00, 0xc0, 0xfc, 0x15, 0x2a, 0xb8, 0x03, 0xff, 0xff, 0x7f]));
    assert.strictEqual(stat.ok, true, stat.reason);
    assert.strictEqual(stat.items.length, 0);
    // toBytes 字符串 latin1 降级
    const s = toBytes('A');
    assert.strictEqual(s.length, 1);
    assert.strictEqual(s[0], 65);
    // segUrl 形态：oid=cid（collect 契约；传 aid 得空段——实测踩坑）
    const u = segUrl(1176840, 810872, 2);
    assert.ok(u.indexOf('type=1') > 0, u);
    assert.ok(u.indexOf('oid=1176840') > 0, u);
    assert.ok(u.indexOf('pid=810872') > 0, u);
    assert.ok(u.indexOf('segment_index=2') > 0, u);
    assert.strictEqual(segCount(360000), 1);
    assert.strictEqual(segCount(360001), 2);
    assert.strictEqual(segCount(7200000), 20, '2h=20 包');
  });

  // ---------------- history ----------------
  test('history: 容错（旧版本/脏条目）与封顶', () => {
    assert.strictEqual(normalizeHistory(null).items.length, 0);
    assert.strictEqual(normalizeHistory({ version: 99, items: [{}] }).items.length, 0);
    const raw = { version: HISTORY_VERSION, items: [] };
    for (let i = 0; i < 15; i++) raw.items.push({ bvid: 'BV1ZCeb6NEyM', title: 'x' + i, at: i });
    raw.items.push({ bvid: 'bad-bvid' });
    const n = normalizeHistory(raw);
    assert.ok(n.items.length <= MAX_HISTORY);
    assert.strictEqual(n.items[0].bvid, 'BV1ZCeb6NEyM'); // 去重保留首条
  });
  test('history: pushHistory 置顶去重 + 时间戳', () => {
    let h = { version: HISTORY_VERSION, items: [] };
    h = pushHistory(h, { bvid: 'BV1ZCeb6NEyM', title: 'a', at: 100 }, 200);
    h = pushHistory(h, { bvid: 'BVaaaaaaaaaa', title: 'b', at: 300 }, 300);
    h = pushHistory(h, { bvid: 'BV1ZCeb6NEyM', title: 'a2', at: 400 }, 400);
    assert.strictEqual(h.items.length, 2);
    assert.strictEqual(h.items[0].bvid, 'BV1ZCeb6NEyM');
    assert.strictEqual(h.items[0].title, 'a2');
    assert.strictEqual(h.items[0].at, 400);
  });
  test('history: formatHistoryTime 同天/昨天/更早/0', () => {
    const now = new Date(2026, 8, 22, 12, 0, 0).getTime();
    assert.strictEqual(formatHistoryTime(new Date(2026, 8, 22, 8, 5, 0).getTime(), now), '08:05');
    assert.strictEqual(formatHistoryTime(new Date(2026, 8, 21, 23, 59, 0).getTime(), now), '昨天');
    assert.strictEqual(formatHistoryTime(new Date(2026, 7, 1, 10, 0, 0).getTime(), now), '08-01');
    assert.strictEqual(formatHistoryTime(0, now), '');
  });

  // ---------------- play_session ----------------
  function fakePlayDeps(overrides) {
    const calls = { open: [], pause: 0, resume: 0, seek: [], stop: 0, release: 0, status: 0 };
    const w = {
      calls: calls,
      player: {
        openSession: async (o) => {
          calls.open.push(o);
          return { ok: true, state: 'playing', outWidth: 254, outHeight: 452, fps: 30 };
        },
        pause: async () => {
          calls.pause++;
          return { ok: true, state: 'paused' };
        },
        resume: async () => {
          calls.resume++;
          return { ok: true, state: 'playing' };
        },
        seek: async (ms) => {
          calls.seek.push(ms);
          return { ok: true, positionMs: ms };
        },
        status: async () => {
          calls.status++;
          return { ok: true, state: 'playing', frames: 10 * calls.status, positionMs: 1000 * calls.status };
        },
        stop: async () => {
          calls.stop++;
          return { ok: true };
        },
        release: async () => {
          calls.release++;
          return { ok: true };
        }
      },
      client: {
        fetchWbiKeys: async () => ({ ok: true, code: -101, imgKey: '7'.repeat(32), subKey: '8'.repeat(32) }),
        fetchView: async () => ({ ok: true, cid: 42027451985, duration: 206, title: 'T', width: 1920, height: 1080 }),
        fetchPlayurl: async () => ({
          ok: true,
          code: 0,
          data: { quality: 16, durl: [{ url: 'https://upos-x.bilivideo.com/v.mp4?deadline=1', length: 999 }], accept_quality: [16] }
        })
      },
      mixinMemo: {},
      mediaUa: DEFAULT_UA,
      mediaReferer: REFERER,
      nowSec: () => 1700000000,
      log: null,
      cancelled: () => false
    };
    if (overrides) Object.assign(w, overrides);
    return w;
  }
  const probeItem = { kind: 'video', bvid: 'BV1ZCeb6NEyM', cid: 42027451985, durationSec: 206, title: 'x', up: 'u', cover: '' };

  test('play_session: openVideo 全链（居中物理矩形 + UA/Referer + fps30）', async () => {
    const w = fakePlayDeps();
    const r = await openVideo(w, probeItem);
    assert.strictEqual(r.ok, true);
    assert.strictEqual(r.session.qn, 16);
    assert.strictEqual(r.session.durationMs, 206000);
    const o = w.calls.open[0];
    assert.deepStrictEqual(o.rect, { x: 0, y: 174, width: 254, height: 452 });
    assert.strictEqual(o.userAgent, DEFAULT_UA);
    assert.strictEqual(o.referer, REFERER);
    assert.strictEqual(o.fps, 30);
    assert.strictEqual(o.audio, true);
    assert.strictEqual(o.transpose, 2);
    assert.ok(o.input.indexOf('https://') === 0);
    // fake 返回 durl 形（无 dash）→ dash两阶梯失败后回退成功 → 单文件 input2=''
    assert.strictEqual(o.input2, '');
  });
  test('play_session: DASH 双流（input2 音频轨 + timelength 权威时长）', async () => {
    const w = fakePlayDeps();
    const dashFx = fixture('playurl_dash.json');
    w.client.fetchPlayurl = async () => ({ ok: true, code: 0, data: dashFx.data });
    const r = await openVideo(w, probeItem);
    assert.strictEqual(r.ok, true, r.message);
    assert.strictEqual(r.session.qn, 16, 'DASH 阶梯首档 qn=16（640x360）即成功');
    assert.strictEqual(r.session.durationMs, 280128);
    const o = w.calls.open[0];
    assert.ok(o.input2 && o.input2.indexOf('https://') === 0, 'input2=' + o.input2);
    assert.ok(o.input.indexOf('https://') === 0);
    assert.notStrictEqual(o.input, o.input2, '视频/音频必须是两条不同的流');
  });
  test('play_session: 始终走 view（item 自带 cid 也不跳过）+ 权威字段回填', async () => {
    const w = fakePlayDeps();
    let viewed = 0;
    w.client.fetchView = async () => {
      viewed++;
      return { ok: true, cid: 777, duration: 60, title: 'V', width: 1920, height: 1080 };
    };
    // item 自带 cid/durationSec —— 修复前会跳过 view 导致拿不到宽高（竖屏拉伸根因）
    const r = await openVideo(w, { kind: 'video', bvid: 'BV1ZCeb6NEyM', cid: 12345, durationSec: 99, title: 'feed标题' });
    assert.strictEqual(r.ok, true, r.message);
    assert.strictEqual(viewed, 1, '必须调用 view');
    assert.strictEqual(r.session.cid, 777);
    assert.strictEqual(r.session.durationMs, 60000);
    assert.strictEqual(r.session.title, 'feed标题', 'item 标题优先');
    // 16:9 横屏 → 居中横矩形
    assert.deepStrictEqual(w.calls.open[0].rect, { x: 0, y: 174, width: 254, height: 452 });
  });
  test('play_session: 竖屏稿件(1080x1920) → 物理竖柱 rect（不拉伸成横屏）', async () => {
    const w = fakePlayDeps();
    w.client.fetchView = async () => ({ ok: true, cid: 999, duration: 30, title: '竖屏', width: 1080, height: 1920 });
    const r = await openVideo(w, { kind: 'video', bvid: 'BV1ZCeb6NEyM', cid: 1, durationSec: 1, title: '' });
    assert.strictEqual(r.ok, true, r.message);
    const rect = w.calls.open[0].rect;
    // 信箱适配：逻辑 {329,0,143,254} → 物理 {0,329,254,143}（宽高互换的竖柱，比例=9:16 不变形）
    assert.deepStrictEqual(rect, { x: 0, y: 329, width: 254, height: 143 });
    assert.ok(rect.height < rect.width, '竖屏输出应为竖长方形');
    // 输出比例 ≈ 源比例（不拉伸）：物理 h/w = 143/254 ≈ 1080/1920
    const outRatio = rect.height / rect.width;
    const srcRatio = 1080 / 1920;
    assert.ok(Math.abs(outRatio - srcRatio) < 0.02, 'ratio=' + outRatio);
  });
  test('play_session: 取消标志生效 → stage=cancel（页面据此静默）', async () => {
    const w = fakePlayDeps({ cancelled: () => true });
    const r = await resolveVideoUrl(w, { bvid: 'BV1ZCeb6NEyM', cid: 1, durationSec: 1, title: 'x' });
    assert.strictEqual(r.ok, false);
    assert.strictEqual(r.stage, 'cancel');
  });
  test('play_session: 打开失败结构化 stage=open + code', async () => {
    const w = fakePlayDeps();
    w.player.openSession = async () => ({ ok: false, code: 'PLAYER_FB_FAILED', error: '/dev/fb0 打不开' });
    const r = await openVideo(w, { bvid: 'BV1ZCeb6NEyM', cid: 5, durationSec: 10, title: 't' });
    assert.strictEqual(r.ok, false);
    assert.strictEqual(r.stage, 'open');
    assert.ok(r.message.indexOf('PLAYER_FB_FAILED') >= 0);
  });
  test('play_session: 非法 bvid → stage=param（不发任何请求）', async () => {
    const w = fakePlayDeps();
    let netCalls = 0;
    w.client.fetchView = async () => {
      netCalls++;
      return { ok: true };
    };
    const r = await resolveVideoUrl(w, { bvid: 'x' });
    assert.strictEqual(r.stage, 'param');
    assert.strictEqual(netCalls, 0);
  });
  test('play_session: toggle 映射与失败透传', async () => {
    const w = fakePlayDeps();
    const a = await togglePlay(w, true); // playing → pause
    assert.strictEqual(a.state, 'paused');
    assert.strictEqual(w.calls.pause, 1);
    const b = await togglePlay(w, false);
    assert.strictEqual(b.state, 'playing');
    assert.strictEqual(w.calls.resume, 1);
    w.player.pause = async () => ({ ok: false, code: 'PLAYER_CALL_FAILED', error: 'x' });
    const c = await togglePlay(w, true);
    assert.strictEqual(c.ok, false);
    assert.ok(c.message.indexOf('PLAYER_CALL_FAILED') >= 0);
  });
  test('play_session: seekBy 钳制 [0, duration-1s]，时长未知不设上界', async () => {
    const w = fakePlayDeps();
    const a = await seekBy(w, 5000, 206000, 20000);
    assert.strictEqual(a.target, 25000);
    const b = await seekBy(w, 195000, 206000, 20000);
    assert.strictEqual(b.target, 205000);
    const c = await seekBy(w, 5000, 206000, -20000);
    assert.strictEqual(c.target, 0);
    const d = await seekBy(w, 5000, 0, 20000);
    assert.strictEqual(d.target, 25000);
    assert.deepStrictEqual(w.calls.seek, [25000, 205000, 0, 25000]);
  });
  test('play_session: readStatus 错误翻译 + closeSession 幂等连打', async () => {
    const w = fakePlayDeps();
    const ok1 = await readStatus(w);
    assert.strictEqual(ok1.ok, true);
    assert.strictEqual(ok1.frames, 10);
    w.player.status = async () => ({ ok: true, state: 'error', error: 'HTTP error 403 Forbidden', frames: 0 });
    const bad = await readStatus(w);
    assert.strictEqual(bad.ok, false);
    assert.ok(bad.message.indexOf('403') >= 0);
    await closeSession(w);
    await closeSession(w);
    assert.strictEqual(w.calls.stop, 2);
    assert.strictEqual(w.calls.release, 2);
  });
  test('play_session: describePlayError 阶段翻译 + 步长常量', () => {
    assert.ok(describePlayError({ ok: false, stage: 'wbi', message: 'x' }).indexOf('签名') >= 0);
    assert.strictEqual(describePlayError({ ok: false, stage: 'cancel', message: '已取消' }), '');
    assert.strictEqual(describePlayError({ ok: true }), '');
    assert.strictEqual(SEEK_STEP_MS, 20000);
  });
  test('play_session: ensureMixin 会话级缓存（第二次不再打接口）', async () => {
    const w = fakePlayDeps();
    let n = 0;
    w.client.fetchWbiKeys = async () => {
      n++;
      return { ok: true, imgKey: '1'.repeat(32), subKey: '2'.repeat(32) };
    };
    const a = await ensureMixin(w);
    const b = await ensureMixin(w);
    assert.ok(/^[0-9a-f]{32}$/.test(a));
    assert.strictEqual(a, b);
    assert.strictEqual(n, 1);
  });

  // ---------------- 播放几何（174+452+174=800）----------------
  test('screen: playVideoRects 居中，物理 {0,174,254,452}（与真机证据一致）', () => {
    const r = playVideoRects(640, 360);
    assert.deepStrictEqual(r.logical, { x: 174, y: 0, width: 452, height: 254 });
    assert.deepStrictEqual(r.physical, { x: 0, y: 174, width: 254, height: 452 });
    assert.strictEqual(BAR_WIDTH, 174);
    assert.strictEqual(BAR_WIDTH + r.logical.width + BAR_WIDTH, 800);
  });
  test('screen: 竖屏 playVideoRects → 物理竖柱 {0,329,254,143}（9:16 不变形）', () => {
    const r = playVideoRects(360, 640);
    assert.deepStrictEqual(r.logical, { x: 329, y: 0, width: 143, height: 254 });
    assert.deepStrictEqual(r.physical, { x: 0, y: 329, width: 254, height: 143 });
    // 比例守恒：物理 {w:254,h:143} 是逻辑 {w:143,h:254} 的转置 → h/w = 143/254 ≈ 1080/1920
    const src = 1080 / 1920;
    const out = r.physical.height / r.physical.width;
    assert.ok(Math.abs(out - src) < 0.02, 'src=' + src + ' out=' + out);
    assert.ok(isRectInsidePanel(r.physical));
  });
  test('screen: 宽比例视频必须信箱进 452 列内（越列溢出回归防护）', () => {
    // 历史 bug：fit box=全屏 800×254 → 比 16:9 更宽的视频按高适配得 w>452 →
    // 物理 y 越出 [174,626] → 画面溢出到左栏按钮与右栏评论区（真机实测）。
    const cases = [
      [1920, 1080], // 16:9 回归（历史恰好 452 未暴露）
      [1920, 800], // 2.4:1
      [1920, 817], // 2.35:1 宽银幕
      [2560, 1080], // 21:9
      [3440, 1440], // 带鱼屏
      [3840, 1080], // 32:9 极宽
      [1000, 1000] // 1:1
    ];
    for (const c of cases) {
      const r = playVideoRects(c[0], c[1]);
      const tag = c.join('x');
      assert.ok(r.logical.width <= 452, tag + ' logical.width=' + r.logical.width);
      assert.ok(r.logical.width <= r.logical.height * 4, tag + ' 逻辑比例异常');
      const p = r.physical;
      assert.ok(isRectInsidePanel(p), tag + ' 不在面板内');
      assert.ok(p.y >= BAR_WIDTH, tag + ' 物理 y 越出列首: ' + p.y);
      assert.ok(p.y + p.height <= 800 - BAR_WIDTH, tag + ' 物理 y 越出列尾: ' + (p.y + p.height));
    }
  });
}

main().then(
  async () => {
    // 等全部异步用例落定（含看门狗），再汇总退出
    try {
      await Promise.all(pending);
    } catch (e) {
      /* 单测失败已计入 failed */
    }
    console.log('\n' + passed + ' passed, ' + failed + ' failed');
    process.exit(failed > 0 ? 1 : 0);
  },
  (e) => {
    console.error(e);
    process.exit(1);
  }
);
