// 生成预置 storage 文件（等价于在笔上手动写 storage）
// 用法：node tools/seed_settings.js on|off
//   on  → bili_autotest={version:1,enabled:true}（真机自动跑完整链路自检）
// 产物：.deploy/preferences.json —— 由 tools/deploy.ps1 推到设备 storage 目录
//       （storage 值必须是字符串，所以整体再序列化一层）
const fs = require('fs');
const path = require('path');

const on = (process.argv[2] || 'on') !== 'off';

const payload = {
  bili_autotest: JSON.stringify({ version: 1, enabled: on })
};

const outDir = path.join(__dirname, '..', '.deploy');
fs.mkdirSync(outDir, { recursive: true });
const out = path.join(outDir, 'preferences.json');
fs.writeFileSync(out, JSON.stringify(payload), 'utf8');
console.log('wrote ' + out + ' autotest=' + on);
