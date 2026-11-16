/*
 * 构建期开关（由 tools/build-wsl.sh 在 PENBILI_VARIANT=diag 时临时改写，
 * 打包结束自动还原；不要手工改完忘记还原）。
 *
 *   DIAG_ONLY = false  普通包：首页 + 自检页
 *   DIAG_ONLY = true   自检包：**只有** diag 一页，作为首页启动，
 *                      用来逐台 X7 验屏幕尺寸 / 平面 / 解码链路。
 *
 * 为什么不用运行时判断：自检包里 index 页根本没被打进 manifest，页面代码
 * 无从知道自己是不是唯一一页。构建期固定成常量最省事，也不会把
 * "$falcon.navTo('index')" 这种调用留到没有 index 的包里。
 */
export const DIAG_ONLY = false;
