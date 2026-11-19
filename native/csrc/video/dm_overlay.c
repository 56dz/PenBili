/*
 * dm_overlay.c - 弹幕烧帧（NV12 软合成）
 *
 * ======================== 这个文件为什么长这样 ========================
 * 视频平面（KMS Esmart1-win0）的 zpos 高于 Falcon UI，UI 文字盖不到画面
 * 上，所以弹幕只能改像素。挂钩点是 kmssink sink pad 的 buffer probe，
 * 拿到的 buffer 已经是 render-rectangle 尺寸（真机 266x472 NV12）。
 *
 * 本文件**不认识 GStreamer**：入口收的是裸 Y/UV 指针，调用方自己
 * gst_buffer_extract 到 scratch、dm_render 改完再 gst_buffer_fill 回去。
 * 这样做的两个理由：
 *   1) 铁律：不能用 GstMapInfo（third_party/gst_min.h 里结构体不完整，
 *      自己抄布局去骗偏移，一次 ABI 变化就是栈越界写）。
 *   2) 抽成纯 C 之后，合成算法可以在开发机上用 Python 精确复刻验证，
 *      不需要真机、不需要链接参数（见 .tmp/dm_blend_check.png）。
 *
 * ======================== 依赖只有 dlopen ========================
 * cairo / freetype / fontconfig 全部运行时 dlopen，不进链接行（stub 目录
 * 里没有这几个库）。dlopen 全是一次性的，失败后记住结果不再重试 —— 每帧
 * 去 dlopen 会把 4×A53 的解码线程拖死。
 *
 * 刻意**不调用任何 libm 函数**（虽然包含了 <math.h> 的取整思路）：build.ps1
 * 的 p2args 里只有 -lpthread，没有 -lm；zig cc 会不会隐式带 libm 取决于
 * 版本，赌不起。所以取整一律用 (int)(x + 0.5f)，clamp 用比较，不用 floorf
 * / fabsf / fminf。
 *
 * ======================== 旋转的坐标系约定 ========================
 * 面板是竖的，帧是横的。逻辑坐标系是「用户看着正的那一块」：
 *   rotate 90 / 270 -> 逻辑宽 = 帧高 h，逻辑高 = 帧宽 w
 *   rotate 0 / 180  -> 逻辑宽 = 帧宽 w，逻辑高 = 帧高 h
 * 弹幕的左右/上下全在逻辑系里算，合成时逐像素映射回帧坐标 —— 不做整帧
 * 转置（266x472 转一次要动 18 万像素，每帧做两次就是纯浪费）。
 *
 * 映射公式（(lx,ly) 逻辑 -> (fx,fy) 帧）：
 *   rot 0   : fx = lx            fy = ly
 *   rot 90  : fx = w-1-ly        fy = lx
 *   rot 180 : fx = w-1-lx        fy = h-1-ly
 *   rot 270 : fx = ly            fy = h-1-lx     <- 真机 g_w.rotate 实测值
 * 270 这条是现场量出来的：面板把帧逆时针转了 270 度，所以我们正向转回来。
 * 90 取同一个变换的镜像方向（fx 用 w-1-ly），两种写法都保证取值范围
 * 落在 [0,w) x [0,h) 内。
 */
#include "dm_overlay.h"

#include <dlfcn.h>
#include <math.h>   /* 只为满足「浮点语义可见」的约定，本文件不调用任何 libm 符号 */
#include <pthread.h> /* dm_render 在 streaming 线程，dm_feed_text 在 worker 线程 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../core/clock.h"
#include "../core/log.h"

/* ============================== 常量 ============================== */

#define DM_SCROLL_MS      8000  /* 滚动弹幕从右边缘到完全出左边（与 JS 侧一致） */
#define DM_FIXED_MS       4000  /* 顶部/底部弹幕的停留时长 */
#define DM_MAX_ONSCREEN   24    /* 每帧最多画这么多条，超出直接不画（保护解码线程） */
#define DM_CACHE_MAX      48    /* 字形位图缓存条目上限 */
#define DM_CACHE_BYTES    (8 * 1024 * 1024) /* 位图缓存总字节预算（放大字号后单条更占空间） */
#define DM_MAX_BMP_W      4096
#define DM_MAX_BMP_H      192
#define DM_DEFAULT_FONT_PX 24
#define DM_FONT_MIN       10
#define DM_FONT_MAX       96
#define DM_MAX_ITEMS      6000  /* 一个分片的弹幕条数上限，防着异常数据把内存吃掉 */
#define DM_MAX_LANES      64    /* 泳道数组的静态上限（266 高 / 28 = 9 条，够用） */
#define DM_MAX_TEXT       512   /* 单条文本字节上限（JS 侧 80 码点，emoji 最坏 4B/码点 = 320B） */
#define DM_SEEK_BACK_MS   500   /* 播放位置往回跳超过这个值，判定为 seek */
#define DM_SEEK_FWD_MS    3000  /* 往前跳超过这个值，也判定为 seek */
#define DM_STAT_MS        5000  /* 运行期统计日志间隔（视频时间轴毫秒） */

/* 【3.5.17】迟到容忍上限。滚动弹幕的 x 由 elapsed = pos_ms - t_ms 决定，
 * elapsed 一大就会算到画面中间甚至左侧 —— 用户看到的就是「从半路突然冒出」。
 * 播放中途开弹幕、切半屏/四分屏、泳道排满排队，都会让 elapsed 变得很大。
 * 超过这个值就干脆不显示：宁可少几条，也不能半路冒出来。 */
#define DM_LATE_MS        300
#define DM_LANE_DEAD      (-2)  /* 已判定迟到，本次播放不再尝试分配泳道 */
/* 【3.5.18】「不算从右边进」的诊断阈值：首次画出去时 elapsed 超过它，
 * 说明这条已经在屏幕中段了（距右缘 > 行程的 2%）。 */
#define DM_LATE_ENTRY_MS  160
/*
 * 【3.5.15 自适应丢帧】弹幕特别密时宁可少画几条也要保住帧率。
 * 每帧合成耗时超过 DM_SLOW_US 就减预算，低于 DM_FAST_US 再慢慢加回来。
 * 用户明确要求过"特别多的情况下可以丢弃一些以保证流畅"，这里让它自己找平衡。
 */
#define DM_SLOW_US        12000 /* 单帧合成超过 12ms 视为吃力 */
#define DM_FAST_US        4000  /* 低于 4ms 视为宽裕，可以多画 */
#define DM_BUDGET_MIN     6

/* cairo 枚举值（不能 include cairo.h，只能抄数字；这几个是 ABI 稳定的） */
#define DM_CAIRO_FORMAT_ARGB32   0
#define DM_CAIRO_OP_CLEAR        0
#define DM_CAIRO_OP_OVER         2
#define DM_CAIRO_SLANT_NORMAL    0
#define DM_CAIRO_WEIGHT_NORMAL   0

/* 弹幕分类 */
enum {
    DM_KIND_NONE = 0,
    DM_KIND_SCROLL = 1,
    DM_KIND_TOP = 2,
    DM_KIND_BOTTOM = 3
};

/* ============================ 数据结构 ============================ */

typedef struct {
    unsigned long long id;      /* idStr 解析值；不是纯数字时用哈希顶上 */
    unsigned long long thash;   /* 文本哈希：缓存 key 的一部分，防 id 冲突串字 */
    long long t_ms;             /* 这一条应该出现的视频时间（progress） */
    char *text;                 /* 自己 malloc 的副本；dm_feed_text 的入参不归我们 */
    int mode;
    unsigned int color;         /* 0xRRGGBB */
    /* 泳道号。**只有这一个字段是可变的运行期状态**：-1 表示还没分到。
     * 分不到就保持 -1，下一帧再试 —— 这样「泳道排满」只是推迟显示，
     * 不会把一条弹幕永久判死（seek 回来还能出现）。 */
    int lane;
    /* 【3.5.18】首次被真正画出去时的 elapsed（ms），-1 = 还没画过。
     * 它直接告诉我们「这条弹幕从右边缘进场时已经迟了多少毫秒」：
     * > DM_LATE_ENTRY_MS 就意味着它是在画面中段凭空出现的。 */
    int first_ms;
} dm_item;

