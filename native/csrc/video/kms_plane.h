/*
 * kms_plane.h - KMS 平面只读监测
 *
 * ============================ 为什么只读 ============================
 * 真机实测（.tmp/m_plane_recover2.sh）：只要**进程活着**，
 *   hal-screen off -> 平面 ACTIVE=0
 *   hal-screen on  -> 平面自动回到 ACTIVE=1，且视频继续出帧（vid_changes=74/2s）
 * 也就是说平面生命周期完全由内核/DRM 管理，**不需要重建管线、不需要重新
 * set_state**。之前「唤醒后不出画」的真正原因是旧实现把 gst/ffmpeg 子进程
 * 弄死了，不是平面需要重建。
 *
 * 所以本模块只做一件事：定期读 /sys/kernel/debug/dri/0/summary，
 * 判断播放用的那个 plane（Esmart1-win0）是否 ACTIVE，把结果交给 status()。
 * 判断异常时只记日志 + 置 lastError，绝不自动重建（重建由 JS 层决定）。
 *
 * ============================ 必须循环 read ============================
 * 该 debugfs 文件单次 read() 只返回前 576 字节（只到 Esmart0-win0），
 * 必须循环 read() 直到返回 <= 0 才能拿到 Esmart1-win0 那一段。
 * 用 cat 能看全是因为 shell 的 cat 自己循环读。
 */
#ifndef PENBILI_PB2_KMS_PLANE_H
#define PENBILI_PB2_KMS_PLANE_H

/* 播放用的平面窗口名（X6 Pro 上视频走 Esmart1-win0） */
#define PB2_PLANE_WIN "Esmart1-win0"

/*
 * 读一次 summary，判断 plane 状态。
 * 返回 1 = ACTIVE 且有有效地址（addr != 0）
 * 返回 0 = 存在但非 ACTIVE（或地址为 0）
 * 返回 -1 = 读不到 / 找不到该窗口
 *
 * 这个函数会 open/read/close，耗时约 200~600us（debugfs 生成内容有开销），
 * 因此**只能在 worker 线程调用**（周期 2s），不能在 status() 里调。
 */
int pb2_plane_query(void);

/*
 * 上一次 pb2_plane_query() 的结果缓存（原子）。
 *
 * 【现状】worker 把结果同时写了 stats 快照（pb2_stats_patch_plane），
 * status() 读的是 st.planeActive，**并不读这两个函数**；它们的调用点为 0，
 * 留着是为了将来需要「不经过 stats 直接取平面状态」时用。
 */
int pb2_plane_cached(void);
void pb2_plane_set_cached(int active);

#endif /* PENBILI_PB2_KMS_PLANE_H */
