// 稳定入口重定向器：给"外网端口/IP 会变"的家庭宽带提供一个固定地址。
//
// 客户端只配置这个重定向器的地址（例如 VPS 上的 http://entry.example.com），
// 它把任意请求 302 到当前真实地址 + 原路径 + 原 query。
//
// 关键点：目标地址从文件热读取，端口变了只要改文件，**不用重启重定向器**。
//
// 用法：
//   node tools/redirector.js --port 8099 --target http://192.168.5.199:8080
//   node tools/redirector.js --port 8099 --target-file .deploy/redirect-target.json
//   # 之后改目标（端口变了）：
//   echo '{"target":"http://192.168.5.199:9000"}' > .deploy/redirect-target.json
//
// 额外接口：
//   GET  /__target       查看当前目标
//   POST /__target       直接改目标（body: {"target":"http://host:port"}）
const http = require('http');
const fs = require('fs');
const path = require('path');

function parseArgs(argv) {
  const out = { port: 8099, target: '', targetFile: '' };
  for (let i = 2; i < argv.length; i++) {
    const a = argv[i];
    if (a === '--port') out.port = parseInt(argv[++i], 10);
    else if (a === '--target') out.target = argv[++i];
    else if (a === '--target-file') out.targetFile = argv[++i];
  }
  if (!out.targetFile) {
    out.targetFile = path.join(__dirname, '..', '.deploy', 'redirect-target.json');
  }
  return out;
}

function readTarget(opts) {
  // 文件优先（可热改），其次命令行
  try {
    const raw = fs.readFileSync(opts.targetFile, 'utf8');
    const parsed = JSON.parse(raw);
    if (parsed && typeof parsed.target === 'string' && parsed.target) return parsed.target.replace(/\/+$/, '');
  } catch (e) {
    /* 文件不存在或损坏：回退命令行参数 */
  }
  return (opts.target || '').replace(/\/+$/, '');
}

function writeTarget(opts, target) {
  fs.mkdirSync(path.dirname(opts.targetFile), { recursive: true });
  fs.writeFileSync(opts.targetFile, JSON.stringify({ target: target, updatedAt: new Date().toISOString() }, null, 2));
}

const opts = parseArgs(process.argv);

if (opts.target) {
  // 首次启动把命令行目标写进文件，之后由文件驱动
  try {
    if (!fs.existsSync(opts.targetFile)) writeTarget(opts, opts.target.replace(/\/+$/, ''));
  } catch (e) {
    /* 忽略 */
  }
}

function sendJson(res, code, obj) {
  const body = JSON.stringify(obj);
  res.writeHead(code, { 'Content-Type': 'application/json; charset=utf-8', 'Content-Length': Buffer.byteLength(body) });
  res.end(body);
}

// 可被测试 require：只在直接运行时才监听端口
function createRedirector(opts) {
  return http.createServer((req, res) => {
    const started = Date.now();
    const target = readTarget(opts);

    if (req.url === '/__target') {
      if (req.method === 'GET') {
        sendJson(res, 200, { ok: !!target, target: target, targetFile: opts.targetFile });
        return;
      }
      if (req.method === 'POST' || req.method === 'PUT') {
        let body = '';
        req.on('data', (c) => (body += c));
        req.on('end', () => {
          try {
            const parsed = JSON.parse(body || '{}');
            const t = String(parsed.target || '').replace(/\/+$/, '');
            if (!/^https?:\/\/[^\s]+$/i.test(t)) {
              sendJson(res, 400, { ok: false, error: 'target 必须是 http(s) 地址' });
              return;
            }
            writeTarget(opts, t);
            console.log('[redirector] 目标已更新: ' + t);
            sendJson(res, 200, { ok: true, target: t });
          } catch (e) {
            sendJson(res, 400, { ok: false, error: 'body 必须是 {"target":"..."}' });
          }
        });
        return;
      }
      sendJson(res, 405, { ok: false, error: 'method_not_allowed' });
      return;
    }

    if (!target) {
      sendJson(res, 503, { ok: false, error: 'redirector 未配置 target', targetFile: opts.targetFile });
      return;
    }

    const location = target + req.url;
    // 302 而不是 301：地址会变，不能让客户端/中间层把旧地址长期缓存
    res.writeHead(302, { Location: location, 'Cache-Control': 'no-store', 'Content-Length': 0 });
    res.end();
    console.log(
      '[redirector] ' + req.method + ' ' + req.url + ' -> 302 ' + location + ' (' + (Date.now() - started) + 'ms)'
    );
  });
}

function startServer(opts) {
  const server = createRedirector(opts);
  server.listen(opts.port, '0.0.0.0', () => {
    console.log('稳定入口重定向器已启动');
    console.log('  监听: 0.0.0.0:' + opts.port);
    console.log('  目标: ' + (readTarget(opts) || '(未配置)'));
    console.log('  目标文件（可热改，无需重启）: ' + opts.targetFile);
    console.log('  客户端服务器地址就填这个重定向器的地址（不含路径）');
  });
  process.on('SIGINT', () => server.close(() => process.exit(0)));
  process.on('SIGTERM', () => server.close(() => process.exit(0)));
  return server;
}

module.exports = { createRedirector, startServer, readTarget, writeTarget, parseArgs };

if (require.main === module) {
  const opts = parseArgs(process.argv);
  if (opts.target) {
    // 首次启动把命令行目标写进文件，之后由文件驱动
    try {
      if (!fs.existsSync(opts.targetFile)) writeTarget(opts, opts.target.replace(/\/+$/, ''));
    } catch (e) {
      /* 忽略 */
    }
  }
  startServer(opts);
}