typedef struct {
    int used;
    unsigned long long id;
    unsigned long long thash;
    int font_px;
    int bw;
    int bh;
    int stride;
    int adv;                    /* 文字前进宽度（逻辑像素），只用于诊断 */
    unsigned char *data;        /* cairo image surface 的像素（自上而下） */
    void *surf;                 /* cairo_surface_t*，销毁时用 */
    long long stamp;            /* 帧序号，LRU 用 */
} dm_glyph;

/* cairo 的 text_extents 布局（6 个 double，公开 ABI） */
typedef struct {
    double x_bearing;
    double y_bearing;
    double width;
    double height;
    double x_advance;
    double y_advance;
} dm_text_extents;

/* ========================= dlopen 出来的函数表 ========================= */

typedef struct {
    void *(*image_surface_create)(int, int, int);
    unsigned char *(*image_surface_get_data)(void *);
    int (*image_surface_get_stride)(void *);
    int (*surface_status)(void *);
    void (*surface_flush)(void *);
    void (*surface_destroy)(void *);
    void *(*create)(void *);
    void (*destroy)(void *);
    int (*status)(void *);
    void (*save)(void *);
    void (*restore)(void *);
    void (*translate)(void *, double, double);
    void (*scale)(void *, double, double);
    void (*set_operator)(void *, int);
    void (*set_source_rgba)(void *, double, double, double, double);
    void (*paint)(void *);
    void (*move_to)(void *, double, double);
    void (*text_extents)(void *, const char *, dm_text_extents *);
    void (*text_path)(void *, const char *);
    void (*fill)(void *);
    void (*stroke)(void *);
    void (*set_line_width)(void *, double);
    void (*set_font_size)(void *, double);
    void (*select_font_face)(void *, const char *, int, int);
    void (*set_font_face)(void *, void *);
    void *(*ft_font_face_create_for_ft_face)(void *, int);
    void (*font_face_destroy)(void *);
} dm_cairo_api;

typedef struct {
    int (*FT_Init_FreeType)(void **);
    int (*FT_Done_FreeType)(void *);
    int (*FT_New_Face)(void *, const char *, long, void **);
    int (*FT_Done_Face)(void *);
} dm_ft_api;

typedef struct {
    int (*FcConfigAppFontAddFile)(void *, const char *);
} dm_fc_api;

/* ============================== 状态 ============================== */

static dm_cairo_api g_cairo;
static dm_ft_api g_ft;
static dm_fc_api g_fc;

static void *g_cairo_lib;
static void *g_ft_lib;          /* libfreetype 的 dlopen 句柄 */
static void *g_fc_lib;

static void *g_ft_library;      /* FT_Library（FT_Init_FreeType 产出） */
static void *g_ft_face;         /* FT_Face（FT_New_Face 产出） */
static void *g_font_face;       /* cairo_font_face_t*（只有路线 B 才有） */
static void *g_probe_surf;      /* 1x1 探针 surface，只用来量字 */
static void *g_probe_cr;

static int g_route;             /* 0=未定 1=路线B(FT) 2=路线A(toy+fontconfig) */
/* 初始化失败时打的 stage 串。由各步骤自己写，而不是在 dm_init 里靠
 * 「哪个句柄非空」反推 —— 反推只能猜出「大概哪一层」，而真机上定位
 * 字体问题恰恰需要「是 .otf 打不开还是族名匹配不上」这种粒度。 */
static const char *g_stage = "dlopen";
static char g_family[64];       /* 路线 A 选中的族名 */
static const char *g_font_name = "none"; /* 日志用 */
static int g_ready;
static int g_failed;
static int g_dm_logged;         /* 首帧日志门闩 */

static dm_item *g_items;
static int g_count;
static int g_cap;

static dm_glyph g_cache[DM_CACHE_MAX];
static long long g_cache_bytes;
static long long g_frame;       /* 帧序号，给 LRU 打戳 */

static int g_mode = PB2_DM_OFF;
static int g_font_px = DM_DEFAULT_FONT_PX;

/* 【并发】这是本模块唯一的锁，保护「弹幕表 + 泳道表 + 统计量」这一组状态，
 * 不保护字形缓存（字形缓存只在 streaming 线程的 dm_render 里动）。
 *
 * 为什么必须有：dm_render() 由 kmssink 的 BUFFER probe 调用，跑在 GStreamer
 * 的 streaming 线程；dm_feed_text()/dm_clear()/dm_set_mode() 由命令队列的
 * worker 线程调用。两边同时碰 g_items 时，realloc() 会让 render 正在遍历的
 * 旧指针立刻失效 —— 真机表现为整条管线卡死（本次联调就踩了一次：
 * RECOVER 之后 teardown 再没重建，帧率掉到 0）。
 *
 * 粒度选择：render 全程持锁不现实（一次合成几毫秒，会把 worker 堵住），
 * 所以 render 只在「遍历前算好窗口 + 取泳道 + 逐条取文本指针」这段持锁，
 * 真正写像素的部分用拷贝出来的快照做。快照最多 DM_MAX_ONSCREEN 条。
 */
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

/* 前向声明：公开包装 dm_render() 写在 dm_stats() 旁边，实现体在后面 */
static int dm_render_locked(unsigned char *yp, unsigned char *uv, int w, int h, int rotate,
                            long long pos_ms, long long wall_ms);

/* 泳道占用：值是「这条泳道空出来的视频时刻」。三套独立，滚动/顶部/底部
 * 互不干扰（B 站本身就是分层渲染的）。 */
static long long g_lane_scroll[DM_MAX_LANES];
static long long g_lane_top[DM_MAX_LANES];
static long long g_lane_bot[DM_MAX_LANES];
static int g_lane_count;        /* 上一帧算出来的泳道数，变了就重置 */

static int g_budget = DM_MAX_ONSCREEN;  /* 当前每帧允许画多少条（自适应） */
static long long g_last_pos = -1;
static long long g_stat_ms;     /* 上一次打运行期统计的 pos_ms（5 秒一行） */

static struct {
    int visible;
    int drawn;
    long long last_us;
} g_stats;

/* 候选字体：设备上 /etc/miniapp/resources/fonts/ 里的实测清单，
 * HarmonyOS 的 .ttf 排第一（freetype 支持面最广，.otf 的 CFF 偶发问题）。 */
static const char *dm_font_paths[] = {
    "/etc/miniapp/resources/fonts/HarmonyOS_Sans_SC_Regular.ttf",
    "/etc/miniapp/resources/fonts/NotoSansSC-Regular.otf",
    "/etc/miniapp/resources/fonts/NotoSans-Regular.ttf",
    NULL
};

/* 路线 A 要按族名找字。设备上 /etc/fonts/fonts.conf 指向的 /usr/share/fonts
 * 不存在，所以族名缓存里通常只有我们用 FcConfigAppFontAddFile 塞进去的那
 * 一个 —— 逐个试，谁量出来有宽度就用谁。 */
static const char *dm_family_candidates[] = {
    "HarmonyOS Sans SC",
    "HarmonyOS Sans",
    "Noto Sans SC",
    "Noto Sans CJK SC",
    "Noto Sans",
    "sans-serif",
    NULL
};

/* ============================== 小工具 ============================== */

static unsigned char dm_clamp(float v) {
    if (!(v > 0.0f)) return 0;      /* 顺手把 NaN 也吃掉 */
    if (v >= 255.0f) return 255;
    return (unsigned char)(v + 0.5f);
}

static int dm_iri(float v) {
    return (int)(v + (v >= 0.0f ? 0.5f : -0.5f));
}

static unsigned long long dm_fnv1a(const char *s) {
    unsigned long long h = 1469598103934665603ULL;
    while (*s) {
        h ^= (unsigned char)*s++;
        h *= 1099511628211ULL;
    }
    return h;
}

/* 只认纯数字 id（B 站的 idStr 可超 2^53，所以按 unsigned long long 解析）；
 * 不是数字（理论上不会发生）就退回哈希 —— 这个值只当缓存 key 用，
 * 撞了最多是两个 id 共用一个位图，不会画错字（thash 还会再兜一层）。 */
static unsigned long long dm_parse_id(const char *s) {
    const char *p = s;
    unsigned long long v = 0;
    int digits = 0;

    while (*p == ' ' || *p == '\t') p++;
    while (*p >= '0' && *p <= '9') {
        v = v * 10ULL + (unsigned long long)(*p - '0');
        p++;
        digits++;
    }
    while (*p == ' ' || *p == '\t') p++;
    if (digits == 0 || *p) return dm_fnv1a(s);
    return v;
}

