// 扫码登录 + 二维码渲染 用例（QR 与 npm qrcode 参考矩阵逐位对拍；接口吃真 fixtures）
const assert = require('assert');
const fsx = require('fs');
const pathx = require('path');

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
  const fixture = (n) => JSON.parse(fsx.readFileSync(pathx.join(__dirname, '..', 'api-mock', 'fixtures', n), 'utf8'));
  const { encodeQr, rowRuns, pickVersion, byteCapacity } = await import('../src/services/bili/qrcode.js');
  const qrRefs = fixture('qr_reference.json').refs;
  const {
    parseGenerate,
    parsePoll,
    parseNavProfile,
    extractCookies,
    QR_STATE,
    loginExpired,
    POLL_INTERVAL_MS,
    POLL_MAX_ROUNDS,
    POLL_ACCEPT_CODES,
    TV_GEN_URL,
    TV_POLL_URL,
    tvForm,
    appSign,
    APPKEY_TV
  } = await import('../src/services/bili/qrlogin.js');
  const { normalizeSession, normalizeLogin, SESSION_SCHEMA_VERSION } = await import('../src/services/storage.js');
  const { createClient, cookieFromSession, DEFAULT_UA, REFERER } = await import('../src/services/bili/client.js');

  function assertRowsEqual(r, ref, label) {
    assert.strictEqual(r.ok, true, label + ': ' + (r.reason || ''));
    assert.strictEqual(r.size, ref.size, label + ': size');
    for (let y = 0; y < ref.size; y++) {
      const mine = r.grid[y].map((v) => (v ? '1' : '0')).join('');
      if (mine !== ref.rows[y]) {
        // 定位首个差异位，便于迭代
        let d = 0;
        while (d < ref.size && mine[d] === ref.rows[y][d]) d++;
        assert.fail(label + ': row ' + y + ' col ' + d + ' mine=' + mine[d] + ' ref=' + ref.rows[y][d]);
      }
    }
  }

  // ---------------- qrcode：解码级金标准（jsQR，devDependency）----------------
  // 说明：与 npm qrcode 同参数下存在合法实现自由度（RS 已数学自证整除、jsQR 双方可解），
  // 因此以“能否被独立解码器解回原文”为断言 —— 能解=能扫，比逐位对拍更强。
  const jsQR = require('jsqr');
  // 矩阵 → RGBA（带 quiet zone=4，标准要求且渲染同样保留白边）
  function matrixToRgba(get, size, quiet) {
    const n = size + quiet * 2;
    const d = new Uint8ClampedArray(n * n * 4);
    for (let y = 0; y < n; y++) {
      for (let x = 0; x < n; x++) {
        const inRange = x >= quiet && y >= quiet && x < quiet + size && y < quiet + size;
        const dark = inRange ? get(x - quiet, y - quiet) : 0;
        const v = dark ? 0 : 255;
        const o = (y * n + x) * 4;
        d[o] = d[o + 1] = d[o + 2] = v;
        d[o + 3] = 255;
      }
    }
    return { data: d, n: n };
  }
  function decodeGrid(grid, size) {
    const m = matrixToRgba((x, y) => grid[y][x], size, 4);
    return jsQR(m.data, m.n, m.n);
  }
  test('qrcode: 扫码 URL 可被 jsQR 解回原文（指定 version+mask0）', () => {
    const ref = qrRefs[0];
    const ver = (ref.size - 17) / 4;
    const r = encodeQr(ref.text, { version: ver, mask: 0 });
    assert.strictEqual(r.ok, true, r.reason);
    const dec = decodeGrid(r.grid, r.size);
    assert.ok(dec, 'jsQR 解不出');
    assert.strictEqual(dec.data, ref.text);
  });
  test('qrcode: 参考矩阵同环境可解（校准解码器与 quiet zone 参数）', () => {
    const ref = qrRefs[0];
    const rows = ref.rows.map((s) => s.split('').map((c) => (c === '1' ? 1 : 0)));
    const dec = decodeGrid(rows, ref.size);
    assert.ok(dec, '参考矩阵 jsQR 解不出（解码环境异常）');
    assert.strictEqual(dec.data, ref.text);
  });
  test('qrcode: 自动选版 + 长文本/UTF-8/中文 均可解回', () => {
    const texts = [
      'https://account.bilibili.com/h5/account-h5/auth/scan-web?navhide=1&callback=close&qrcode_key=' + 'a1b2c3d4'.repeat(4) + '&from=',
      'HELLO-BILI-254-长文本自动选版验证'
    ];
    for (const t of texts) {
      const r = encodeQr(t, { mask: 0 });
      assert.strictEqual(r.ok, true, t.slice(0, 20));
      const dec = decodeGrid(r.grid, r.size);
      assert.ok(dec, '解不出: ' + t.slice(0, 30));
      assert.strictEqual(dec.data, t);
    }
  });
  test('qrcode: 选版边界与超容量拒绝', () => {
    assert.strictEqual(pickVersion(byteCapacity(1)), 1);
    assert.strictEqual(pickVersion(byteCapacity(1) + 1), 2);
    const big = encodeQr('x'.repeat(400));
    assert.strictEqual(big.ok, false);
  });
  test('qrcode: run-length 渲染数据可逆（runs → 矩阵 == grid）', () => {
    const r = encodeQr(qrRefs[0].text, { mask: 0 });
    assert.strictEqual(r.ok, true);
    const runs = rowRuns(r.grid);
    assert.strictEqual(runs.length, r.size);
    const rebuilt = r.grid.map(() => new Array(r.size).fill(0));
    runs.forEach((row, y) =>
      row.forEach(([x, len]) => {
        for (let k = 0; k < len; k++) rebuilt[y][x + k] = 1;
      })
    );
    assert.deepStrictEqual(rebuilt, r.grid);
    // run 的 x+len 不越界且不重叠
    runs.forEach((row, y) => {
      let prevEnd = 0;
      row.forEach(([x, len]) => {
        assert.ok(x >= prevEnd, 'row ' + y + ' run 重叠');
        assert.ok(x + len <= r.size, 'row ' + y + ' run 越界');
        prevEnd = x + len;
      });
    });
  });
  test('qrcode: 掩码选优（不传 mask 也产出合法矩阵）+ 结构特征', () => {
    const r = encodeQr('https://passport.bilibili.com/x');
    assert.strictEqual(r.ok, true);
    // 三个 finder 左上角7x7
    for (const [row, col] of [[0, 0], [0, r.size - 7], [r.size - 7, 0]]) {
      assert.strictEqual(r.grid[row][col], 1, 'finder 角');
      assert.strictEqual(r.grid[row + 3][col + 3], 1, 'finder 心');
    }
    // timing 第6行交替
    for (let i = 8; i < r.size - 8; i++) assert.strictEqual(r.grid[6][i], i % 2 === 0 ? 1 : 0, 'timing@' + i);
  });

  // ---------------- qrlogin（TV 变体：cookie 在响应 body 的 cookie_info）----------------
  test('qrlogin: appSign 与文档示例逐字符一致（签名算法锚点）', () => {
    assert.strictEqual(appSign({ appkey: APPKEY_TV, local_id: '0', ts: '0' }), 'e134154ed6add881d28fbdf68653cd9c');
    const p = { appkey: APPKEY_TV, local_id: '0', ts: '1700000000' };
    const s = appSign(p);
    assert.ok(/^[0-9a-f]{32}$/.test(s));
    // sign 参数必须排除在自身计算之外
    assert.strictEqual(appSign(Object.assign({}, p, { sign: s })), s, 'sign 应排除自身');
    // 参数顺序不影响签名（内部排序）
    assert.strictEqual(appSign({ ts: p.ts, local_id: p.local_id, appkey: p.appkey }), s);
  });
  test('qrlogin: tvForm 排序+encode+含 sign（与签名逐字一致）', () => {
    const f = tvForm({ auth_code: 'a'.repeat(32) });
    const parts = f.split('&');
    const keys = parts.map((x) => x.split('=')[0]);
    assert.deepStrictEqual(keys, keys.slice().sort(), 'key 必须有序');
    assert.ok(keys.indexOf('appkey') >= 0 && keys.indexOf('auth_code') >= 0 && keys.indexOf('sign') >= 0 && keys.indexOf('ts') >= 0);
    const sign = (f.match(/sign=([0-9a-f]{32})/) || [])[1];
    const ts = (f.match(/ts=(\d+)/) || [])[1];
    const local = (f.match(/local_id=(\d+)/) || [])[1];
    assert.ok(sign && ts && local);
    assert.strictEqual(sign, appSign({ appkey: APPKEY_TV, auth_code: 'a'.repeat(32), local_id: local, ts }));
  });
  test('qrlogin: generate TV 真 fixture → url+32hex auth_code（postForm 注入）', async () => {
    const fx = fixture('tv_generate.json');
    let seenUrl = '';
    let seenBody = '';
    const c = createClient({
      post: async (u, b) => {
        seenUrl = u;
        seenBody = b;
        return { statusCode: 200, body: JSON.stringify(fx) };
      }
    });
    const r = parseGenerate(await c.postForm(TV_GEN_URL, tvForm(), { acceptCodes: [0] }));
    assert.strictEqual(r.ok, true, r.message);
    assert.ok(/^[0-9a-f]{32}$/.test(r.authCode));
    assert.ok(r.url.indexOf('https://passport.bilibili.com/x/passport-tv-login/h5/qrcode/auth?auth_code=') === 0, r.url);
    assert.strictEqual(seenUrl, TV_GEN_URL);
    assert.ok(seenBody.indexOf('sign=') >= 0);
  });
  test('qrlogin: poll waiting 真 fixture(86039) → WAITING', async () => {
    const fx = fixture('tv_poll_waiting.json');
    const c = createClient({ post: async () => ({ statusCode: 200, body: JSON.stringify(fx) }) });
    const r = parsePoll(await c.postForm(TV_POLL_URL, tvForm({ auth_code: 'b'.repeat(32) }), { acceptCodes: POLL_ACCEPT_CODES }));
    assert.strictEqual(r.state, QR_STATE.WAITING, JSON.stringify(r));
  });
  test('qrlogin: poll 成功 fixture → cookie_info 提取（SESSDATA/有效期/mid）', async () => {
    const fx = fixture('tv_poll_ok.json');
    const c = createClient({ post: async () => ({ statusCode: 200, body: JSON.stringify(fx) }) });
    const r = parsePoll(await c.postForm(TV_POLL_URL, tvForm({ auth_code: 'c'.repeat(32) }), { acceptCodes: POLL_ACCEPT_CODES }));
    assert.strictEqual(r.state, QR_STATE.OK, JSON.stringify(r));
    assert.strictEqual(r.cookies.SESSDATA, 'SESS_PLACEHOLDER_222CHARS');
    assert.strictEqual(r.cookies.bili_jct, 'JCT_PLACEHOLDER');
    assert.strictEqual(r.cookies.DedeUserID, '293793435');
    assert.strictEqual(r.cookies.expiresAt, 1790140171000);
    assert.strictEqual(r.mid, 293793435);
    assert.strictEqual(r.refreshToken, 'RT_PLACEHOLDER');
  });
  test('qrlogin: 成功态缺 SESSDATA → 防御且只露 name 列表', () => {
    const r = parsePoll({
      ok: true,
      code: 0,
      data: { cookie_info: { cookies: [{ name: 'bili_jct', value: 'SECRET-X' }, { name: 'sid', value: 'SECRET-Y' }] }, mid: 1 }
    });
    assert.strictEqual(r.state, QR_STATE.ERROR);
    assert.strictEqual(r.stage, 'cookie');
    assert.ok(r.message.indexOf('SESSDATA') >= 0);
    assert.ok(r.message.indexOf('names=') >= 0);
    assert.ok(r.message.indexOf('SECRET') < 0, '不得泄漏值');
  });
  test('qrlogin: 86090/86038/-3/传输失败 状态映射（状态码在外层 code）', () => {
    assert.strictEqual(parsePoll({ ok: true, code: 86090, message: 'x' }).state, QR_STATE.SCANNED);
    assert.strictEqual(parsePoll({ ok: true, code: 86038, message: 'y' }).state, QR_STATE.EXPIRED);
    const unk = parsePoll({ ok: false, stage: 'api', code: -3, message: '签名错误' });
    assert.strictEqual(unk.state, QR_STATE.ERROR);
    assert.ok(unk.message.indexOf('-3') >= 0);
    assert.strictEqual(parsePoll({ ok: false, stage: 'transport', message: 'NET_TIMEOUT' }).state, QR_STATE.ERROR);
    assert.deepStrictEqual(POLL_ACCEPT_CODES, [0, 86039, 86090, 86038]);
  });
  test('qrlogin: extractCookies cookieInfo 边界（null/无SESSDATA/正常）', () => {
    assert.strictEqual(extractCookies(null).ok, false);
    const r = extractCookies({ cookies: [{ name: 'sid', value: 's' }] });
    assert.strictEqual(r.ok, false);
    assert.strictEqual(r.reason, 'no_SESSDATA_in_cookie_info');
    assert.ok(r.keys.indexOf('sid') >= 0);
    const ok2 = extractCookies({ cookies: [{ name: 'SESSDATA', value: 'v', expires: 100 }] });
    assert.strictEqual(ok2.ok, true);
    assert.strictEqual(ok2.cookies.expiresAt, 100000);
  });
  test('qrlogin: nav 登录 fixture → 档案；匿名 fixture → unauthorized', async () => {
    const loginFx = fixture('nav_login.json');
    const c1 = createClient({ get: async () => ({ statusCode: 200, body: JSON.stringify(loginFx) }) });
    const p = parseNavProfile(await c1.request('/x/web-interface/nav'));
    assert.strictEqual(p.ok, true, p.message);
    assert.strictEqual(p.mid, 293793435);
    assert.strictEqual(p.uname, '社会易姐QwQ');
    assert.strictEqual(p.level, 6);
    assert.strictEqual(p.vipType, 2);

    const anonFx = fixture('nav_anon.json');
    const c2 = createClient({ get: async () => ({ statusCode: 200, body: JSON.stringify(anonFx) }) });
    const q = parseNavProfile(await c2.request('/x/web-interface/nav', null, { acceptCodes: [-101] }));
    assert.strictEqual(q.ok, false);
    assert.strictEqual(q.stage, 'unauthorized');
  });
  test('qrlogin: loginExpired 边界（提前 60s / 未知期 / 无凭证）', () => {
    const now = 1700000000000;
    assert.strictEqual(loginExpired({ SESSDATA: '' }, now), true);
    assert.strictEqual(loginExpired(null), true);
    assert.strictEqual(loginExpired({ SESSDATA: 'x', expiresAt: 0 }, now), false);
    assert.strictEqual(loginExpired({ SESSDATA: 'x', expiresAt: now + 3600000 }, now), false);
    assert.strictEqual(loginExpired({ SESSDATA: 'x', expiresAt: now + 30000 }, now), true); // 30s 内 → 过期
  });
  test('qrlogin: 轮询节奏常量', () => {
    assert.strictEqual(POLL_INTERVAL_MS, 2000);
    assert.ok(POLL_MAX_ROUNDS >= 80 && POLL_MAX_ROUNDS <= 90);
  });

  // ---------------- session v2 / cookie ----------------
  test('session: v1 → v2 迁移（触点保留，login=null）', () => {
    const s = normalizeSession({ version: 1, buvid3: 'B3', buvid4: 'B4', updatedAt: 9 });
    assert.strictEqual(s.version, SESSION_SCHEMA_VERSION);
    assert.strictEqual(s.version, 2);
    assert.strictEqual(s.buvid3, 'B3');
    assert.strictEqual(s.buvid4, 'B4');
    assert.strictEqual(s.updatedAt, 9);
    assert.strictEqual(s.login, null);
  });
  test('session: v2 完整登录保真 + 脏 login 丢弃', () => {
    const s = normalizeSession({
      version: 2,
      buvid3: 'B3',
      buvid4: '',
      updatedAt: 1,
      login: { SESSDATA: 'se', bili_jct: 'jct', DedeUserID: '42', mid: 42, uname: 'tester', level: 4, face: 'http://i1.hdslb.com/bfs/face/a.jpg', expiresAt: 1766000000000, loggedAt: 123 }
    });
    assert.strictEqual(s.login.SESSDATA, 'se');
    assert.strictEqual(s.login.uname, 'tester');
    assert.strictEqual(s.login.level, 4);
    assert.strictEqual(s.login.expiresAt, 1766000000000);
    // 脏 login（无 SESSDATA）→ null
    assert.strictEqual(normalizeSession({ version: 2, login: { bili_jct: 'x' } }).login, null);
    assert.strictEqual(normalizeLogin({}), null);
    // 版本不认识 → 空
    assert.strictEqual(normalizeSession({ version: 99, buvid3: 'x' }).buvid3, '');
  });
  test('session: cookieFromSession 拼装（v2 全量 / v1 只触点 / 空）', () => {
    const full = normalizeSession({
      version: 2,
      buvid3: 'B3',
      buvid4: 'B4',
      login: { SESSDATA: 'se1', bili_jct: 'jct1', DedeUserID: '7', 'DedeUserID__ckMd5': 'md5x' }
    });
    const c = cookieFromSession(full);
    assert.ok(c.indexOf('buvid3=B3') >= 0);
    assert.ok(c.indexOf('buvid4=B4') >= 0);
    assert.ok(c.indexOf('DedeUserID=7') >= 0);
    assert.ok(c.indexOf('DedeUserID__ckMd5=md5x') >= 0);
    assert.ok(c.indexOf('SESSDATA=se1') >= 0);
    assert.ok(c.indexOf('bili_jct=jct1') >= 0);
    // 顺序稳定：buvid 在前
    assert.ok(c.indexOf('buvid3') < c.indexOf('SESSDATA'));
    // 旧 v1 形态（兼容既有测试）
    assert.strictEqual(cookieFromSession({ version: 1, buvid3: 'B3-1', buvid4: 'B4-1' }), 'buvid3=B3-1; buvid4=B4-1');
    assert.strictEqual(cookieFromSession({ version: 2, buvid3: 'b' }), 'buvid3=b');
    assert.strictEqual(cookieFromSession(null), '');
  });
  test('session: 登录后 apiHeaders 自动带 Cookie（client.setCookie 链路）', async () => {
    let seen = null;
    const c = createClient({
      get: async (u, o) => {
        seen = o.headers;
        return { statusCode: 200, body: '{"code":0,"data":{}}' };
      }
    });
    const session = normalizeSession({ version: 2, buvid3: 'B3', login: { SESSDATA: 'se', bili_jct: 'j' } });
    c.setCookie(cookieFromSession(session));
    await c.request('/x/web-interface/nav');
    assert.strictEqual(seen['User-Agent'], DEFAULT_UA);
    assert.strictEqual(seen.Referer, REFERER);
    assert.ok(seen.Cookie.indexOf('SESSDATA=se') >= 0);
    assert.ok(seen.Cookie.indexOf('buvid3=B3') >= 0);
  });
}

main().then(
  async () => {
    try {
      await Promise.all(pending);
    } catch (e) {
      /* failed 已计数 */
    }
    console.log('\n' + passed + ' passed, ' + failed + ' failed');
    process.exit(failed > 0 ? 1 : 0);
  },
  (e) => {
    console.error(e);
    process.exit(1);
  }
);
