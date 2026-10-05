// 重定向器测试（node tools/redirector.test.js）
// 覆盖：路径/query 透传、目标热改、__target 读写、未配置 503、非法目标 400、方法 405。
const assert = require('assert');
const http = require('http');
const fs = require('fs');
const os = require('os');
const path = require('path');

const { createRedirector } = require('./redirector.js');

let passed = 0;
let failed = 0;

function test(name, fn) {
  return Promise.resolve()
    .then(fn)
    .then(
      () => {
        passed++;
        console.log('  ok  ' + name);
      },
      (e) => {
        failed++;
        console.log('FAIL  ' + name + ' -> ' + (e && e.message));
      }
    );
}

// 不跟随重定向的请求，返回 {status, location, body}
function request(port, reqPath, method, body) {
  return new Promise((resolve, reject) => {
    const req = http.request(
      { host: '127.0.0.1', port: port, path: reqPath, method: method || 'GET' },
      (res) => {
        let data = '';
        res.on('data', (c) => (data += c));
        res.on('end', () => resolve({ status: res.statusCode, location: res.headers.location || '', body: data }));
      }
    );
    req.on('error', reject);
    if (body) req.write(body);
    req.end();
  });
}

async function main() {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'vp-redirector-'));
  const targetFile = path.join(dir, 'redirect-target.json');
  fs.writeFileSync(targetFile, JSON.stringify({ target: 'http://10.0.0.1:1111' }));

  const server = createRedirector({ port: 0, target: '', targetFile: targetFile });
  await new Promise((r) => server.listen(0, '127.0.0.1', r));
  const port = server.address().port;

  await test('GET 任意路径 -> 302 且 Location = target + 原路径', async () => {
    const r = await request(port, '/api/catalog');
    assert.strictEqual(r.status, 302);
    assert.strictEqual(r.location, 'http://10.0.0.1:1111/api/catalog');
  });

  await test('query 与深层路径原样透传', async () => {
    const r = await request(port, '/media/videos/a/b.mp4?x=1&y=2');
    assert.strictEqual(r.status, 302);
    assert.strictEqual(r.location, 'http://10.0.0.1:1111/media/videos/a/b.mp4?x=1&y=2');
  });

  await test('URL 编码路径不被改写', async () => {
    const r = await request(port, '/media/%E4%B8%AD%E6%96%87.mp4');
    assert.strictEqual(r.location, 'http://10.0.0.1:1111/media/%E4%B8%AD%E6%96%87.mp4');
  });

  await test('目标文件热改后立即生效（模拟外网端口变化）', async () => {
    fs.writeFileSync(targetFile, JSON.stringify({ target: 'http://10.0.0.1:2222/' }));
    const r = await request(port, '/api/health');
    assert.strictEqual(r.location, 'http://10.0.0.1:2222/api/health', '尾部斜杠应被规整');
  });

  await test('GET /__target 返回当前目标', async () => {
    const r = await request(port, '/__target');
    assert.strictEqual(r.status, 200);
    const obj = JSON.parse(r.body);
    assert.strictEqual(obj.ok, true);
    assert.strictEqual(obj.target, 'http://10.0.0.1:2222');
  });

  await test('POST /__target 改目标并落盘', async () => {
    const r = await request(port, '/__target', 'POST', JSON.stringify({ target: 'http://10.0.0.1:3333' }));
    assert.strictEqual(r.status, 200);
    const onDisk = JSON.parse(fs.readFileSync(targetFile, 'utf8'));
    assert.strictEqual(onDisk.target, 'http://10.0.0.1:3333');
    const r2 = await request(port, '/api/health');
    assert.strictEqual(r2.location, 'http://10.0.0.1:3333/api/health');
  });

  await test('POST /__target 非法目标 -> 400 且不落盘', async () => {
    const r = await request(port, '/__target', 'POST', JSON.stringify({ target: 'ftp://x/y' }));
    assert.strictEqual(r.status, 400);
    const onDisk = JSON.parse(fs.readFileSync(targetFile, 'utf8'));
    assert.strictEqual(onDisk.target, 'http://10.0.0.1:3333');
  });

  await test('POST /__target 非法 JSON -> 400', async () => {
    const r = await request(port, '/__target', 'POST', 'not-json');
    assert.strictEqual(r.status, 400);
  });

  await test('/__target 不支持的方法 -> 405', async () => {
    const r = await request(port, '/__target', 'DELETE');
    assert.strictEqual(r.status, 405);
  });

  await test('未配置目标 -> 503（不崩溃）', async () => {
    const emptyFile = path.join(dir, 'empty.json');
    const s2 = createRedirector({ port: 0, target: '', targetFile: emptyFile });
    await new Promise((r) => s2.listen(0, '127.0.0.1', r));
    const p2 = s2.address().port;
    const r = await request(p2, '/api/catalog');
    assert.strictEqual(r.status, 503);
    await new Promise((r2) => s2.close(r2));
  });

  await test('目标文件损坏时回退命令行 --target', async () => {
    const badFile = path.join(dir, 'bad.json');
    fs.writeFileSync(badFile, '{ broken');
    const s3 = createRedirector({ port: 0, target: 'http://fallback:9', targetFile: badFile });
    await new Promise((r) => s3.listen(0, '127.0.0.1', r));
    const p3 = s3.address().port;
    const r = await request(p3, '/api/catalog');
    assert.strictEqual(r.location, 'http://fallback:9/api/catalog');
    await new Promise((r2) => s3.close(r2));
  });

  await new Promise((r) => server.close(r));

  console.log('');
  console.log('passed=' + passed + ' failed=' + failed);
  process.exit(failed ? 1 : 0);
}

main().catch((e) => {
  console.error('test runner error:', e);
  process.exit(1);
});