static long long dm_parse_ll(const char *s) {
    long long v = 0;
    int neg = 0;
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '-') { neg = 1; s++; }
    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (*s - '0');
        s++;
    }
    return neg ? -v : v;
}

/* 解析失败返回白色而不是 0：**黑色的弹幕是合法值**（B 站允许用户选黑），
 * 所以 0 不能当「没解析到」的哨兵。字段缺失时给白色，至少看得见。 */
static unsigned int dm_parse_color(const char *s) {
    unsigned int v = 0;
    const char *p = s;

    while (*p == ' ' || *p == '\t') p++;
    if (*p < '0' || *p > '9') return 0xffffffu;
    while (*p >= '0' && *p <= '9') {
        v = v * 10u + (unsigned int)(*p - '0');
        p++;
    }
    return v & 0xffffffu;
}

/* rotate 同时接受度数（session.c 里 g_w.rotate 的实测编码）和 1/2/3 的
 * transpose 枚举（JS 侧 setRect 传的），统一成 0/90/180/270。 */
static int dm_norm_rot(int r) {
    switch (r) {
    case 1:
    case 90:  return 90;
    case 2:
    case 270: return 270;
    case 3:
    case 180: return 180;
    default:  return 0;
    }
}

static int dm_kind(int mode) {
    switch (mode) {
    case 1:                     /* B 站标准滚动 */
    case 2:                     /* 旧协议里也是滚动，不认就白丢弹幕 */
    case 3:
    case 6:                     /* 逆向弹幕：本项目统一按右->左处理（见头注释） */
        return DM_KIND_SCROLL;
    case 4:  return DM_KIND_BOTTOM;
    case 5:  return DM_KIND_TOP;
    default: return DM_KIND_NONE; /* 7/8/9 高级弹幕：位置/动画在 JSON 里，放弃 */
    }
}

/* ============================== 字体加载 ============================== */

#define DM_SYM(handle, field, name)                                          \
    do {                                                                     \
        *(void **)(&(field)) = dlsym((handle), (name));                      \
        if (!(field)) return 0;                                              \
    } while (0)

static int dm_load_cairo(void *h) {
    DM_SYM(h, g_cairo.image_surface_create, "cairo_image_surface_create");
    DM_SYM(h, g_cairo.image_surface_get_data, "cairo_image_surface_get_data");
    DM_SYM(h, g_cairo.image_surface_get_stride, "cairo_image_surface_get_stride");
    DM_SYM(h, g_cairo.surface_flush, "cairo_surface_flush");
    DM_SYM(h, g_cairo.surface_destroy, "cairo_surface_destroy");
    DM_SYM(h, g_cairo.create, "cairo_create");
    DM_SYM(h, g_cairo.destroy, "cairo_destroy");
    DM_SYM(h, g_cairo.status, "cairo_status");
    DM_SYM(h, g_cairo.save, "cairo_save");
    DM_SYM(h, g_cairo.restore, "cairo_restore");
    DM_SYM(h, g_cairo.translate, "cairo_translate");
    DM_SYM(h, g_cairo.scale, "cairo_scale");
    DM_SYM(h, g_cairo.set_operator, "cairo_set_operator");
    DM_SYM(h, g_cairo.set_source_rgba, "cairo_set_source_rgba");
    DM_SYM(h, g_cairo.paint, "cairo_paint");
    DM_SYM(h, g_cairo.move_to, "cairo_move_to");
    DM_SYM(h, g_cairo.text_extents, "cairo_text_extents");
    DM_SYM(h, g_cairo.text_path, "cairo_text_path");
    DM_SYM(h, g_cairo.fill, "cairo_fill");
    DM_SYM(h, g_cairo.stroke, "cairo_stroke");
    DM_SYM(h, g_cairo.set_line_width, "cairo_set_line_width");
    DM_SYM(h, g_cairo.set_font_size, "cairo_set_font_size");
    DM_SYM(h, g_cairo.select_font_face, "cairo_select_font_face");
    /* 下面这几个只有路线 B 用，缺了不算整体失败（A 路能顶上） */
    *(void **)(&g_cairo.surface_status) = dlsym(h, "cairo_surface_status");
    *(void **)(&g_cairo.set_font_face) = dlsym(h, "cairo_set_font_face");
    *(void **)(&g_cairo.ft_font_face_create_for_ft_face) =
        dlsym(h, "cairo_ft_font_face_create_for_ft_face");
    *(void **)(&g_cairo.font_face_destroy) = dlsym(h, "cairo_font_face_destroy");
    return 1;
}

static int dm_load_ft(void *h) {
    DM_SYM(h, g_ft.FT_Init_FreeType, "FT_Init_FreeType");
    DM_SYM(h, g_ft.FT_Done_FreeType, "FT_Done_FreeType");
    DM_SYM(h, g_ft.FT_New_Face, "FT_New_Face");
    DM_SYM(h, g_ft.FT_Done_Face, "FT_Done_Face");
    return 1;
}

static void dm_apply_font(void *cr, int font_px) {
    if (g_route == 1) {
        if (g_cairo.set_font_face && g_font_face) {
            g_cairo.set_font_face(cr, g_font_face);
        }
    } else {
        g_cairo.select_font_face(cr, g_family, DM_CAIRO_SLANT_NORMAL,
                                 DM_CAIRO_WEIGHT_NORMAL);
    }
    g_cairo.set_font_size(cr, (double)font_px);
}

/* 用一个汉字探一下：宽度和高度都非零才算这个 font_face 真的能出 CJK。
 * 设备上没有 pango/harfbuzz，cairo 的 toy API 一旦找不到字就画空白，
 * 不会报错 —— 不探一次就会出现「初始化成功但屏幕上什么都没有」。 */
static int dm_probe_cjk(int font_px) {
    dm_text_extents te;
    if (!g_probe_cr) return 0;
    dm_apply_font(g_probe_cr, font_px);
    g_cairo.text_extents(g_probe_cr, "弹", &te);
    if (g_cairo.status(g_probe_cr) != 0) return 0;
    return (te.x_advance > 1.0 && te.height > 0.5) ? 1 : 0;
}

/* 路线 B：FT_New_Face -> cairo_ft_font_face_create_for_ft_face。
 * 最可控：字体文件是谁就用谁，不经过 fontconfig 的匹配逻辑。 */
static int dm_try_route_b(void) {
    const char **p;
    void *face = NULL;
    const char *hit = NULL;

    g_ft_lib = dlopen("libfreetype.so.6", RTLD_LAZY);
    if (!g_ft_lib) g_ft_lib = dlopen("libfreetype.so", RTLD_LAZY);
    if (!g_ft_lib) { g_stage = "freetype"; return 0; }
    if (!dm_load_ft(g_ft_lib)) { g_stage = "freetype"; return 0; }
    /* 库句柄和 FT_Library/FT_Face 都立刻记进全局：即使后面任何一步失败，
     * dm_shutdown() 也能把它们收干净，不会留下「半个 freetype」。 */
    if (g_ft.FT_Init_FreeType(&g_ft_library) != 0 || !g_ft_library) {
        g_stage = "freetype";
        return 0;
    }

    for (p = dm_font_paths; *p; p++) {
        if (g_ft.FT_New_Face(g_ft_library, *p, 0, &face) == 0 && face) {
            hit = *p;
            break;
        }
    }
    if (!face) { g_stage = "fontface"; return 0; }   /* 三个候选字体一个都打不开 */
    g_ft_face = face;

    /* cairo 编进 ft 后端才有这个符号。缺了它就只能走路线 A —— 这是
     * 「两条路都实现」的意义所在，不能因为缺符号就整个弹幕不可用。 */
    if (!g_cairo.ft_font_face_create_for_ft_face || !g_cairo.set_font_face) {
        g_stage = "fontface";   /* cairo 没编 ft 后端，只能交给路线 A */
        return 0;
    }
    g_font_face = g_cairo.ft_font_face_create_for_ft_face(face, 0);
    if (!g_font_face) { g_stage = "fontface"; return 0; }

    g_route = 1;
    /* 真跑一遍才能确认「有 FT_Face」和「cairo 能拿它出 CJK 字形」是两件事：
     * cairo 会按 font matrix 重新要 glyph，某些老 cairo 对 CJK face 的
     * unicode->glyph 映射会静默返回 0（画出来是空白，不报错）。 */
    if (!dm_probe_cjk(DM_DEFAULT_FONT_PX)) {
        g_route = 0;
        if (g_cairo.font_face_destroy) g_cairo.font_face_destroy(g_font_face);
        g_font_face = NULL;
        return 0;
    }
    g_font_name = hit ? hit : "freetype";
    return 1;
}

