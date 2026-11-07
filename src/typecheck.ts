// 构建占位文件（不参与打包）。
//
// aiot-vue-cli 的 rollup 配置无条件挂了 @rollup/plugin-typescript，
// 项目里一个 .ts 都没有时插件会以 TS18003 "No inputs were found" 中断构建。
// 本文件不被任何模块 import，因此不会进 bundle，只用于给类型插件一个输入。
// 参考：YouDaoPen-SDK/templates/app/src/typecheck.ts 采用同样的做法。
export const TYPES_ONLY_PLACEHOLDER = 1;
