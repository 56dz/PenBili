/*
 * gst_min.h - 手写的最小 GStreamer 1.22 声明（player2 专用）
 *
 * 为什么手写而不是用官方头文件：
 *   1) 设备 SDK 里没有 gstreamer dev 包，交叉编译环境也拿不到；
 *   2) 我们只需要二十来个函数，手写声明零依赖、编译快；
 *   3) 全部声明都经过真机 libgstreamer-1.0.so.0.2200.0 的 .dynsym 核对（见
 *      docs/audit/spike-report.md §6）。
 *
 * 铁律：
 *   - 所有 GstXxx 结构体都是不透明类型，只能通过 API 访问。
 *     绝对不允许自己定义结构体布局 / 算字段偏移，一旦 GStreamer 版本变了就崩。
 *   - 唯一可以自己定义的是 GError（3 个字段顺序在 aarch64 上已实测固定）。
 *   - 常量值全部来自 gst 官方头文件的稳定 ABI，不可改动。
 */
#ifndef PENBILI_GST_MIN_H
#define PENBILI_GST_MIN_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------- 不透明类型 ---------- */
typedef struct _GstElement GstElement;
typedef struct _GstBus GstBus;
typedef struct _GstMessage GstMessage;
typedef struct _GstPad GstPad;
typedef struct _GstPadProbeInfo GstPadProbeInfo;
typedef struct _GstCaps GstCaps;
typedef struct _GstStructure GstStructure;
typedef struct _GstBin GstBin;

/* ---------- 基础标量（宽度在 LP64 上与 gst 一致） ---------- */
typedef int gboolean;
typedef unsigned long gulong;
typedef unsigned long long GstClockTime;
typedef long long GstClockTimeDiff;

typedef int GstState;
typedef int GstStateChangeReturn;
typedef int GstPadProbeReturn;
typedef int GstFormat;
typedef int GstSeekFlags;
typedef int GstMessageType;
typedef int GstParseFlags;

/* GError 是唯一允许自定义布局的结构体：domain/code/message 三个字段顺序固定。 */
typedef struct _GError {
    unsigned int domain;
    int code;
    char *message;
} GError;

/* ---------- 常量（gst 稳定 ABI） ---------- */
#define GST_CLOCK_TIME_NONE ((GstClockTime)-1)

/* GstState */
#define GST_STATE_VOID_PENDING 0
#define GST_STATE_NULL 1
#define GST_STATE_READY 2
#define GST_STATE_PAUSED 3
#define GST_STATE_PLAYING 4

/* GstStateChangeReturn */
#define GST_STATE_CHANGE_FAILURE 0
#define GST_STATE_CHANGE_SUCCESS 1
#define GST_STATE_CHANGE_ASYNC 2
#define GST_STATE_CHANGE_NO_PREROLL 3

/* GstPadProbeReturn */
#define GST_PAD_PROBE_DROP 0
#define GST_PAD_PROBE_OK 1
#define GST_PAD_PROBE_REMOVE 2
#define GST_PAD_PROBE_PASS 3
#define GST_PAD_PROBE_HANDLED 4

/* GstPadProbeType */
#define GST_PAD_PROBE_TYPE_BUFFER 16
/*
 * 【音量统计冻结修复】demuxer 对 AAC 推 GST_BUFFER_LIST 时需要这个 flag。
 * 值必须与 gstreamer 1.22 的 GstPadProbeType 枚举一致：
 *   BUFFER      = 1 << 4 = 16
 *   BUFFER_LIST = 1 << 5 = 32
 *   EVENT_DOWNSTREAM = 1 << 6 = 64
 *   EVENT_UPSTREAM   = 1 << 7 = 128
 * 踩过的坑：一开始写成 128（那是 EVENT_UPSTREAM），probe 永远不触发，
 * ab 恒 0，看起来像"修复无效"。
 */
#define GST_PAD_PROBE_TYPE_BUFFER_LIST 32

/* GstFormat */
#define GST_FORMAT_TIME 3

/* GstSeekFlags */
#define GST_SEEK_FLAG_FLUSH (1 << 0)
#define GST_SEEK_FLAG_ACCURATE (1 << 1)
#define GST_SEEK_FLAG_KEY_UNIT (1 << 2)
#define GST_SEEK_FLAG_SEGMENT (1 << 3)
#define GST_SEEK_FLAG_SKIP (1 << 4)

/* GstMessageType */
#define GST_MESSAGE_EOS (1 << 0)
#define GST_MESSAGE_ERROR (1 << 1)
#define GST_MESSAGE_WARNING (1 << 2)
#define GST_MESSAGE_INFO (1 << 3)
#define GST_MESSAGE_BUFFERING (1 << 5)
#define GST_MESSAGE_STATE_CHANGED (1 << 6)
#define GST_MESSAGE_CLOCK_LOST (1 << 10)
#define GST_MESSAGE_ASYNC_DONE (1 << 21)

/* ---------- 核心 ---------- */
extern void gst_init(int *argc, char ***argv);
extern gboolean gst_init_check(int *argc, char ***argv, GError **error);
extern const char *gst_version_string(void);
extern void gst_deinit(void);
extern void gst_debug_set_threshold_from_string(const char *list, gboolean reset);