/* 路线 A：fontconfig 把字体文件塞进默认配置，再让 cairo 的 toy API 按族名
 * 自己找。设备上 /usr/share/fonts 不存在，不先 AddFile 就一个字体都没有。 */
static int dm_try_route_a(void) {
    const char **p;
    const char **fam;

    g_fc_lib = dlopen("libfontconfig.so.1", RTLD_LAZY);
    if (!g_fc_lib) g_fc_lib = dlopen("libfontconfig.so", RTLD_LAZY);
    if (!g_fc_lib) { g_stage = "fontconfig"; return 0; }
    DM_SYM(g_fc_lib, g_fc.FcConfigAppFontAddFile, "FcConfigAppFontAddFile");

    for (p = dm_font_paths; *p; p++) {
        /* 失败无所谓：下一个候选还能塞，反正最后靠族名探针判生死 */
        g_fc.FcConfigAppFontAddFile(NULL, *p);
    }
    for (fam = dm_family_candidates; *fam; fam++) {
        snprintf(g_family, sizeof(g_family), "%s", *fam);
        g_route = 2;
        if (dm_probe_cjk(DM_DEFAULT_FONT_PX)) {
            g_font_name = g_family;
            return 1;
        }
    }
    /* 六个族名全试完还是量不出「弹」：说明 AddFile 没真的进配置，
     * 或者这个字体文件本身没有 CJK 字形。这两种情况的现场表现一模一样
     * （屏幕上什么都没有），所以 stage 统一记 fontface，靠日志里的
     * font= 前缀区分不了，但至少知道不是 dlopen 挂了。 */
    g_stage = "fontface";
    g_route = 0;
    return 0;
}

/*
 * 释放所有 dlopen 出来的库与 cairo/freetype 对象，但**不动**业务状态
 * （弹幕数组、缓存、模式）。dm_init 的失败路径和 dm_shutdown 都要用，
 * 所以单独抽出来 —— 失败路径不释放的话，一次初始化失败就把三个 so 的
 * 句柄永久挂着，而真机上「用户点开关 -> 缺字体 -> 放弃」是常见路径。
 *
 * 顺序：cairo font face -> FT_Face -> FT_Library -> 探针 -> dlclose。
 * cairo 的 ft font face 内部引用 FT_Face，先销毁 face 再 FT_Done_Face；
 * 反过来就是 use-after-free，且只在下次取字形时炸，现场极难定位。
 */
static void dm_release_all(void) {
    if (g_font_face) {
        if (g_cairo.font_face_destroy) g_cairo.font_face_destroy(g_font_face);
        else g_ft_face = NULL;  /* 拿不到 destroy：宁可漏一个 face 也不留悬空指针 */
        g_font_face = NULL;
    }
    if (g_ft_face && g_ft.FT_Done_Face) {
        g_ft.FT_Done_Face(g_ft_face);
    }
    g_ft_face = NULL;
    if (g_ft_library && g_ft.FT_Done_FreeType) {
        g_ft.FT_Done_FreeType(g_ft_library);
    }
    g_ft_library = NULL;

    if (g_probe_cr && g_cairo.destroy) {
        g_cairo.destroy(g_probe_cr);
    }
    g_probe_cr = NULL;
    if (g_probe_surf && g_cairo.surface_destroy) {
        g_cairo.surface_destroy(g_probe_surf);
    }
    g_probe_surf = NULL;

    if (g_cairo_lib) {
        dlclose(g_cairo_lib);
        g_cairo_lib = NULL;
    }
    if (g_ft_lib) {
        dlclose(g_ft_lib);
        g_ft_lib = NULL;
    }
    if (g_fc_lib) {
        dlclose(g_fc_lib);
        g_fc_lib = NULL;
    }

    memset(&g_cairo, 0, sizeof(g_cairo));
    memset(&g_ft, 0, sizeof(g_ft));
    memset(&g_fc, 0, sizeof(g_fc));
}

static int dm_init_locked(void) {
    const char *stage = "dlopen";
    int ok;

    if (g_ready) return 0;
    if (g_failed) return -1;    /* 失败过就永不重试，也不再刷日志 */
    g_stage = "dlopen";

    g_cairo_lib = dlopen("libcairo.so.2", RTLD_LAZY);
    if (!g_cairo_lib) g_cairo_lib = dlopen("libcairo.so", RTLD_LAZY);
    if (!g_cairo_lib) { stage = "dlopen"; goto fail; }
    memset(&g_cairo, 0, sizeof(g_cairo));
    if (!dm_load_cairo(g_cairo_lib)) { stage = "dlopen"; goto fail; }

    /* 1x1 探针：cairo 的文字度量只跟 font_face/size 有关，跟 surface 大小
     * 无关，所以一个 1x1 的 surface 就够量所有字，避免每次冷启动都新建。 */
    g_probe_surf = g_cairo.image_surface_create(DM_CAIRO_FORMAT_ARGB32, 1, 1);
    if (!g_probe_surf) { stage = "surface"; goto fail; }
    if (g_cairo.surface_status && g_cairo.surface_status(g_probe_surf) != 0) {
        stage = "surface";
        goto fail;
    }
    g_probe_cr = g_cairo.create(g_probe_surf);
    if (!g_probe_cr) { stage = "surface"; goto fail; }
    if (g_cairo.status(g_probe_cr) != 0) { stage = "surface"; goto fail; }

    /* 先 B 后 A：B 更可控，A 依赖 fontconfig 的族名匹配，作为兜底。
     * 任何一条成功就够用，两条都挂才判定弹幕不可用。
     * g_stage 由失败的那一层自己写，这里只负责在最终失败时用它。 */
    if (dm_try_route_b()) {
        ok = 1;
    } else {
        ok = dm_try_route_a();
    }
    if (!ok) {
        stage = g_stage;
        goto fail;
    }

    g_font_px = DM_DEFAULT_FONT_PX;
    g_lane_count = 0;
    g_ready = 1;
    g_failed = 0;
    pb2_logf("dm: overlay ready font=%s px=%d", g_font_name, g_font_px);
    return 0;

fail:
    /* 初始化失败不能让播放跟着死：调用方看到 -1 会把模式置 OFF，
     * 之后 dm_render 第一条语句就返回，代价是一个比较。 */
    pb2_logf("dm: overlay 初始化失败 stage=%s", stage);
    if (strcmp(stage, "fontface") == 0 || strcmp(stage, "fontconfig") == 0) {
        pb2_logf("dm: 字体初始化失败（弹幕不可用）");
    }
    dm_release_all();
    g_failed = 1;
    g_ready = 0;
    return -1;
}

/* ============================== 字形缓存 ============================== */

static void dm_glyph_release(dm_glyph *gl) {
    if (!gl->used) return;
    if (gl->surf && g_cairo.surface_destroy) g_cairo.surface_destroy(gl->surf);
    g_cache_bytes -= (long long)gl->bw * gl->bh * 4;
    if (g_cache_bytes < 0) g_cache_bytes = 0;
    memset(gl, 0, sizeof(*gl));
}

static void dm_cache_clear(void) {
    int i;
    for (i = 0; i < DM_CACHE_MAX; i++) dm_glyph_release(&g_cache[i]);
    g_cache_bytes = 0;
}

/* 淘汰到「条数 <= 上限 且 总字节 <= 预算」。keep 是刚插入的下标，不能选它，
 * 否则刚建好的位图会被自己挤掉，下一帧又要重建（每帧一次 cairo 描边，
 * 4×A53 上就是肉眼可见的掉帧）。 */
static void dm_cache_trim(int keep) {
    for (;;) {
        int i;
        int victim = -1;
        int live = 0;
        long long oldest = 0;

        for (i = 0; i < DM_CACHE_MAX; i++) {
            if (!g_cache[i].used) continue;
            live++;
            if (i == keep) continue;
            if (victim < 0 || g_cache[i].stamp < oldest) {
                victim = i;
                oldest = g_cache[i].stamp;
            }
        }
        if (live == 0) break;
        /* 条数上限由数组长度天然保证（live 不可能超过 DM_CACHE_MAX），
         * 这里真正要守的是字节预算 —— 一条 200px 宽的长句位图就 20KB，
         * 48 条满打满算远超 6MB，光靠条数封顶挡不住。 */
        if (g_cache_bytes <= DM_CACHE_BYTES) break;
        if (victim < 0) break;
        dm_glyph_release(&g_cache[victim]);
    }
}

