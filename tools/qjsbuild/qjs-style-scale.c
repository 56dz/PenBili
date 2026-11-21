/* qjs-style-scale —— 改写 QuickJS .js.bin 里 style 对象的尺寸/字号原子。
 *
 * Usage: qjs-style-scale <input.bin> <output.bin>
 *
 * 目的（PenBili on CV1826 640x260 面板）：
 *   JS 按 X6 Pro 设计 960x266，Falcon setViewPort(960) 把它缩到 640x177，
 *   屏幕下方留 83px 黑边。这里把"竖向"尺寸按 260/177≈1.467 放大到 390 设计高
 *   （390*2/3=260，刚好铺满面板），字号默认保持设计值。
 *   横向 width 不动（仍按 960 设计，横向缩放 2/3 不变）。
 *   字号默认保持设计值（font-scale 1.0），需要时可用 --font-scale 放大。
 *
 * 规则（只改 `push_atom_value "..."` 且下一条指令是 define_field 的样式值）：
 *   - fontSize        : ×font-scale（默认 1.0，不放大；1.5/2.0 为可选放大档）
 *   - height >= 70px  : 按 height-scale（默认 390/266）放大
 *   - height < 70px   : 不动（图标/进度条小元件保持方形与比例）
 *   其它属性一律不动。
 */
#define CONFIG_VERSION "2020-07-05"
#include "quickjs.c"
#include <math.h>

/* 可调开关（命令行）：
 *   --font-scale <f>     字号倍率，默认 2.0（同步前的大字号 UI）；1.0 = 原字号
 *   --height-scale <f>   高度倍率，默认 390/266；1.0 = 不放大
 *   --no-height          等价于 --height-scale 1.0 */
static double g_font_scale = 2.0;
static double g_height_scale = 390.0 / 266.0;
static int g_height_enabled = 1;
static int g_default_heights = 1;

