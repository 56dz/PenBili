/*
 * dm_overlay.h - 弹幕烧帧（NV12 软合成）
 *
 * ======================== 为什么必须烧进视频帧 ========================
 * 视频走 GStreamer + Rockchip MPP 硬解 + kmssink 直送 DRM 平面，像素格式
 * NV12。视频平面的 KMS zpos 高于 Falcon UI，**任何 UI 文字都盖不到画面上**
 * （真机实测：弹幕层/评论面板/下拉控制中心全部被视频平面盖住）。所以弹幕
 * 只能是「改像素」这一条路，不能是「叠一层」。
 *
 * 挂钩点是 kmssink 本体 sink pad 上的 buffer probe：那里拿到的 buffer 与
 * render-rectangle 等大（真机 266x472 / 240x427 等），已经过 videoscale，
 * 改完就上屏，不会被再缩放一次。
 *
 * ============================ 与 GStreamer 的关系 ============================
 * **本模块完全不认识 GStreamer**。入口收的是裸 Y/UV 指针，调用方负责
 * gst_buffer_extract / gst_buffer_fill。这样做的原因：
 *   - 铁律：绝不能用 GstMapInfo。third_party/gst_min.h 里只有类型的不完整
 *     声明，自己抄一份布局去骗结构体偏移，一次 ABI 变化就是栈越界写。
 *   - 只有 extract/fill 这一条通道是安全的（session.c 的 scrub_edge_green()
 *     已经跑了很久）。
 *   - 把像素逻辑抽成纯 C 之后，整段算法可以在开发机上用 Python 复刻验证，
 *     不需要真机也不需要链接参数（见 .tmp/dm_blend_check.png 的来历）。
 *
 * ============================ 线程与生命周期 ============================
 * 全部函数**只在 worker 线程调用**（GStreamer 的 buffer probe 就在 worker
 * 线程里跑）。模块内部没有任何锁 —— 这不是疏忽，是硬性要求：probe 里加锁
 * 会在解码线程上引入不可控的抖动，而这里的所有状态本来就只有 worker 碰。
 *
 * 初始化失败的语义是「弹幕不可用，但播放照常」：dm_init() 返回 -1 之后，
 * dm_render() 永远第一条语句返回 0，调用方可以完全不再判断。
 */
#ifndef PENBILI_DM_OVERLAY_H
#define PENBILI_DM_OVERLAY_H

/*
 * 弹幕显示模式（与 JS 侧 DM_MODES 一一对应：off / full / half / quarter）。
 *
 * 用 #define 而不是 enum：调用方只有「值比较」（session.c 里的
 * `mode == PB2_DM_OFF`、`mode > PB2_DM_QUARTER`），两种写法生成的代码完全
 * 一样；但 #define 常量能进 #if，将来若要在预处理期裁剪弹幕代码不用改这里。
 */
#define PB2_DM_OFF     0   /* 关闭：dm_render 第一条语句就返回，零开销 */
#define PB2_DM_FULL    1   /* 整帧可用 */
#define PB2_DM_HALF    2   /* 只有上方一半能出弹幕 */
#define PB2_DM_QUARTER 3   /* 只有上方 1/4 能出弹幕 */

/*
 * 懒初始化（dlopen cairo/freetype/fontconfig + 加载 CJK 字体）。
 * 只在 worker 线程调用。返回 0 成功，-1 失败（失败后内部记住，不再重试、
 * 不再刷日志 —— 真机上每帧刷一行日志本身就能把播放拖死）。
 * 重复调用幂等。
 */
int  dm_init(void);

/* 释放（release / stop 时调）。可被重复调用；之后可以再次 dm_init()。 */
void dm_shutdown(void);

/* 是否已成功初始化（1/0）。 */
int  dm_ready(void);

/*
 * 设置显示模式（PB2_DM_*）与字号（像素高度，逻辑像素；传 0 = 用默认）。
 * 只在 worker 线程调用。模式变化会清空泳道占用，字号变化会清空字形缓存。
 */
void dm_set_mode(int mode, int font_px);

/*
 * 喂入一批弹幕文本（一次一个分片，几十 KB）。
 * 格式：每行 `id|progress|mode|color|fontsize|content`，最后一行可以没有换行。
 * content 里不会出现 '|' 和换行（JS 侧 buildFeedText() 已清理）。
 * 内部会解析并存进自己的数组（会先 dm_clear 掉上一片）。
 * 只在 worker 线程调用。返回解析成功的条数，-1 = 失败。
 */
int  dm_feed_text(const char *text);

/* 清空弹幕数据（换片 / seek 到别的分片 / 切集时调）。 */
void dm_clear(void);

/* 当前已加载的弹幕条数（诊断用）。 */
int  dm_count(void);

/*
 * 【核心】把弹幕烧进一帧 NV12。
 *   yp/uv  : Y 平面与 UV 平面基址（uv 每行 w 字节、U/V 交错，即 NV12 的 UV
 *            半高平面）；必须是可写内存，调用方要给的是 scratch 缓冲，不是
 *            GstBuffer 内部指针。
 *   w, h   : 帧像素宽高（都是偶数）
 *   rotate : 旋转量。**本模块同时接受两种编码**（因为 session.c 里
 *            g_w.rotate 存的是度数，而 JS 侧 transpose 是 1/2/3 的枚举）：
 *              90 或 1  = 90 度
 *              270 或 2 = 270 度（真机 g_w.rotate 的实测值）
 *              180 或 3 = 180 度
 *              其它     = 不旋转
 *            旋转后「逻辑宽 = 帧高、逻辑高 = 帧宽」，弹幕的左右与上下都在
 *            逻辑坐标系里算，合成时逐像素映射回帧坐标（不做整帧转换）。
 *   pos_ms : 当前播放位置（毫秒，视频时间轴）
 *   wall_ms: 当前墙钟毫秒（保留做将来的缓动/淡出；当前实现不读它，理由见
 *            dm_overlay.c 里 dm_render 的注释）
 * 返回实际绘制的弹幕条数。未初始化 / 模式为 OFF / 没有数据时立即返回 0，
 * 零分配零拷贝。只在 worker 线程调用（由 buffer probe 调用）。
 */
int  dm_render(unsigned char *yp, unsigned char *uv, int w, int h, int rotate,
               long long pos_ms, long long wall_ms);

/* 最后一次渲染的统计（诊断）：可见条数 / 绘制条数 / 耗时微秒。任一指针可为 NULL。 */
void dm_stats(int *visible, int *drawn, long long *last_us);

#endif /* PENBILI_DM_OVERLAY_H */