static void dm_cache_put(dm_glyph *dst) {
    int i;
    int slot = -1;

    for (i = 0; i < DM_CACHE_MAX; i++) {
        if (!g_cache[i].used) { slot = i; break; }
    }
    if (slot < 0) {
        /* 全满：挑最久没用的那个（这才是真正的 LRU 淘汰路径） */
        long long oldest = 0;
        for (i = 0; i < DM_CACHE_MAX; i++) {
            if (slot < 0 || g_cache[i].stamp < oldest) {
                slot = i;
                oldest = g_cache[i].stamp;
            }
        }
    }
    if (slot < 0) return;
    dm_glyph_release(&g_cache[slot]);
    g_cache[slot] = *dst;
    g_cache_bytes += (long long)dst->bw * dst->bh * 4;
    dm_cache_trim(slot);
}

/* 把一条弹幕画成 ARGB32 位图：先黑色描边再填字色，都在同一个 surface 上
 * 完成（描边和填充必须叠在同一张图上，分开合成会露出半透明白边）。 */
static int dm_glyph_build(dm_item *it, dm_glyph *out) {
    dm_text_extents te;
    void *cr;
    void *surf;
    int pad;
    int bw;
    int bh;
    int use_px;          /* 实际绘制字号；超长文本会从 g_font_px 缩下来 */
    double inkw;
    double inkh;
    unsigned int c;

    use_px = g_font_px;
    dm_apply_font(g_probe_cr, g_font_px);
    g_cairo.text_extents(g_probe_cr, it->text, &te);
    if (g_cairo.status(g_probe_cr) != 0) return 0;

    inkw = te.width < 0.0 ? -te.width : te.width;
    inkh = te.height < 0.0 ? -te.height : te.height;

    /*
     * 【3.5.15 超长文本缩字】字号按缓冲区换算后可以到 65px 以上，80 个汉字的
     * 宽度能到 5000px，直接超过位图上限 —— 那样的弹幕会被整条丢弃，用户只
     * 会觉得"这条怎么不显示"。这里按比例缩字重测一次：宁可小一点，不能不显示。
     * 注意 out->font_px 仍然写名义字号，否则字形缓存永远命中不了。
     */
    if (inkw > 0.5) {
        double limit = (double)(DM_MAX_BMP_W - 2 * (g_font_px / 8 + 1));
        if (inkw > limit && limit > 8.0) {
            int shrunk = (int)((double)g_font_px * limit / inkw);
            if (shrunk < DM_FONT_MIN) shrunk = DM_FONT_MIN;
            if (shrunk < g_font_px) {
                use_px = shrunk;
                dm_apply_font(g_probe_cr, shrunk);
                g_cairo.text_extents(g_probe_cr, it->text, &te);
                inkw = te.width < 0.0 ? -te.width : te.width;
                inkh = te.height < 0.0 ? -te.height : te.height;
            }
        }
    }
    /* 用 !(x >= 0.5) 而不是 x < 0.5：NaN 参与的比较恒为假，写成后一种形式
     * 时 NaN 会漏过检查一路走到 dm_iri()，而 (int)NaN 是未定义行为。 */
    if (!(inkw >= 0.5) || !(inkh >= 0.5)) return 0;

    /*
     * 【3.5.15 描边改薄】外层线宽 use_px/10，cairo 的 stroke 是居中的，向
     * 路径两侧各扩一半 = use_px/20，再留 1px 给抗锯齿。原来按 use_px/8 留白
     * （43px 字要 5px 黑圈），既占位又把字糊成一坨。
     */
    pad = (use_px / 20 > 1 ? use_px / 20 : 1) + 1;
    bw = dm_iri((float)inkw) + pad * 2;
    bh = dm_iri((float)inkh) + pad * 2;
    if (bw <= 0 || bh <= 0 || bw > DM_MAX_BMP_W || bh > DM_MAX_BMP_H) return 0;
    if ((long long)bw * bh * 4 > DM_CACHE_BYTES / 4) return 0;

    surf = g_cairo.image_surface_create(DM_CAIRO_FORMAT_ARGB32, bw, bh);
    if (!surf) return 0;
    if (g_cairo.surface_status && g_cairo.surface_status(surf) != 0) {
        g_cairo.surface_destroy(surf);
        return 0;
    }
    cr = g_cairo.create(surf);
    if (!cr) {
        g_cairo.surface_destroy(surf);
        return 0;
    }

    /* 新建的 image surface 文档上说是全 0，这里还是显式清一次：
     * 万一某个 cairo 版本偷懒，残留 alpha 会在画面上变成噪点。 */
    g_cairo.set_operator(cr, DM_CAIRO_OP_CLEAR);
    g_cairo.paint(cr);
    g_cairo.set_operator(cr, DM_CAIRO_OP_OVER);

    dm_apply_font(cr, use_px);
    /* 把字形墨迹的左上角挪到 (pad,pad)：x_bearing/y_bearing 是墨迹框相对
     * 基线的偏移，y_bearing 通常是负数（y 轴向下） */
    g_cairo.translate(cr, (double)pad - te.x_bearing, (double)pad - te.y_bearing);
    g_cairo.move_to(cr, 0.0, 0.0);

    c = it->color;   /* dm_parse_color 已保证字段缺失时是白色，这里不再兜底 */

    /*
     * 【3.5.15 描边改薄】原来是 use_px/8 的纯黑实边（43px 字上是 5px 黑圈），
     * 用户反馈「黑圈太厚」。改成两层柔和的：
     *   外层：宽一点的暗灰半透明 —— 充当投影，让白字从任何底色上"浮"出来
     *   内层：窄一点的浅灰 —— 只补一圈轮廓，不形成硬边
     * 两层都开着抗锯齿（cairo 默认），边缘是渐变不是台阶。
     */
    g_cairo.text_path(cr, it->text);
    g_cairo.set_source_rgba(cr, 0.10, 0.10, 0.10, 0.42);   /* 外层：柔和投影 */
    g_cairo.set_line_width(cr, (double)(use_px / 10 > 2 ? use_px / 10 : 2));
    g_cairo.stroke(cr);

    g_cairo.text_path(cr, it->text);
    g_cairo.set_source_rgba(cr, 0.55, 0.55, 0.55, 0.85);   /* 内层：浅灰薄轮廓 */
    g_cairo.set_line_width(cr, (double)(use_px / 20 > 1 ? use_px / 20 : 1));
    g_cairo.stroke(cr);

    g_cairo.text_path(cr, it->text);
    g_cairo.set_source_rgba(cr,
                            (double)((c >> 16) & 0xff) / 255.0,
                            (double)((c >> 8) & 0xff) / 255.0,
                            (double)(c & 0xff) / 255.0,
                            1.0);
    g_cairo.fill(cr);

    g_cairo.destroy(cr);
    g_cairo.surface_flush(surf);

    memset(out, 0, sizeof(*out));
    out->data = g_cairo.image_surface_get_data(surf);
    if (!out->data) {
        g_cairo.surface_destroy(surf);
        return 0;
    }
    out->surf = surf;
    out->stride = g_cairo.image_surface_get_stride(surf);
    out->bw = bw;
    out->bh = bh;
    out->adv = dm_iri((float)te.x_advance);
    out->id = it->id;
    out->thash = it->thash;
    out->font_px = g_font_px;
    out->used = 1;
    out->stamp = g_frame;
    return 1;
}

static dm_glyph *dm_glyph_get(dm_item *it) {
    dm_glyph tmp;
    int i;

    for (i = 0; i < DM_CACHE_MAX; i++) {
        dm_glyph *gl = &g_cache[i];
        if (!gl->used) continue;
        if (gl->id != it->id) continue;
        if (gl->thash != it->thash) continue;
        if (gl->font_px != g_font_px) continue;
        gl->stamp = g_frame;
        return gl;
    }
    if (!dm_glyph_build(it, &tmp)) return NULL;
    dm_cache_put(&tmp);
    /* put 之后 tmp 已经进表，重新找一次拿到表内地址 */
    for (i = 0; i < DM_CACHE_MAX; i++) {
        dm_glyph *gl = &g_cache[i];
        if (gl->used && gl->id == tmp.id && gl->thash == tmp.thash &&
            gl->font_px == tmp.font_px) {
            return gl;
        }
    }
    return NULL;
}