static uint8_t *read_file(JSContext *ctx, const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    long size;
    uint8_t *buf;
    if (!f)
        return NULL;
    if (fseek(f, 0, SEEK_END) != 0 || (size = ftell(f)) < 0 ||
        fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }
    buf = js_malloc(ctx, (size_t)size + 1);
    if (!buf || fread(buf, 1, (size_t)size, f) != (size_t)size) {
        js_free(ctx, buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    buf[size] = '\0';
    *len = (size_t)size;
    return buf;
}

static int scale_height(int v, int *changed)
{
    int nv = v;
    if (!g_height_enabled || g_height_scale <= 0) return v;
    if (g_default_heights) {
        /* 390/264/242 是「新布局最终设计值」（详情页封面区/封面、两列大卡片封面）；
         * 96/216/78 是「全屏播放页」顶栏/视频带/底栏的最终设计高，都不能再乘 390/266。 */
        if (v == 390 || v == 264 || v == 242) return v;
        if (v == 96 || v == 216 || v == 78) return v;
        if (v >= 70) {
            switch (v) {
            case 266: nv = 390; break;
            case 240: nv = 364; break;   /* 364 + 26(seek/输入行) = 390 */
            case 192: nv = 282; break;   /* 282 + 108(按钮区) = 390 */
            case 74:  nv = 108; break;
            case 160: nv = 235; break;
            case 220: nv = 323; break;
            case 232: nv = 341; break;
            case 200: nv = 294; break;
            case 150: nv = 220; break;
            default:  nv = (int)lround((double)v * g_height_scale); break;
            }
        }
    } else if (v >= 70) {
        nv = (int)lround((double)v * g_height_scale);
    }
    if (nv != v) *changed = 1;
    return nv;
}

static int scale_font(int v)
{
    /* Falcon 把 960 逻辑宽缩到 640（2/3），物理字号 = 源字号 × 倍率 × 2/3。
     * 默认 2.0 ≈ 1.33× 源字号；1.0 = 原字号（只缩放高度，不放大字）。 */
    if (g_font_scale <= 0) return v;
    return (int)lround((double)v * g_font_scale);
}

static void patch_func(JSContext *ctx, JSFunctionBytecode *b, int *changes)
{
    uint8_t *tab = b->byte_code_buf;
    int len = b->byte_code_len;
    int pos = 0;
    int i;

    while (pos < len) {
        int op = tab[pos];
        const JSOpCode *oi = &short_opcode_info(op);
        if (op == OP_push_atom_value) {
            int npos = pos + oi->size;
            if (npos + 5 <= len && tab[npos] == OP_define_field) {
                JSAtom vatom = get_u32(tab + pos + 1);
                JSAtom katom = get_u32(tab + npos + 1);
                const char *val = JS_AtomToCString(ctx, vatom);
                const char *key = JS_AtomToCString(ctx, katom);
                if (val && key) {
                    size_t n = strlen(val);
                    if (n >= 3 && n <= 10 && strcmp(val + n - 2, "px") == 0) {
                        int v = atoi(val);
                        int nv = v;
                        int is_font = strcmp(key, "fontSize") == 0;
                        int is_height = strcmp(key, "height") == 0;
                        if (is_font) nv = scale_font(v);
                        else if (is_height) { int ch = 0; nv = scale_height(v, &ch); }
                        if (nv != v) {
                            char buf[24];
                            JSAtom na;
                            snprintf(buf, sizeof(buf), "%dpx", nv);
                            na = JS_NewAtom(ctx, buf);
                            if (na != JS_ATOM_NULL) {
                                put_u32(tab + pos + 1, na);
                                (*changes)++;
                            }
                        }
                    }
                }
                if (val) JS_FreeCString(ctx, val);
                if (key) JS_FreeCString(ctx, key);
            }
        }
        pos += oi->size;
    }

    for (i = 0; i < b->cpool_count; i++) {
        if (JS_VALUE_GET_TAG(b->cpool[i]) == JS_TAG_FUNCTION_BYTECODE)
            patch_func(ctx, JS_VALUE_GET_PTR(b->cpool[i]), changes);
    }
}

int main(int argc, char **argv)
{
    JSRuntime *rt;
    JSContext *ctx;
    JSValue module;
    JSModuleDef *m;
    JSFunctionBytecode *top;
    uint8_t *in, *out;
    size_t in_len, out_len;
    int changes = 0;
    const char *in_path = NULL;
    const char *out_path = NULL;
    int i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--font-scale") == 0 && i + 1 < argc) {
            g_font_scale = strtod(argv[++i], NULL);
            if (!(g_font_scale > 0)) {
                fprintf(stderr, "bad --font-scale\n");
                return 2;
            }
        } else if (strcmp(argv[i], "--height-scale") == 0 && i + 1 < argc) {
            g_height_scale = strtod(argv[++i], NULL);
            if (!(g_height_scale > 0)) {
                fprintf(stderr, "bad --height-scale\n");
                return 2;
            }
            g_height_enabled = 1;
            g_default_heights = fabs(g_height_scale - 390.0 / 266.0) < 1e-9;
        } else if (strcmp(argv[i], "--no-height") == 0) {
            g_height_enabled = 0;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("usage: %s [options] input.bin output.bin\n"
                   "  --font-scale <f>     font multiplier (default 2.0, 1.0 = off)\n"
                   "  --height-scale <f>   height multiplier (default 390/266)\n"
                   "  --no-height          same as --height-scale 1.0\n",
                   argv[0]);
            return 0;
        } else if (!in_path) {
            in_path = argv[i];
        } else if (!out_path) {
            out_path = argv[i];
        } else {
            fprintf(stderr, "too many arguments: %s\n", argv[i]);
            return 2;
        }
    }
    if (!in_path || !out_path) {
        fprintf(stderr, "usage: %s [--font-scale f] [--height-scale f|--no-height] input.bin output.bin\n", argv[0]);
        return 2;
    }

    rt = JS_NewRuntime();
    ctx = JS_NewContext(rt);
    if (!rt || !ctx) {
        fprintf(stderr, "cannot create runtime\n");
        return 2;
    }

    in = read_file(ctx, in_path, &in_len);
    if (!in) {
        fprintf(stderr, "cannot read %s\n", in_path);
        return 2;
    }
    module = JS_ReadObject(ctx, in, in_len, JS_READ_OBJ_BYTECODE);
    js_free(ctx, in);
    if (JS_IsException(module) ||
        JS_VALUE_GET_TAG(module) != JS_TAG_MODULE) {
        fprintf(stderr, "cannot parse module bytecode\n");
        return 1;
    }
    m = JS_VALUE_GET_PTR(module);
    top = JS_VALUE_GET_PTR(m->func_obj);

    patch_func(ctx, top, &changes);
    printf("patched %d style atoms (font x%.2f, height ", changes, g_font_scale);
    if (g_height_enabled) printf("x%.3f)\n", g_height_scale);
    else printf("off)\n");

    out = JS_WriteObject(ctx, &out_len, module, JS_WRITE_OBJ_BYTECODE);
    if (!out) {
        fprintf(stderr, "serialize failed\n");
        return 1;
    }
    {
        FILE *f = fopen(out_path, "wb");
        if (!f || fwrite(out, 1, out_len, f) != out_len) {
            fprintf(stderr, "cannot write %s\n", out_path);
            return 1;
        }
        fclose(f);
    }
    printf("wrote %s (%lu bytes)\n", out_path, (unsigned long)out_len);
    return 0;
}