/* ---------- 元素 / 管线 ---------- */
extern GstElement *gst_pipeline_new(const char *name);
extern GstElement *gst_element_factory_make(const char *factoryname, const char *name);
extern gboolean gst_bin_add(GstBin *bin, GstElement *element);
/* 【P0-4】把已经 add 进去的 bin 摘掉：pad-added 的早退路径必须用，
 * 否则 pipeline 仍持有一份引用，坏分支会一直挂在管线里。 */
extern gboolean gst_bin_remove(GstBin *bin, GstElement *element);
extern GstElement *gst_bin_get_by_name(GstBin *bin, const char *name);
extern gboolean gst_element_link(GstElement *src, GstElement *dest);
extern gboolean gst_element_link_pads(GstElement *src, const char *srcpadname,
                                      GstElement *dest, const char *destpadname);
extern GstElement *gst_parse_launch(const char *pipeline_description, GError **error);
extern GstElement *gst_parse_bin_from_description(const char *bin_description,
                                                  gboolean ghost_unlinked_pads,
                                                  GError **error);
extern GstStateChangeReturn gst_element_set_state(GstElement *element, GstState state);
extern GstStateChangeReturn gst_element_get_state(GstElement *element, GstState *state,
                                                  GstState *pending, GstClockTime timeout);
extern gboolean gst_element_sync_state_with_parent(GstElement *element);
extern GstBus *gst_element_get_bus(GstElement *element);

/* ---------- 查询 / seek ---------- */
extern gboolean gst_element_query_position(GstElement *element, GstFormat format, long long *cur);
extern gboolean gst_element_query_duration(GstElement *element, GstFormat format, long long *duration);
extern gboolean gst_element_seek_simple(GstElement *element, GstFormat format,
                                        GstSeekFlags seek_flags, long long seek_pos);

/* ---------- Bus / Message ---------- */
extern GstMessage *gst_bus_timed_pop_filtered(GstBus *bus, GstClockTime timeout,
                                              GstMessageType types);
extern void gst_bus_set_flushing(GstBus *bus, gboolean flushing);
extern void gst_message_parse_error(GstMessage *message, GError **gerror, char **debug);
extern const char *gst_message_type_get_name(GstMessageType type);
/*
 * gst_message_get_structure 返回的消息名就是消息类型名（"error"/"eos"/
 * "async-done"/"state-changed"…）。这是**唯一可靠的消息类型判别方式**：
 * gst_bus_timed_pop_filtered 只返回消息本身，我们手写声明里没有
 * gst_message_get_type 可用的 GType 表，而 gst_message_parse_error 对非 ERROR
 * 消息的行为是实现细节（GStreamer 1.22 会打 warning 且返回 NULL）。
 * 实测设备库 libgstreamer-1.0.so.0 有该符号。
 */
extern GstStructure *gst_message_get_structure(GstMessage *message);
extern const char *gst_message_type_get_name(GstMessageType type);
extern void gst_message_unref(GstMessage *message);

/* ---------- Pad / Caps / Buffer ---------- */
extern GstPad *gst_element_get_static_pad(GstElement *element, const char *name);
extern int gst_pad_link(GstPad *srcpad, GstPad *sinkpad);
extern gulong gst_pad_add_probe(GstPad *pad, int mask,
                                GstPadProbeReturn (*callback)(GstPad *, GstPadProbeInfo *, void *),
                                void *user_data, void (*destroy_data)(void *));
extern void gst_pad_remove_probe(GstPad *pad, gulong id);
extern GstCaps *gst_pad_get_current_caps(GstPad *pad);
extern GstCaps *gst_pad_query_caps(GstPad *pad, GstCaps *filter);
extern unsigned int gst_caps_get_size(const GstCaps *caps);
extern GstStructure *gst_caps_get_structure(const GstCaps *caps, unsigned int index);
extern const char *gst_structure_get_name(const GstStructure *structure);
extern gboolean gst_structure_get_int(const GstStructure *structure, const char *fieldname, int *value);
extern const char *gst_structure_get_string(const GstStructure *structure, const char *fieldname);
extern void gst_caps_unref(GstCaps *caps);
extern void *gst_pad_probe_info_get_buffer(GstPadProbeInfo *info);
/* 【音量统计冻结修复】GstBufferList 相关（gstreamer 1.22 实符号） */
extern void *gst_pad_probe_info_get_buffer_list(GstPadProbeInfo *info);
extern unsigned int gst_buffer_list_length(void *list);
extern void *gst_buffer_list_get(void *list, unsigned int idx);
extern size_t gst_buffer_get_size(void *buffer);

/* ---------- GObject 基础 ---------- */
extern void gst_object_unref(void *object);
extern const char *gst_object_get_name(void *object);
extern void gst_util_set_object_arg(void *object, const char *name, const char *value);
extern void g_object_set(void *object, const char *first_property_name, ...);
extern void g_object_unref(void *object);
extern gulong g_signal_connect_data(void *instance, const char *detailed_signal,
                                    void *c_handler, void *data,
                                    void *destroy_data, int connect_flags);

/* ---------- GLib 基础 ---------- */
extern void g_error_free(GError *error);
extern void g_free(void *mem);

#ifdef __cplusplus
}
#endif

#endif /* PENBILI_GST_MIN_H */