/* ============================== 弹幕数据 ============================== */

int dm_count(void) {
    int n;
    pthread_mutex_lock(&g_lock);
    n = g_count;
    pthread_mutex_unlock(&g_lock);
    return n;
}

/* 内部版本：调用方必须已持 g_lock。dm_feed_text 换片时直接用它，
 * 避免「先 unlock 再 lock」被 render 插进来看到半清空的表。 */
static void dm_clear_locked(void) {
    int i;
    for (i = 0; i < g_count; i++) {
        free(g_items[i].text);
        g_items[i].text = NULL;
    }
    g_count = 0;
    /* 泳道占用跟着数据一起清：换片之后旧泳道表毫无意义 */
    memset(g_lane_scroll, 0, sizeof(g_lane_scroll));
    memset(g_lane_top, 0, sizeof(g_lane_top));
    memset(g_lane_bot, 0, sizeof(g_lane_bot));
    g_lane_count = 0;
    g_last_pos = -1;
    g_stat_ms = 0;
    g_budget = DM_MAX_ONSCREEN;
}

void dm_clear(void) {
    pthread_mutex_lock(&g_lock);
    dm_clear_locked();
    pthread_mutex_unlock(&g_lock);
}

static int dm_cmp_item(const void *a, const void *b) {
    const dm_item *x = (const dm_item *)a;
    const dm_item *y = (const dm_item *)b;
    if (x->t_ms < y->t_ms) return -1;
    if (x->t_ms > y->t_ms) return 1;
    if (x->id < y->id) return -1;
    if (x->id > y->id) return 1;
    return 0;
}

static int dm_feed_text_locked(const char *text) {
    char *buf;
    char *p;
    size_t len;
    int n = 0;
    int truncated = 0;

    dm_clear_locked();
    if (!text || !*text) return 0;

    len = strlen(text);
    /* 复制一份再切：入参是 const，而且就在调用方栈外由它自己 free，
     * 直接在里面写 '\0' 属于越界改别人的内存。几十 KB 拷一次可接受。 */
    buf = (char *)malloc(len + 1);
    if (!buf) return -1;
    memcpy(buf, text, len + 1);

    p = buf;
    while (*p) {
        char *line = p;
        char *nl = strchr(p, '\n');
        char *f[5];
        char *q;
        int nf = 0;

        if (nl) {
            *nl = '\0';
            p = nl + 1;
        } else {
            p += strlen(p);
        }
        if (*line == '\0') continue;
        len = strlen(line);
        while (len > 0 && (line[len - 1] == '\r' || line[len - 1] == ' ')) {
            line[--len] = '\0';
        }
        if (*line == '\0') continue;

        /* 只切前 5 个 '|'，剩下的整段都是内容（JS 侧保证内容里没有 '|'，
         * 但这里仍然只在第 5 个分隔符处停手，容错更强） */
        q = line;
        while (nf < 5) {
            char *bar = strchr(q, '|');
            if (!bar) break;
            *bar = '\0';
            f[nf++] = q;
            q = bar + 1;
        }
        if (nf < 5) continue;

        if (g_count >= DM_MAX_ITEMS) {
            truncated = 1;
            break;
        }
        if (*q == '\0') continue;                   /* 空内容不画 */
        if (strlen(q) >= DM_MAX_TEXT) {
            /* 截断必须落在 UTF-8 字符边界上：从第 DM_MAX_TEXT-1 字节往回退到
             * 一个首字节（(b & 0xC0) != 0x80），否则尾部会留下半个汉字，
             * cairo 拿到非法序列要么画方块要么直接跳过整段。 */
            size_t cut = DM_MAX_TEXT - 1;
            while (cut > 0 && ((unsigned char)q[cut] & 0xc0) == 0x80) cut--;
            q[cut] = '\0';
        }

        if (g_count == g_cap) {
            int ncap = g_cap ? g_cap * 2 : 512;
            dm_item *ni = (dm_item *)realloc(g_items, (size_t)ncap * sizeof(dm_item));
            if (!ni) {
                truncated = 2;
                break;
            }
            g_items = ni;
            g_cap = ncap;
        }

        memset(&g_items[g_count], 0, sizeof(dm_item));
        g_items[g_count].id = dm_parse_id(f[0]);
        g_items[g_count].t_ms = dm_parse_ll(f[1]);
        g_items[g_count].mode = (int)dm_parse_ll(f[2]);
        g_items[g_count].color = dm_parse_color(f[3]);
        g_items[g_count].text = (char *)malloc(strlen(q) + 1);
        if (!g_items[g_count].text) {
            truncated = 2;
            break;
        }
        strcpy(g_items[g_count].text, q);
        g_items[g_count].thash = dm_fnv1a(g_items[g_count].text);
        g_items[g_count].lane = -1;
        g_items[g_count].first_ms = -1;
        if (g_items[g_count].t_ms < 0) g_items[g_count].t_ms = 0;
        g_count++;
        n++;
    }
    free(buf);

    /* 按时间排序：一来泳道分配必须按出现顺序做（否则先到的抢不到泳道），
     * 二来渲染时能用二分把「本帧可能可见」的区间夹出来。 */
    if (g_count > 1) {
        qsort(g_items, (size_t)g_count, sizeof(dm_item), dm_cmp_item);
    }
    if (truncated == 1) {
        pb2_logf("dm: 弹幕超过 %d 条，多出的部分已丢弃", DM_MAX_ITEMS);
    } else if (truncated == 2) {
        pb2_logf("dm: 弹幕入表失败（内存不足），已入 %d 条", n);
        return -1;
    }
    return n;
}

/* 公开入口：整批替换期间持锁，render 不会看到「清了一半」的表。 */
int dm_feed_text(const char *text) {
    int n;
    pthread_mutex_lock(&g_lock);
    n = dm_feed_text_locked(text);
    pthread_mutex_unlock(&g_lock);
    return n;
}

static void dm_set_mode_locked(int mode, int font_px) {
    if (mode < PB2_DM_OFF || mode > PB2_DM_QUARTER) mode = PB2_DM_OFF;
    if (font_px <= 0) font_px = DM_DEFAULT_FONT_PX;
    if (font_px < DM_FONT_MIN) font_px = DM_FONT_MIN;
    if (font_px > DM_FONT_MAX) font_px = DM_FONT_MAX;

    if (mode != g_mode || font_px != g_font_px) {
        /* 泳道数是「可用高度 / 泳道高」算出来的，模式或字号一变就作废 */
        memset(g_lane_scroll, 0, sizeof(g_lane_scroll));
        memset(g_lane_top, 0, sizeof(g_lane_top));
        memset(g_lane_bot, 0, sizeof(g_lane_bot));
        g_lane_count = 0;
    }
    if (font_px != g_font_px) {
        /* 旧字号的位图永远不会再命中，占着 6MB 预算只会把新字号挤出去 */
        dm_cache_clear();
        g_font_px = font_px;
    }
    g_mode = mode;
}

void dm_set_mode(int mode, int font_px) {
    pthread_mutex_lock(&g_lock);
    dm_set_mode_locked(mode, font_px);
    pthread_mutex_unlock(&g_lock);
}

int dm_ready(void) {
    return g_ready;
}

void dm_stats(int *visible, int *drawn, long long *last_us) {
    pthread_mutex_lock(&g_lock);
    if (visible) *visible = g_stats.visible;
    if (drawn) *drawn = g_stats.drawn;
    if (last_us) *last_us = g_stats.last_us;
    pthread_mutex_unlock(&g_lock);
}

/* 公开入口：只多了一层锁。锁粒度是「整帧合成」，这是刻意的取舍 ——
 * 拆成细粒度要么让快照拷贝成本超过合成本身，要么得给每条弹幕引用计数。
 * 实测合成耗时的中位数在 1~3ms（首帧字形未命中时 ~140ms），worker 线程
 * 每 50ms 才会跑一次命令处理，被挡几百毫秒没有任何功能影响。 */
int dm_render(unsigned char *yp, unsigned char *uv, int w, int h, int rotate,
              long long pos_ms, long long wall_ms) {
    int n;
    pthread_mutex_lock(&g_lock);
    n = dm_render_locked(yp, uv, w, h, rotate, pos_ms, wall_ms);
    pthread_mutex_unlock(&g_lock);
    return n;
}

