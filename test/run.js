// 纯逻辑单测（Node 直接运行：node test/run.js）
// 覆盖：MD5 对拍/向量、WBI 官方 worked example、net 归一化、client 四段错误语义、
//       playurl 阶梯与 URL 校验、player 参数映射与 UA/Referer 校验、屏幕几何、
//       probe 编排（成功/失败/取消）、storage schema、
//       搜索三分栏条目归一化（视频/图文/直播）、图文正文三形态解析、评论类型契约（1/12）。
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
    DASH_QN_LADDER,
    HTML5_FPS
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
  const { normalizeLive, normalizeLiveAddr, LIVE_LIMITS } = await import('../src/services/storage.js');
  const { ensureLiveAddr, probeAddr, parseHealth, buildHealthUrl, PROBE_MS } = await import(
    '../src/services/live.js'
  );
  const feedMod = await import('../src/services/feed.js');
  const {
    normalizeVideo,
    normalizeSearch,
    normalizeSearchArticle,
    normalizeSearchLive,
    normalizeCover,
    formatDuration,
    formatCount,
    parseDuration,
    stripHtml,
    appendDeduped,
    fetchRecommended,
    fetchPopular,
    searchVideos,
    searchBili,
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
    resolveLiveUrl,
    buildLiveUrl,
    LIVE_FPS,
    togglePlay,
    seekBy,
    readStatus,
    closeSession,
    describePlayError,
    ensureMixin,
    ensureMixinResult,
    SEEK_STEP_MS
  } = await import('../src/services/play_session.js');
  const { fetchReplies, addReply, parseReplies, normalizeReply, replyType } = await import('../src/services/bili/reply.js');
  const {
    opusToBlocks,
    opsToBlocks,
    decodeEntities,
    htmlToBlocks,
    parseArticle,
    fetchArticle
  } = await import('../src/services/bili/article.js');
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
  test('client: nav 双栈兜底 —— jsapi.http 失败时 native libcurl 接管', async () => {
    const wbiBody = JSON.stringify({
      code: -101,
      data: {
        wbi_img: {
          img_url: 'https://i0.hdslb.com/bfs/wbi/' + 'a'.repeat(32) + '.png',
          sub_url: 'https://i0.hdslb.com/bfs/wbi/' + 'b'.repeat(32) + '.png'
        }
      }
    });
    let nativeCalls = 0;
    let nativeHeaders = null;
    const c = createClient({
      get: async () => ({ statusCode: 200, body: '<html>risk</html>' }), // jsapi 栈：非 JSON
      getTextNative: async (url, o) => {
        nativeCalls++;
        nativeHeaders = o.headers;
        return { statusCode: 200, body: wbiBody, errorMessage: '' };
      }
    });
    const r = await c.fetchWbiKeys();
    assert.strictEqual(nativeCalls, 1);
    assert.strictEqual(r.ok, true);
    assert.strictEqual(r.imgKey, 'a'.repeat(32));
    assert.strictEqual(nativeHeaders.Origin, 'https://www.bilibili.com'); // native 栈带浏览器头
  });
  test('client: nav 双栈皆败 → 保留信息量大的错误（stage=api 优先）', async () => {
    const c = createClient({
      get: async () => ({ statusCode: 200, body: JSON.stringify({ code: -352, message: 'risk' }) }),
      getTextNative: async () => {
        throw new Error('NET_NATIVE_MISSING: 缺少 libjsapi_httpjson.so');
      }
    });
    const r = await c.fetchWbiKeys();
    assert.strictEqual(r.ok, false);
    assert.strictEqual(r.stage, 'api'); // 不用 transport 覆盖
    assert.strictEqual(r.code, -352);
  });
  test('client: nav 瞬态抽风短退避重试（transport 失败后第 3 趟成功）', async () => {
    const wbiBody = JSON.stringify({
      code: -101,
      data: {
        wbi_img: {
          img_url: 'https://i0.hdslb.com/bfs/wbi/' + 'c'.repeat(32) + '.png',
          sub_url: 'https://i0.hdslb.com/bfs/wbi/' + 'd'.repeat(32) + '.png'
        }
      }
    });
    let n = 0;
    const c = createClient({
      // 第 1 趟 jsapi 模拟 DNS 解析超时（真机 YHttpManager 实测该故障）
      get: async () => {
        n++;
        if (n === 1) throw new Error('Resolving timed out after 6000 milliseconds');
        return { statusCode: 200, body: wbiBody };
      },
      getTextNative: async () => {
        throw new Error('also down');
      }
    });
    const r = await c.fetchWbiKeys();
    assert.strictEqual(r.ok, true);
    assert.strictEqual(r.via, 'jsapi');
    assert.strictEqual(r.imgKey, 'c'.repeat(32));
    assert.strictEqual(n, 2); // 第 3 趟（jsapi）才成功
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
  test('client: 请求头必含 UA+Referer，Cookie 可回填', async () => {    let seen = null;
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
    // v2.6.1：durl 也只走 16——qn32 durl 实测 852×480(409K px) 超解码预算(310K)，再次压垮软解
    assert.deepStrictEqual(QN_LADDER, [16]);
    // 发布阶梯仅保留已验证的 DASH id16(360p)，避免服务端不提供 240p 时额外多发一轮请求。
    // 不提供 id32(480p)：弱设备上解码吞吐低于实时。
    assert.deepStrictEqual(DASH_QN_LADDER, [16]);
    assert.ok(DASH_QN_LADDER.every((q) => q < 32), '不得含 480p/720p');
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
  test('playurl: DASH qn16 保持 360p 并优先 AVC', () => {
    const mk = (video) => ({ ok: true, code: 0, data: { timelength: 1000, dash: { video: video, audio: [{ id: 30216, bandwidth: 65000, baseUrl: 'https://a.bilivideo.com/a.m4s' }] } } });
    const v360 = { id: 16, width: 640, height: 360, bandwidth: 230000, codecs: 'avc1.64001E', baseUrl: 'https://a.bilivideo.com/v16.m4s' };
    const hevc360 = { id: 16, width: 640, height: 360, bandwidth: 90000, codecs: 'hvc1.1.6.L120.90', baseUrl: 'https://a.bilivideo.com/h16.m4s' };
    const v480 = { id: 32, width: 852, height: 480, bandwidth: 400000, codecs: 'avc1.64001F', baseUrl: 'https://a.bilivideo.com/v32.m4s' };
    assert.deepStrictEqual(DASH_QN_LADDER, [16]);
    const p = parseDashResponse(mk([v480, hevc360, v360]), 16, false);
    assert.strictEqual(p.ok, true, p.message);
    assert.strictEqual(p.qn, 16);
    assert.strictEqual(p.width, 640);
    assert.strictEqual(p.height, 360);
    assert.strictEqual(p.codecs.indexOf('avc1'), 0);
    assert.ok(p.avail.indexOf('16:640x360') >= 0, '诊断应包含实际 360p 档位');
    assert.ok(p.avail.indexOf('32:852x480') >= 0, '诊断应包含服务端提供的高档位');
    const onlyHevc = parseDashResponse(mk([hevc360]), 16, true);
    assert.strictEqual(onlyHevc.ok, false);
    assert.strictEqual(onlyHevc.stage, 'codec');
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
  test('feed: describeListError wbi 段可读（不再是 code=?）', () => {
    const s = describeListError({ ok: false, stage: 'wbi', message: 'nav 缺少 wbi_img 字段' });
    assert.strictEqual(s.indexOf('签名密钥获取失败'), 0);
    assert.ok(s.indexOf('nav 缺少 wbi_img 字段') >= 0);
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
  test('feed: appendDeduped 按 key 跨类型去重（v:/a:/l: 前缀不互相顶掉）', () => {
    const a = [{ key: 'v:BV1ZCeb6NEyM', bvid: 'BV1ZCeb6NEyM', kind: 'video' }];
    const b = [
      { key: 'v:BV1ZCeb6NEyM', bvid: 'BV1ZCeb6NEyM', kind: 'video' }, // 重复 → 丢
      { key: 'a:117278561535782', id: 117278561535782, kind: 'article' },
      { key: 'l:88', roomid: 88, kind: 'live' }
    ];
    const r = appendDeduped(a, b);
    assert.strictEqual(r.added, 2);
    assert.strictEqual(r.items.length, 3);
    assert.deepStrictEqual(r.items.map((x) => x.kind), ['video', 'article', 'live']);
  });

  /* ---------------- 搜索三分栏：图文（专栏）与直播条目归一化 ---------------- */
  // 结果卡片字段来自 2026-10-04 真机探针（tools/probe_search.mjs）：
  //   图文卡 search_type=article → { id/aid, title(<em>), image_urls[]（无 pic）, author, view, reply }
  //   直播卡 search_type=live  → { roomid, title(<em>), cover/user_cover, uname, online, cate_name }
  test('feed: normalizeSearchArticle 字段映射（封面取 image_urls[0]，无 pic 字段）', () => {
    const it = normalizeSearchArticle({
      id: 117278561535782,
      title: '【<em class="keyword">测试</em>】专栏标题',
      image_urls: ['//i0.hdslb.com/bfs/article/cover.jpg'],
      author: '作者甲',
      desc: '摘要',
      view: 4321,
      reply: 12
    });
    assert.strictEqual(it.kind, 'article');
    assert.strictEqual(it.key, 'a:117278561535782');
    assert.strictEqual(it.id, 117278561535782);
    assert.strictEqual(it.title, '【测试】专栏标题');
    assert.strictEqual(it.cover, 'http://i0.hdslb.com/bfs/article/cover.jpg');
    assert.strictEqual(it.up, '作者甲');
    assert.strictEqual(it.view, 4321);
    assert.strictEqual(it.reply, 12);
    assert.strictEqual(it.source, 'search');
    // 无封面 → cover 空串（不抛）；无 aid/id → null
    assert.strictEqual(normalizeSearchArticle({ id: 7, title: 'a' }).cover, '');
    assert.strictEqual(normalizeSearchArticle({ title: 'x' }), null);
    assert.strictEqual(normalizeSearchArticle(null), null);
  });
  test('feed: normalizeSearchLive 字段映射（cover/user_cover 兜底）', () => {
    const it = normalizeSearchLive({
      roomid: 88,
      title: '<em>直播</em>间',
      cover: '//i0.hdslb.com/live.jpg',
      uname: '主播乙',
      online: 9,
      cate_name: '网游'
    });
    assert.strictEqual(it.kind, 'live');
    assert.strictEqual(it.key, 'l:88');
    assert.strictEqual(it.roomid, 88);
    assert.strictEqual(it.title, '直播间');
    assert.strictEqual(it.up, '主播乙');
    assert.strictEqual(it.online, 9);
    assert.strictEqual(it.cate, '网游');
    assert.strictEqual(normalizeSearchLive({ cover: 'x' }), null); // 无 roomid
    assert.strictEqual(normalizeSearchLive(null), null);
  });
  test('feed: searchBili(article) 全链（search_type=article + WBI 签名 + 解析）', async () => {
    let seenUrl = '';
    const body = JSON.stringify({
      code: 0,
      data: {
        result: [{ id: 999, title: '专栏A', image_urls: ['//i0.hdslb.com/a.jpg'], author: 'up', view: 5, reply: 1 }],
        numPages: 3,
        numResults: 40
      }
    });
    const c = createClient({
      get: async (u) => {
        seenUrl = u;
        return { statusCode: 200, body: body };
      }
    });
    const r = await searchBili(c, 'a'.repeat(32), '图文', 1, 'article');
    assert.strictEqual(r.ok, true, r.message);
    assert.ok(seenUrl.indexOf('/x/web-interface/wbi/search/type') > 0, seenUrl);
    assert.ok(seenUrl.indexOf('search_type=article') > 0, seenUrl);
    assert.ok(seenUrl.indexOf('w_rid=') > 0, seenUrl);
    assert.strictEqual(r.items.length, 1);
    assert.strictEqual(r.items[0].kind, 'article');
    assert.strictEqual(r.items[0].id, 999);
    assert.strictEqual(r.noMore, false);
    assert.strictEqual(r.total, 40);
    // 缺 id 的卡片被丢弃 → 全丢时返回 parse 失败（区分"无结果"与"没有更多"）
    const c2 = createClient({
      get: async () => ({ statusCode: 200, body: '{"code":0,"data":{"result":[{"title":"x"}],"numPages":1}}' })
    });
    const r2 = await searchBili(c2, 'a'.repeat(32), 'kw', 1, 'article');
    assert.strictEqual(r2.ok, false);
    assert.strictEqual(r2.stage, 'parse');
  });
  test('feed: searchBili 默认回落 video；search_type/参数防御', async () => {
    let seenUrl = '';
    const c = createClient({
      get: async (u) => {
        seenUrl = u;
        return { statusCode: 200, body: JSON.stringify(fixture('search.json')) };
      }
    });
    const r = await searchBili(c, 'a'.repeat(32), '测试', 1); // 未传 searchType
    assert.strictEqual(r.ok, true, r.message);
    assert.ok(seenUrl.indexOf('search_type=video') > 0, seenUrl);
    assert.ok(r.items.length >= 5);
    r.items.forEach((it) => assert.strictEqual(it.kind, 'video'));
    // 未知类型 → 回落 video（不把非法值透传给接口）
    let seen2 = '';
    const c2 = createClient({
      get: async (u) => {
        seen2 = u;
        return { statusCode: 200, body: JSON.stringify(fixture('search.json')) };
      }
    });
    await searchBili(c2, 'a'.repeat(32), 'kw', 1, 'bogus');
    assert.ok(seen2.indexOf('search_type=video') > 0, seen2);
    // 参数防御
    assert.strictEqual((await searchBili(c, 'short', 'x', 1, 'video')).ok, false);
    assert.strictEqual((await searchBili(c, 'a'.repeat(32), '   ', 1, 'video')).ok, false);
  });
  test('feed: searchBili(live) 支持 result 为分组对象（live_room）', async () => {
    // 直播检索 result 不是数组而是按子类型分组的对象（2026-10-04 探针实测）
    let seenUrl = '';
    const body = JSON.stringify({
      code: 0,
      data: {
        result: { live_room: [{ roomid: 5, title: '房', cover: '//i0.hdslb.com/l.jpg', uname: 'u', online: 3 }] },
        numPages: 1
      }
    });
    const c = createClient({
      get: async (u) => {
        seenUrl = u;
        return { statusCode: 200, body: body };
      }
    });
    const r = await searchBili(c, 'a'.repeat(32), 'l', 1, 'live');
    assert.strictEqual(r.ok, true, r.message);
    assert.ok(seenUrl.indexOf('search_type=live') > 0, seenUrl);
    assert.strictEqual(r.items.length, 1);
    assert.strictEqual(r.items[0].kind, 'live');
    assert.strictEqual(r.items[0].roomid, 5);
    assert.strictEqual(r.noMore, true);
  });

  /* ---------------- 图文详情（专栏正文三形态解析） ---------------- */
  test('article: decodeEntities 数字/命名实体', () => {
    assert.strictEqual(decodeEntities('a&amp;b&#39;c&nbsp;d&hellip;'), 'a&b\'c d…');
    assert.strictEqual(decodeEntities(null), '');
    assert.strictEqual(decodeEntities(undefined), '');
  });
  test('article: htmlToBlocks 文本/图片混排 + 段落切块 + <br> 软换行 + script 剔除', () => {
    // 块级标签结束 = 段落硬边界 → 每段独立成块（与 opus/ops 主路径一致；
    // 若合并成单块，.art-p 的 lines:30 会截断长正文）
    const blocks = htmlToBlocks('<p>第一段</p><p>第二段</p><img src="//i0.hdslb.com/a.jpg"><p>第三段</p>', 120);
    assert.strictEqual(blocks.length, 4);
    assert.strictEqual(blocks[0].t, 'text');
    assert.strictEqual(blocks[0].text, '第一段');
    assert.strictEqual(blocks[1].t, 'text');
    assert.strictEqual(blocks[1].text, '第二段');
    assert.strictEqual(blocks[2].t, 'img');
    assert.strictEqual(blocks[2].src, 'https://i0.hdslb.com/a.jpg');
    assert.strictEqual(blocks[2].dw, 436); // 正文列宽 ART_W
    assert.ok(blocks[2].dh > 0);
    assert.strictEqual(blocks[3].t, 'text');
    assert.strictEqual(blocks[3].text, '第三段');
    // <br> 为段内软换行 → 同一块内空格续接（不切块）
    const soft = htmlToBlocks('<p>甲<br>乙</p>', 120);
    assert.strictEqual(soft.length, 1);
    assert.strictEqual(soft[0].text, '甲 乙');
    assert.strictEqual(htmlToBlocks('<script>var a=1</script><style>i{}</style>', 120).length, 0);
    assert.strictEqual(htmlToBlocks('', 120).length, 0);
    assert.strictEqual(htmlToBlocks(null, 120).length, 0);
  });
  test('article: opusToBlocks 富文本段（text.nodes 拼接 + pic 按原始比例算高）', () => {
    const opus = {
      content: {
        paragraphs: [
          { para_type: 1, text: { nodes: [{ word: { words: '标题' } }, { word: { words: '正文' } }] } },
          { para_type: 2, pic: { pics: [{ url: '//i0.hdslb.com/p.jpg', width: 800, height: 400 }] } },
          { para_type: 1, text: { nodes: [{ word: { words: '   ' } }] } } // 空白段丢弃
        ]
      }
    };
    const b = opusToBlocks(opus);
    assert.strictEqual(b.length, 2);
    assert.strictEqual(b[0].t, 'text');
    assert.strictEqual(b[0].text, '标题正文');
    assert.strictEqual(b[1].t, 'img');
    assert.strictEqual(b[1].src, 'https://i0.hdslb.com/p.jpg');
    assert.strictEqual(b[1].dw, 436);
    assert.strictEqual(b[1].dh, 218); // 436*400/800
    assert.strictEqual(opusToBlocks(null).length, 0);
    assert.strictEqual(opusToBlocks({}).length, 0);
  });
  test('article: opsToBlocks（type3 老版 Quill deltas：软换行合并 + 图块 + 卡片占位）', () => {
    const b = opsToBlocks(
      JSON.stringify({
        ops: [
          { insert: '第一段' },
          { insert: '\n第二段' }, // 软换行 → 合并进上一文本块
          { insert: { 'native-image': { url: '//i0.hdslb.com/n.png', width: 100, height: 100 } } },
          { insert: { 'video-card': {} } }
        ]
      })
    );
    assert.strictEqual(b.length, 3);
    assert.strictEqual(b[0].t, 'text');
    assert.strictEqual(b[0].text, '第一段第二段');
    assert.strictEqual(b[1].t, 'img');
    assert.strictEqual(b[1].src, 'https://i0.hdslb.com/n.png');
    assert.strictEqual(b[1].dh, 436); // 1:1 → 436 宽对应高
    assert.strictEqual(b[2].t, 'text');
    assert.strictEqual(b[2].text, '[视频卡片]');
    assert.strictEqual(opsToBlocks('{bad json').length, 0);
    assert.strictEqual(opsToBlocks('').length, 0);
    assert.strictEqual(opsToBlocks(null).length, 0);
  });
  test('article: parseArticle 三形态优先级(opus>ops>html) + id/正文缺失语义', () => {
    // opus 优先
    const r1 = parseArticle({
      ok: true,
      data: {
        id: 5,
        title: 'T',
        type: 3,
        opus: { content: { paragraphs: [{ para_type: 1, text: { nodes: [{ word: { words: 'OPUS正文' } }] } }] } },
        content: '{"ops":[{"insert":"OPS正文"}]}',
        author: { name: 'A' },
        stats: { view: 9, like: 2 },
        words: 100
      }
    });
    assert.strictEqual(r1.ok, true, r1.message);
    assert.strictEqual(r1.article.aid, 5);
    assert.strictEqual(r1.article.blocks[0].text, 'OPUS正文');
    assert.strictEqual(r1.article.author, 'A');
    assert.strictEqual(r1.article.read, 9);
    assert.strictEqual(r1.article.like, 2);
    assert.strictEqual(r1.article.words, 100);
    // opus 为空 → type3 走 ops
    const r2 = parseArticle({ ok: true, data: { id: 6, title: '', type: 3, content: '{"ops":[{"insert":"OPS"}]}' } });
    assert.strictEqual(r2.article.blocks[0].text, 'OPS');
    assert.strictEqual(r2.article.title, '未命名图文');
    // type0 → HTML 兜底
    const r3 = parseArticle({ ok: true, data: { id: 7, title: 'H', type: 0, content: '<p>HTML正文</p>' } });
    assert.strictEqual(r3.article.blocks[0].text, 'HTML正文');
    // 正文全空 → summary 兜底
    const r4 = parseArticle({ ok: true, data: { id: 8, title: 's', summary: '摘要文本' } });
    assert.strictEqual(r4.ok, true);
    assert.strictEqual(r4.article.blocks[0].text, '摘要文本');
    // 无 id → parse 失败；无正文且无 summary → parse 失败
    assert.strictEqual(parseArticle({ ok: true, data: { title: 'x', content: '<p>y</p>' } }).ok, false);
    assert.strictEqual(parseArticle({ ok: true, data: { id: 9, title: 'x' } }).ok, false);
    // transport 失败透传 stage
    const bad = parseArticle({ ok: false, stage: 'transport', message: 'net' });
    assert.strictEqual(bad.ok, false);
    assert.strictEqual(bad.stage, 'transport');
    assert.strictEqual(parseArticle(null).ok, false);
  });
  test('article: fetchArticle 走 WBI 签名（id= + wts= + w_rid=）+ 参数防御', async () => {
    let seenPath = '';
    let seenQuery = '';
    const fakeClient = {
      getHeaders: () => ({ 'User-Agent': 'UA', Referer: 'R' }),
      request: (path, query) => {
        seenPath = path;
        seenQuery = String(query);
        return Promise.resolve({ ok: true, data: { id: 3, title: 'T', content: '<p>c</p>' } });
      }
    };
    const r = await fetchArticle(fakeClient, 'a'.repeat(32), 3);
    assert.strictEqual(r.ok, true, r.message);
    assert.strictEqual(seenPath, '/x/article/view');
    assert.ok(seenQuery.indexOf('id=3') >= 0, seenQuery);
    assert.ok(seenQuery.indexOf('wts=') > 0, seenQuery);
    assert.ok(seenQuery.indexOf('w_rid=') > 0, seenQuery);
    assert.strictEqual(r.article.aid, 3);
    // 参数防御：id 非法 / WBI 密钥非法 都不发请求
    assert.strictEqual((await fetchArticle(fakeClient, 'a'.repeat(32), 0)).stage, 'param');
    assert.strictEqual((await fetchArticle(fakeClient, 'a'.repeat(32), 'x')).stage, 'param');
    assert.strictEqual((await fetchArticle(fakeClient, 'bad', 3)).stage, 'wbi');
  });
  // （live_playinfo.json fixture 保留作技术留档：直播已从 UI 移除，720p 软解不实时）

  /* ---------------- 直播：服务器地址 / 重定向缓存 / HLS 播放地址 ---------------- */
  // 服务端 v5 以 HLS 分片回传（见 server/live_proxy.py）；客户端只把 /live?... 当点播地址。
  test('live: normalizeLiveAddr 补 scheme、去尾斜杠、空值', () => {
    assert.strictEqual(normalizeLiveAddr('192.168.5.224:2050'), 'http://192.168.5.224:2050');
    assert.strictEqual(normalizeLiveAddr('http://penbili.560726.best/'), 'http://penbili.560726.best');
    assert.strictEqual(normalizeLiveAddr('  https://a.b:1//  '), 'https://a.b:1');
    assert.strictEqual(normalizeLiveAddr(''), '');
    assert.strictEqual(normalizeLiveAddr(null), '');
  });
  test('live: normalizeLive schema（缺省 / 越界钳制 / 版本迁移 / 未知版本丢弃）', () => {
    const d = normalizeLive(null);
    assert.strictEqual(d.addr, '');
    assert.strictEqual(d.bufMs, LIVE_LIMITS.bufDefaultMs);
    assert.strictEqual(d.res, 254); // 默认 = 匹配屏幕（452x254；480 真机只有 0.83x 必卡）
    assert.strictEqual(d.trans, 1);
    assert.strictEqual(d.resolvedAddr, '');
    // v1 → v2 迁移：保留地址/缓冲/码率，**只把画质纠正到 254**
    const c = normalizeLive({ version: 1, addr: '1.2.3.4:5', bufMs: 99999, res: 480, bv: '2M', trans: 0, resolvedAddr: 'x.y:9' });
    assert.strictEqual(c.addr, 'http://1.2.3.4:5');
    assert.strictEqual(c.bufMs, LIVE_LIMITS.bufMaxMs);
    assert.strictEqual(c.res, 254);
    assert.strictEqual(c.version, 2);
    assert.strictEqual(c.bv, '2m');
    assert.strictEqual(c.trans, 0);
    assert.strictEqual(c.resolvedAddr, 'http://x.y:9');
    // v2 起尊重用户选择（254/360/480 合法；540 已废弃 → 回落 254）
    assert.strictEqual(normalizeLive({ version: 2, addr: 'a:1', res: 360 }).res, 360);
    assert.strictEqual(normalizeLive({ version: 2, addr: 'a:1', res: 540 }).res, 254);
    assert.strictEqual(normalizeLive({ version: 1, bufMs: 10 }).bufMs, LIVE_LIMITS.bufMinMs);
    assert.strictEqual(normalizeLive({ version: 99, addr: 'z' }).addr, '');
  });
  test('live: buildLiveUrl 参数构造与钳制（cookie 必须 URL 编码）', () => {
    assert.strictEqual(buildLiveUrl('', 1, {}), '');
    assert.strictEqual(buildLiveUrl('http://a:1', 0, {}), '');
    const u = buildLiveUrl('http://a:1/', 88, { bufMs: 4000, res: 360, bv: '900k', trans: 1, ck: 'SESSDATA=x%2Cy; bili_jct=z' });
    assert.strictEqual(u.indexOf('http://a:1/live?'), 0, u);
    assert.ok(u.indexOf('room=88') > 0, u);
    assert.ok(u.indexOf('buf=4000') > 0, u);
    assert.ok(u.indexOf('res=360') > 0, u);
    assert.ok(u.indexOf('bv=900k') > 0, u);
    assert.ok(u.indexOf('trans=1') > 0, u);
    assert.ok(u.indexOf('ck=SESSDATA%3Dx%252Cy%3B%20bili_jct%3Dz') > 0, u);
    const u2 = buildLiveUrl('http://a:1', 5, { bufMs: 999999, res: 111, bv: 'zzz', trans: 0 });
    assert.ok(u2.indexOf('buf=20000') > 0, u2);
    assert.ok(u2.indexOf('res=254') > 0, u2); // 非法 res（含废弃的 540）→ 默认 254
    assert.ok(u2.indexOf('bv=700k') > 0, u2);
    assert.ok(u2.indexOf('trans=0') > 0, u2);
    assert.ok(buildLiveUrl('http://a:1', 5, { res: 540 }).indexOf('res=254') > 0, '540 已废弃');
  });
  test('live: resolveLiveUrl —— 未配地址→liveaddr；正常→单输入+durationMs=0', () => {
    const bad = resolveLiveUrl({ live: {} }, { kind: 'live', roomid: 9 });
    assert.strictEqual(bad.ok, false);
    assert.strictEqual(bad.stage, 'liveaddr');
    assert.ok(describePlayError(bad).indexOf('无法播放') > 0, describePlayError(bad));
    const r = resolveLiveUrl(
      { live: { addr: 'http://s:2050', bufMs: 3000, res: 480, bv: '700k', trans: 1 } },
      { kind: 'live', roomid: 9, title: 'T' }
    );
    assert.strictEqual(r.ok, true);
    assert.strictEqual(r.live, true);
    assert.strictEqual(r.audioUrl, '');
    assert.strictEqual(r.durationMs, 0);
    assert.strictEqual(r.aid, 0);
    assert.strictEqual(r.fps, LIVE_FPS);
    assert.strictEqual(r.roomid, 9);
    assert.ok(r.url.indexOf('room=9') > 0, r.url);
    // kind=live 会绕过 bvid 校验（直播本来就没有 bvid）
    assert.strictEqual(resolveLiveUrl({ live: { addr: 'http://s' } }, { kind: 'live', roomid: 0 }).stage, 'param');
  });
  test('live: parseHealth 解析 host（=301 链终点的权威来源）', () => {
    const h = parseHealth('{"ok":true,"version":"5.0","host":"penbili.560726.xyz:1728","lan":["http://192.168.5.224:2050"],"load":{"sessions":2,"load1":0.6,"high":false}}');
    assert.strictEqual(h.ok, true);
    assert.strictEqual(h.host, 'penbili.560726.xyz:1728');
    assert.strictEqual(h.addr, 'http://penbili.560726.xyz:1728');
    assert.strictEqual(h.version, '5.0');
    assert.strictEqual(h.load.sessions, 2);
    assert.strictEqual(h.lan.length, 1);
    assert.strictEqual(parseHealth('not json'), null);
    assert.strictEqual(parseHealth('{"ok":false}'), null);
    assert.strictEqual(parseHealth(''), null);
    assert.strictEqual(buildHealthUrl('http://a:1/'), 'http://a:1/health');
    assert.strictEqual(buildHealthUrl(''), '');
  });
  test('live: ensureLiveAddr —— 缓存命中 / 失效重解析 / 全失败兜底 / 未配置', async () => {
    const mk = (map) => async (url) => {
      if (!map[url]) throw new Error('NET_FAIL ' + url);
      return { statusCode: 200, body: JSON.stringify(map[url]) };
    };
    assert.strictEqual((await ensureLiveAddr(mk({}), normalizeLive(null))).ok, false);
    // ① 缓存活着 → 直接用缓存（不再碰入口）
    const r1 = await ensureLiveAddr(mk({ 'http://real:7/health': { ok: true, host: 'real:7' } }),
      { addr: 'http://entry', resolvedAddr: 'http://real:7' });
    assert.strictEqual(r1.ok, true);
    assert.strictEqual(r1.source, 'cache');
    assert.strictEqual(r1.addr, 'http://real:7');
    assert.strictEqual(r1.changed, false);
    // ② 缓存死了 + 入口可解析 → 重解析（changed=true，调用方回写缓存）
    const r2 = await ensureLiveAddr(mk({ 'http://entry/health': { ok: true, host: 'real:8' } }),
      { addr: 'http://entry', resolvedAddr: 'http://real:7' });
    assert.strictEqual(r2.source, 'resolved');
    assert.strictEqual(r2.addr, 'http://real:8');
    assert.strictEqual(r2.changed, true);
    assert.strictEqual(r2.stale, true);
    // ③ 全失败 → 兜底用入口地址（播放时再暴露错误）
    const r3 = await ensureLiveAddr(mk({}), { addr: 'http://entry', resolvedAddr: 'http://real:7' });
    assert.strictEqual(r3.ok, true);
    assert.strictEqual(r3.source, 'entry');
    assert.strictEqual(r3.addr, 'http://entry');
    assert.strictEqual(r3.degraded, true);
    // ④ 只有入口（无缓存）→ 解析一次；缓存为空 → changed=true（表示"应回写缓存"）
    const r4 = await ensureLiveAddr(mk({ 'http://entry/health': { ok: true, host: 'entry' } }),
      { addr: 'http://entry', resolvedAddr: '' });
    assert.strictEqual(r4.source, 'resolved');
    assert.strictEqual(r4.addr, 'http://entry');
    assert.strictEqual(r4.changed, true);
    // 探测失败不抛（返回 null）
    assert.strictEqual(await probeAddr(async () => { throw new Error('x'); }, 'http://x', PROBE_MS), null);
  });

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
  test('reply: replyType 契约（1=视频 12=专栏；17 旧值回落 12）', () => {
    assert.strictEqual(replyType(1), 1);
    assert.strictEqual(replyType(12), 12);
    assert.strictEqual(replyType(17), 12); // 17 为旧文档值（实测 -404）→ 收敛到 12
    assert.strictEqual(replyType(undefined), 1);
    assert.strictEqual(replyType(0), 1);
    assert.strictEqual(replyType('12'), 1); // 非数字一律按视频回落
  });
  test('reply: fetchReplies 把 type 透传到请求（专栏 12 / 缺省视频 1）', async () => {
    let seenUrl = '';
    const c = createClient({
      get: async (u) => {
        seenUrl = u;
        return { statusCode: 200, body: JSON.stringify(fixture('reply.json')) };
      }
    });
    const r = await fetchReplies(c, 123, 1, 12);
    assert.strictEqual(r.ok, true, r.message);
    assert.ok(seenUrl.indexOf('type=12') > 0, seenUrl);
    assert.ok(seenUrl.indexOf('oid=123') > 0, seenUrl);
    let seen2 = '';
    const c2 = createClient({
      get: async (u) => {
        seen2 = u;
        return { statusCode: 200, body: JSON.stringify(fixture('reply.json')) };
      }
    });
    await fetchReplies(c2, 1, 1);
    assert.ok(seen2.indexOf('type=1') > 0, seen2);
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

  test('play_session: openVideo 全链（居中物理矩形 + UA/Referer + 配置帧率）', async () => {
    const w = fakePlayDeps();
    const r = await openVideo(w, probeItem);
    assert.strictEqual(r.ok, true);
    assert.strictEqual(r.session.qn, 16);
    assert.strictEqual(r.session.durationMs, 206000);
    const o = w.calls.open[0];
    assert.deepStrictEqual(o.rect, { x: 0, y: 174, width: 254, height: 452 });
    assert.strictEqual(o.userAgent, DEFAULT_UA);
    assert.strictEqual(o.referer, REFERER);
    assert.strictEqual(o.fps, HTML5_FPS); // 不锁死数值：锚定 openVideo 传递配置常量（v2.5.1 起 24）
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
    w.player.status = async () => ({
      ok: true,
      state: 'playing',
      frames: 10,
      audioBytes: 777,
      videoStallMs: 9001,
      gateWaitMs: 1500
    });
    const ok1 = await readStatus(w);
    assert.strictEqual(ok1.ok, true);
    assert.strictEqual(ok1.frames, 10);
    assert.strictEqual(ok1.audioBytes, 777);
    assert.strictEqual(ok1.videoStallMs, 9001, 'A/V 巡检字段透传');
    assert.strictEqual(ok1.gateWaitMs, 1500, '起播门时长透传');
    w.player.status = async () => ({ ok: true, state: 'error', error: 'HTTP error 403 Forbidden', frames: 0 });
    const bad = await readStatus(w);
    assert.strictEqual(bad.ok, false);
    assert.ok(bad.message.indexOf('403') >= 0);
    await closeSession(w);
    await closeSession(w);
    assert.strictEqual(w.calls.stop, 2);
    assert.strictEqual(w.calls.release, 2);
  });
  test('play_session: readStatus 透传 v2.1.0 强制重同步字段', async () => {
    const w = fakePlayDeps();
    w.player.status = async () => ({
      ok: true,
      state: 'playing',
      frames: 100,
      positionMs: 5000,
      avDriftMs: 900,
      resyncing: true,
      resyncCount: 3,
      avDriftNowMs: 812
    });
    const st = await readStatus(w);
    assert.strictEqual(st.resyncing, true, '重同步标志透传 → 页面显示"加载中…"');
    assert.strictEqual(st.resyncCount, 3);
    assert.strictEqual(st.avDriftNowMs, 812);
    // 缺省（老 native 无这些字段）→ false/0，页面不会误显示"加载中…"
    w.player.status = async () => ({ ok: true, state: 'playing', frames: 1 });
    const st2 = await readStatus(w);
    assert.strictEqual(st2.resyncing, false);
    assert.strictEqual(st2.resyncCount, 0);
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
  test('play_session: ensureMixinResult 透传失败细节（stage/code/message）', async () => {
    const w = fakePlayDeps();
    w.client.fetchWbiKeys = async () => ({
      ok: false,
      stage: 'api',
      code: -352,
      message: '风控(-352)：签名/UA 被识别异常'
    });
    const r = await ensureMixinResult(w);
    assert.strictEqual(r.ok, false);
    assert.strictEqual(r.stage, 'api');
    assert.strictEqual(r.code, -352);
    assert.ok(r.message.indexOf('-352') >= 0);
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
