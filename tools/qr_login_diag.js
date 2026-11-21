// 扫码登录票据流程真相采集 v2：过期自动换码（同文件重写，最多3轮）
const QR = require('qrcode');
const UA = 'Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/124.0.0.0 Safari/537.36';
const REF = 'https://www.bilibili.com/';
const H = { 'User-Agent': UA, 'Referer': REF };
const OUT = process.argv[2];
const mask = (v) => (v && v.length > 8 ? v.slice(0, 8) + '…(' + v.length + ')' : v ? '(len' + v.length + ')' : '');
function describeUrl(u) {
  try {
    const x = new URL(u);
    const params = [...x.searchParams.entries()].map(([k, v]) => k + '=' + mask(v)).join('&');
    return x.origin + x.pathname + (params ? '?' + params : '');
  } catch (e) { return '(relative) ' + String(u).slice(0, 80); }
}
(async () => {
  let successData = null;
  for (let round = 1; round <= 3 && !successData; round++) {
    const gen = await (await fetch('https://passport.bilibili.com/x/passport-login/web/qrcode/generate', { headers: H })).json();
    if (gen.code !== 0 || !gen.data) { console.log('GENERATE_FAIL round' + round); return; }
    const key = gen.data.qrcode_key;
    await QR.toFile(OUT, gen.data.url, { width: 420, margin: 3, errorCorrectionLevel: 'M' });
    console.log('QR_READY round' + round + ' ' + OUT + ' (180s 窗口)');
    for (let i = 0; i < 85; i++) {
      await new Promise((r) => setTimeout(r, 2000));
      const pol = await (await fetch('https://passport.bilibili.com/x/passport-login/web/qrcode/poll?qrcode_key=' + key, { headers: H })).json();
      const biz = pol.data && pol.data.code;
      if (biz === 86101) continue;
      if (biz === 86090) { if (i % 4 === 0) console.log('SCANNED_WAIT_CONFIRM'); continue; }
      if (biz === 0) { successData = pol.data; break; }
      console.log('POLL_BIZ round' + round + ' ' + biz + ' ' + (pol.data && pol.data.message));
      break; // 86038 等 → 换下一轮
    }
    if (!successData && round < 3) console.log('EXPIRED → 新码已覆盖同一文件，请重新打开该文件扫码');
  }
  if (!successData) { console.log('GIVE_UP_3_ROUNDS'); return; }
  console.log('SUCCESS_CODE_0');
  const su = successData.url;
  console.log('success.url => ' + describeUrl(su));
  console.log('data keys => ' + Object.keys(successData).join(','));
  if (successData.refresh_token) console.log('refresh_token present ' + mask(successData.refresh_token));
  if (successData.timestamp) console.log('timestamp=' + successData.timestamp);
  let cur = su;
  for (let hop = 0; hop < 6; hop++) {
    let res;
    try {
      res = await fetch(cur, { headers: H, redirect: 'manual' });
    } catch (e) { console.log('hop' + hop + ' FETCH_ERR ' + String(e).slice(0, 140)); break; }
    const sc = typeof res.headers.getSetCookie === 'function' ? res.headers.getSetCookie() : [];
    const loc = res.headers.get('location');
    console.log('hop' + hop + ' status=' + res.status +
      (loc ? ' location=' + describeUrl(loc.startsWith('http') ? loc : new URL(loc, cur).toString()) : '') +
      ' set-cookie=[' + sc.map((c) => { const p = c.split(';')[0].split('='); return p[0] + '=' + mask(p.slice(1).join('=')); }).join('; ') + ']');
    if (res.status >= 300 && res.status < 400 && loc) {
      cur = loc.startsWith('http') ? loc : new URL(loc, cur).toString();
      continue;
    }
    const t = await res.text();
    try {
      const j = JSON.parse(t);
      console.log('hop' + hop + ' BODY_JSON keys=' + Object.keys(j).join(',') +
        (j.data && typeof j.data === 'object' ? ' dataKeys=' + Object.keys(j.data).join(',') : '') + ' code=' + j.code);
      if (j.data && j.data.cookie_info) console.log('  cookie_info names=' + j.data.cookie_info.cookies.map((c) => c.name).join(','));
    } catch (e) { console.log('hop' + hop + ' BODY_HTML len=' + t.length + ' head=' + t.slice(0, 160).replace(/\s+/g, ' ')); }
    break;
  }
  console.log('DIAG_DONE');
})().catch((e) => { console.log('FATAL ' + e.message); });