/* ============================== 泳道 ============================== */

/* 找一条此刻空着的泳道。占用表里存的是「什么时候空出来」（视频时间轴上的
 * 绝对毫秒），所以暂停/seek 之后不用特别清理 —— 前提是 seek 时重置过。 */
static int dm_take_lane(dm_item *it, int kind, int lanes, long long now_ms) {
    long long *tab;
    long long until;
    int i;

    tab = (kind == DM_KIND_SCROLL) ? g_lane_scroll
        : (kind == DM_KIND_TOP) ? g_lane_top : g_lane_bot;
    until = it->t_ms + (kind == DM_KIND_SCROLL ? DM_SCROLL_MS : DM_FIXED_MS);

    for (i = 0; i < lanes; i++) {
        if (tab[i] <= now_ms) {
            tab[i] = until;
            it->lane = i;
            return 1;
        }
    }
    return 0;   /* 排满：这一条这一轮不出现，不叠着画 */
}

static void dm_reset_lanes(void) {
    int i;
    for (i = 0; i < DM_MAX_LANES; i++) {
        g_lane_scroll[i] = 0;
        g_lane_top[i] = 0;
        g_lane_bot[i] = 0;
    }
    for (i = 0; i < g_count; i++) {
        g_items[i].lane = -1;
        g_items[i].first_ms = -1;
    }
}

/* ============================== 合成 ============================== */

/* 逻辑坐标 -> 帧坐标（四个公式见文件头） */
static void dm_map(int rot, int w, int h, int lx, int ly, int *fx, int *fy) {
    switch (rot) {
    case 90:
        *fx = w - 1 - ly;
        *fy = lx;
        break;
    case 180:
        *fx = w - 1 - lx;
        *fy = h - 1 - ly;
        break;
    case 270:
        *fx = ly;
        *fy = h - 1 - lx;
        break;
    default:
        *fx = lx;
        *fy = ly;
        break;
    }
}

/*
 * 单个像素的 NV12 合成。cairo ARGB32 是**预乘**的：0xAARRGGBB 里 RGB 已经
 * 乘过 A，所以先除掉 A 还原成非预乘色，再算 YUV：
 *     a  = A / 255
 *     sr = R / a, sg = G / a, sb = B / a          (a > 0)
 *     Y  =  0.299*sr    + 0.587*sg    + 0.114*sb
 *     U  = -0.168736*sr - 0.331264*sg + 0.5*sb    + 128
 *     V  =  0.5*sr      - 0.418688*sg - 0.081312*sb + 128
 *     dstY = dstY*(1-a) + Y*a
 * UV 是 2x2 下采样，只在 (fx 偶, fy 偶) 的像素上更新，否则一个字会写四遍
 * UV，彩色边缘会糊成一块。减少除法：R/a = R*255/A，A=255 时就是原值。
 *
 * 端序：cairo 的 ARGB32 在内存里是**本机字序**的一个 uint32，aarch64 是
 * 小端，所以按 unsigned int 读出来正好是 0xAARRGGBB。真要移植到大端机器上，
 * 这里必须改成按字节读 data[0]=B data[1]=G data[2]=R data[3]=A。
 */
static void dm_put_px(unsigned char *yp, unsigned char *uv, int w,
                      int fx, int fy, unsigned int argb) {
    unsigned int a8 = argb >> 24;
    float a;
    float inv;
    float sr;
    float sg;
    float sb;
    float yy;
    float uu;
    float vv;
    size_t yi;
    size_t ui;

    if (a8 == 0) return;

    sr = (float)((argb >> 16) & 0xff);
    sg = (float)((argb >> 8) & 0xff);
    sb = (float)(argb & 0xff);

    if (a8 != 255) {
        /* 预乘还原：R/a 就是 R*255/A。A=255 时这一步是恒等变换，
         * 单独走快路径能省掉逐像素的除法（A53 上除法是 10+ 周期，
         * 一帧十几万像素就是实打实的时间）。 */
        inv = 255.0f / (float)a8;
        sr *= inv;
        sg *= inv;
        sb *= inv;
    }

    yy = 0.299f * sr + 0.587f * sg + 0.114f * sb;
    yi = (size_t)fy * (size_t)w + (size_t)fx;

    if (a8 == 255) {
        yp[yi] = dm_clamp(yy);
    } else {
        a = (float)a8 * (1.0f / 255.0f);
        yp[yi] = dm_clamp((float)yp[yi] * (1.0f - a) + yy * a);
    }

    if (((fx | fy) & 1) != 0) return;   /* UV 只取偶数列偶数行 */

    uu = -0.168736f * sr - 0.331264f * sg + 0.5f * sb + 128.0f;
    vv = 0.5f * sr - 0.418688f * sg - 0.081312f * sb + 128.0f;
    ui = (size_t)(fy >> 1) * (size_t)w + (size_t)(fx & ~1);

    if (a8 == 255) {
        uv[ui] = dm_clamp(uu);
        uv[ui + 1] = dm_clamp(vv);
    } else {
        a = (float)a8 * (1.0f / 255.0f);
        uv[ui] = dm_clamp((float)uv[ui] * (1.0f - a) + uu * a);
        uv[ui + 1] = dm_clamp((float)uv[ui + 1] * (1.0f - a) + vv * a);
    }
}

/* 位图按旋转逐像素写到帧里。每行只算一次映射，行内用「bx 每 +1 帧坐标
 * 走多少」的差分推进 —— dm_map 是线性的，差分恒为 (0,±1) 或 (±1,0)，
 * 这样旋转不带来任何 per-pixel 的分支开销，也不需要整帧转置。 */
static void dm_blit(unsigned char *yp, unsigned char *uv, int w, int h, int rot,
                    const dm_glyph *gl, int lx, int ly) {
    int by;

    for (by = 0; by < gl->bh; by++) {
        const unsigned int *row;
        int lyy = ly + by;
        int fx;
        int fy;
        int fx1;
        int fy1;
        int dfx;
        int dfy;
        int bx;

        if (lyy < 0 || lyy >= (rot == 90 || rot == 270 ? w : h)) continue;
        row = (const unsigned int *)(gl->data + (size_t)by * (size_t)gl->stride);
        dm_map(rot, w, h, lx, lyy, &fx, &fy);
        dm_map(rot, w, h, lx + 1, lyy, &fx1, &fy1);
        dfx = fx1 - fx;
        dfy = fy1 - fy;

        for (bx = 0; bx < gl->bw; bx++, fx += dfx, fy += dfy) {
            if (fx < 0 || fx >= w || fy < 0 || fy >= h) continue;
            if (((const unsigned char *)row)[bx * 4 + 3] == 0) continue;
            dm_put_px(yp, uv, w, fx, fy, row[bx]);
        }
    }
}

