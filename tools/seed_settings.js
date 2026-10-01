// 生成预置 storage 文件（等价于在笔上手动写 storage）
// 用法：node tools/seed_settings.js on|off [soak秒]
//   on  → bili_autotest={version:1,enabled:true}（真机自动跑完整链路自检）
//   on N → 追加 soak:N：自检通过后复播推荐第一个视频持续 N 秒（音画漂移长播验证）
// 产物：.deploy/preferences.json —— 由 tools/deploy.ps1 推到设备 storage 目录
//       （storage 值必须是字符串，所以整体再序列化一层）
const fs = require('fs');
const path = require('path');

const on = (process.argv[2] || 'on') !== 'off';
const soak = Number(process.argv[3]) || 0;

const seed = { version: 1, enabled: on };
if (on && soak > 0) seed.soak = Math.round(soak);

const payload = {
  bili_autotest: JSON.stringify(seed)
};

const outDir = path.join(__dirname, '..', '.deploy');
fs.mkdirSync(outDir, { recursive: true });
const out = path.join(outDir, 'preferences.json');
fs.writeFileSync(out, JSON.stringify(payload), 'utf8');
console.log('wrote ' + out + ' autotest=' + on);