/* 二分找第一条 t_ms >= t 的下标（g_items 按 t_ms 升序） */
static int dm_lower_bound(long long t) {
    int lo = 0;
    int hi = g_count;
    while (lo < hi) {
        int mid = (lo + hi) >> 1;
        if (g_items[mid].t_ms < t) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

/* 真正干活的那一半，调用方必须已持 g_lock（见文件末尾的公开包装）。 */
static int dm_render_locked(unsigned char *yp, unsigned char *uv, int w, int h, int rotate,
                            long long pos_ms, long long wall_ms) {
    long long t0;
    long long elapsed;
    int rot;
    int lw;
    int lh;
    int avail;
    int lane_h;
    int lanes;
    int start;
    int i;
    int drawn = 0;
    int visible = 0;
    static long long dead;     /* 累计迟到丢弃数，每 5 秒打一次并归零 */
    static long long hungry;   /* 【3.5.18】本帧被预算挡在门外的条目数。
                                * 旧序下这些条目会先占泳道再被丢，把位置卡住 8 秒。 */
    static long long late_entry;  /* 【3.5.18】首次画出去时已经迟了超过 DM_LATE_ENTRY_MS 的条目数。
                                   * 这就是「半路冒出」的直接计数器。 */

    /* ---- 早退：模式关闭或没初始化成功。必须是第一条语句，一帧都不能多花。 */
    if (!g_ready || g_mode == PB2_DM_OFF) return 0;

    (void)wall_ms;  /* 现在所有动画都由 pos_ms 驱动，暂停时弹幕也应定格 */
    if (!yp || !uv || w < 16 || h < 16) return 0;
    if (g_count <= 0) return 0;

    t0 = pb2_now_us();
    g_frame++;

    rot = dm_norm_rot(rotate);
    if (rot == 90 || rot == 270) {
        lw = h;
        lh = w;
    } else {
        lw = w;
        lh = h;
    }

    /* 可见区域：FULL 整帧、HALF 上半、QUARTER 上方 1/4。
     * 用户开「半屏」就是不想让弹幕糊满整幅画，超出可用区的直接丢弃，
     * 不往剩余空间里挤 —— 挤压会让弹幕挤成一片，比不显示更糟。 */
    avail = (g_mode == PB2_DM_FULL) ? lh
        : (g_mode == PB2_DM_HALF) ? lh / 2 : lh / 4;
    lane_h = g_font_px + 4;
    lanes = avail / lane_h;
    if (lanes < 1) lanes = 1;
    if (lanes > DM_MAX_LANES) lanes = DM_MAX_LANES;
    if (lanes != g_lane_count) {
        g_lane_count = lanes;
        dm_reset_lanes();
    }

    /* seek 检测：往回跳（或大跨度前跳）时泳道占用表必须重来，否则会出现
     * 「跳回去之后一半弹幕再也不显示」。 */
    if (g_last_pos >= 0 &&
        (pos_ms < g_last_pos - DM_SEEK_BACK_MS ||
         pos_ms > g_last_pos + DM_SEEK_FWD_MS)) {
        dm_reset_lanes();
    }
    g_last_pos = pos_ms;

    start = dm_lower_bound(pos_ms - DM_SCROLL_MS);

    for (i = start; i < g_count; i++) {
        dm_item *it = &g_items[i];
        dm_glyph *gl;
        int kind;
        int life;
        int x;
        int y;

        if (it->t_ms > pos_ms) break;
        elapsed = pos_ms - it->t_ms;
        kind = dm_kind(it->mode);
        if (kind == DM_KIND_NONE) continue;     /* 7/8/9：整条跳过，不计入统计 */
        life = (kind == DM_KIND_SCROLL) ? DM_SCROLL_MS : DM_FIXED_MS;
        if (elapsed < 0 || elapsed >= life) continue;

        if (it->lane == DM_LANE_DEAD) continue;

        /* 【3.5.18】迟到判死与预算判定都提到了抢泳道之前。
         * 旧顺序是「先 dm_take_lane（写泳道占用表）→ 再看预算」，
         * 结果预算耗尽时还有一堆条目占着泳道却没画出去：
         * 泳道表被占满 → 后面更新的条目抢不到位置 → 300ms 后全部判死，
         * 而那些老条目下一帧依旧没预算。得不到泳道的新条目只能等，
         * 等到预算腾出来时 elapsed 已经几百毫秒，于是「半路冒出」。
         * 现在先把不可能画出去的排除掉，泳道只分给真要画的条目。 */
        if (it->lane < 0 && elapsed > DM_LATE_MS) {
            it->lane = DM_LANE_DEAD;
            dead++;
            continue;
        }
        if (drawn >= g_budget) {
            /* 预算用完：本帧不再抢泳道，也不给它预留位置。
             * 下一帧只要还在 DM_LATE_MS 之内就能重新排队。 */
            hungry++;
            continue;
        }
        if (it->lane < 0) {
            if (!dm_take_lane(it, kind, lanes, pos_ms)) {
                /* 泳道排满：下一帧再试。计进 visible 但不算 drawn，
                 * 诊断时能一眼看出「是没数据还是没位置」。 */
                visible++;
                continue;
            }
        }
        visible++;

        gl = dm_glyph_get(it);
        if (!gl) continue;

        if (kind == DM_KIND_SCROLL) {
            /* x = w - (elapsed / DM_SCROLL_MS) * (w + text_w)：
             * elapsed=0 时整条在右边缘外，elapsed=DM_SCROLL_MS 时整条出左边 */
            x = lw - (int)((long long)elapsed * (long long)(lw + gl->bw) / DM_SCROLL_MS);
            y = it->lane * lane_h + (lane_h - gl->bh) / 2;
        } else if (kind == DM_KIND_TOP) {
            x = (lw - gl->bw) / 2;
            y = it->lane * lane_h + (lane_h - gl->bh) / 2;
        } else {
            x = (lw - gl->bw) / 2;
            y = avail - (it->lane + 1) * lane_h + (lane_h - gl->bh) / 2;
        }

        /* 【3.5.18】记下首次真正画出去时的 elapsed。
         * 这个值就是「这条弹幕从右边缘进场时已经迟了多少毫秒」：
         * > DM_LATE_ENTRY_MS 就意味着它是在画面中段凭空出现的。 */
        if (it->first_ms < 0) {
            it->first_ms = (int)elapsed;
            if (elapsed > DM_LATE_ENTRY_MS) late_entry++;
        }

        dm_blit(yp, uv, w, h, rot, gl, x, y);
        drawn++;
    }

    g_stats.visible = visible;
    g_stats.drawn = drawn;
    g_stats.last_us = pb2_now_us() - t0;

    /* 自适应预算：吃力就减，宽裕就加。一次只动 1~2 条，避免画面上
     * 条数来回突变（那是"闪"而不是"卡"）。 */
    if (g_stats.last_us > DM_SLOW_US) {
        g_budget -= 2;
        if (g_budget < DM_BUDGET_MIN) g_budget = DM_BUDGET_MIN;
    } else if (g_stats.last_us < DM_FAST_US) {
        if (g_budget < DM_MAX_ONSCREEN) g_budget++;
    }

    if (drawn > 0 && !g_dm_logged) {
        g_dm_logged = 1;
        pb2_logf("dm: first render %dx%d rotate=%d lanes=%d took=%lldus",
                 w, h, rot, lanes, g_stats.last_us);
    }
    /* 每 5 秒一行运行期统计：drawn/avail 能一眼看出「没数据」还是「泳道排满了」，
     * took 是单帧合成耗时（预算 33ms）。只在真开着的时候打，关弹幕零日志。 */
    if (pos_ms - g_stat_ms >= DM_STAT_MS) {
        g_stat_ms = pos_ms;
        pb2_logf("dm: %dx%d rot=%d mode=%d px=%d lanes=%d vis=%d drawn=%d late=%d hungry=%d mid=%d budget=%d items=%d took=%lldus",
                 w, h, rot, g_mode, g_font_px, lanes, g_stats.visible, drawn,
                 dead, hungry, late_entry, g_budget, g_count, g_stats.last_us);
        dead = 0;
        hungry = 0;
        late_entry = 0;
    }
    return drawn;
}

/* ============================== 收尾 ============================== */

static void dm_shutdown_locked(void) {
    dm_cache_clear();   /* 缓存里的 surface 必须先于 dlclose 销毁 */
    dm_clear_locked();  /* 注意：这里是 locked 版本，dm_clear() 会自己加锁 → 死锁 */
    dm_release_all();

    g_route = 0;
    g_family[0] = '\0';
    g_font_name = "none";
    g_ready = 0;
    g_failed = 0;           /* 关键：允许下一次 PB2_CMD_DM_MODE 重新 dm_init() */
    g_dm_logged = 0;
    g_font_px = DM_DEFAULT_FONT_PX;
    g_lane_count = 0;
    g_last_pos = -1;
    g_stat_ms = 0;
    g_budget = DM_MAX_ONSCREEN;
    g_stats.visible = 0;
    g_stats.drawn = 0;
    g_stats.last_us = 0;
}

/* ============================== 公开入口（加锁） ============================== */

/* dm_init / dm_shutdown 由 worker 线程在 DM_MODE / RELEASE 命令里调用，但它们
 * 会同 dm_cache_clear()、g_ready、g_font_px 这些 render 也会读的状态打交道，
 * 所以同样走 g_lock。它们不会在 streaming 线程的 probe 里被调用，不存在
 * 与 render 互相等待的第二把锁。 */

int dm_init(void) {
    int rc;
    pthread_mutex_lock(&g_lock);
    rc = dm_init_locked();
    pthread_mutex_unlock(&g_lock);
    return rc;
}

void dm_shutdown(void) {
    pthread_mutex_lock(&g_lock);
    dm_shutdown_locked();
    pthread_mutex_unlock(&g_lock);
}
