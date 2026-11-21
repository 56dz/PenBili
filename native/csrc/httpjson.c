/*
 * httpjson —— 有道词典笔 Falcon 原生 JSAPI（纯 C）
 *
 * 存在原因：运行时的 $falcon.jsapi.http.request 会对 data 字符串做二次 JSON 序列化
 * （httpbin 实测：Content-Length 明显大于原始 JSON，服务端 json 字段收到的是字符串），
 * 也无法通过任何开关发送原始请求体，导致 DeepSeek/GLM 等 OpenAI 兼容 API 返回 422。
 *
 * 本模块直接调用设备自带的 libcurl，把 JS 传入的字符串原样作为 POST body 发送。
 *
 * JS 用法：
 *   import { postSync } from 'httpjson'
 *   const r = postSync(url, "Key: value\r\nKey2: v2", bodyString, timeoutMs)
 *   // r = { status:number, body:string, error:string|null, curlCode:number }
 *
 * 异步接口（页面不卡死的关键）：
 *   const id = startPost(url, headers, body, timeoutMs)   // 立即返回，后台线程发请求
 *   const raw = takeResult(id)                            // 轮询：null=未完成，字符串=JSON 结果
 *   cancelAll()                                           // 页面销毁时丢弃所有在途请求
 * 同步接口 postSync 保留给诊断使用。
 *
 * 纯 C 实现（不使用 C++ 标准库），避免与设备上的 libstdc++ ABI 耦合。
 */

#include <jquick_config.h>
#include <jsmodules/JSCModuleExtension.h>
#include <quickjs/quickjs.h>

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/* ---------------- libcurl 最小声明（无需 curl 头文件） ---------------- */
typedef void CURL;
struct curl_slist;
typedef struct curl_slist CURL_slist;

extern CURL *curl_easy_init(void);
extern void curl_easy_cleanup(CURL *handle);
extern int curl_easy_setopt(CURL *handle, int option, ...);
extern int curl_easy_perform(CURL *handle);
extern int curl_easy_getinfo(CURL *handle, int info, ...);
extern CURL_slist *curl_slist_append(CURL_slist *list, const char *s);
extern void curl_slist_free_all(CURL_slist *list);
extern const char *curl_easy_strerror(int code);

#define CURLOPT_WRITEDATA 10001
#define CURLOPT_URL 10002
#define CURLOPT_POSTFIELDS 10015
#define CURLOPT_HTTPHEADER 10023
#define CURLOPT_WRITEFUNCTION 20011
#define CURLOPT_TIMEOUT_MS 155
#define CURLOPT_CONNECTTIMEOUT_MS 156
#define CURLOPT_FOLLOWLOCATION 52
#define CURLOPT_SSL_VERIFYPEER 64
#define CURLOPT_SSL_VERIFYHOST 81
#define CURLOPT_NOSIGNAL 99
#define CURLOPT_POST 47
#define CURLOPT_POSTFIELDSIZE 60
#define CURLOPT_ACCEPT_ENCODING 10102
#define CURLINFO_RESPONSE_CODE 0x200002

struct resp_buf {
    char *data;
    size_t len;
    size_t cap;
};

static size_t write_cb(char *ptr, size_t size, size_t nmemb, void *userdata) {
    struct resp_buf *b = (struct resp_buf *)userdata;
    size_t n = size * nmemb;
    if (b->len + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 4096;
        while (cap < b->len + n + 1) cap *= 2;
        char *p = (char *)realloc(b->data, cap);
        if (!p) return 0;
        b->data = p;
        b->cap = cap;
    }
    memcpy(b->data + b->len, ptr, n);
    b->len += n;
    b->data[b->len] = '\0';
    return n;
}

static char *dup_str(const char *s) {
    size_t n;
    char *p;
    if (!s) return NULL;
    n = strlen(s) + 1;
    p = (char *)malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

/* headersText: "Key: value\nKey2: value2"（JS 侧用 \r\n 或 \n 拼接均可）
 * insecure: 1 = 跳过 TLS 校验（设备无 CA 库时的回退路径，结果中会标记） */
static int do_post(const char *url, const char *headersText, const char *body, long bodyLen,
                   long timeoutMs, int insecure, struct resp_buf *out, long *status,
                   int *curlCode, char **errMsg) {
    CURL *h;
    CURL_slist *list = NULL;
    int code;

    *status = 0;
    *curlCode = 0;
    *errMsg = NULL;

    h = curl_easy_init();
    if (!h) {
        *errMsg = dup_str("curl_easy_init failed");
        return -1;
    }
    if (headersText && headersText[0]) {
        char *copy = dup_str(headersText);
        if (copy) {
            char *save = NULL;
            char *line = strtok_r(copy, "\n", &save);
            while (line) {
                size_t l = strlen(line);
                while (l && (line[l - 1] == '\r' || line[l - 1] == ' ')) line[--l] = '\0';
                if (l) list = curl_slist_append(list, line);
                line = strtok_r(NULL, "\n", &save);
            }
            free(copy);
        }
    }

    curl_easy_setopt(h, CURLOPT_URL, url);
    if (list) curl_easy_setopt(h, CURLOPT_HTTPHEADER, list);
    curl_easy_setopt(h, CURLOPT_POST, 1L);
    curl_easy_setopt(h, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(h, CURLOPT_POSTFIELDSIZE, bodyLen);
    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(h, CURLOPT_WRITEDATA, out);
    curl_easy_setopt(h, CURLOPT_TIMEOUT_MS, timeoutMs);
    curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT_MS, 15000L);
    curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(h, CURLOPT_NOSIGNAL, 1L);
    /* Accept-Encoding 不再协商：A-E 头会命中 article/view 等接口的风控连接拒绝（2026-10-04 实测），JSON 无压缩需求 */
    /* curl_easy_setopt(h, CURLOPT_ACCEPT_ENCODING, ""); */
    if (insecure) {
        curl_easy_setopt(h, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(h, CURLOPT_SSL_VERIFYHOST, 0L);
    } else {
        curl_easy_setopt(h, CURLOPT_SSL_VERIFYPEER, 1L);
        curl_easy_setopt(h, CURLOPT_SSL_VERIFYHOST, 2L);
    }

    code = curl_easy_perform(h);
    *curlCode = code;
    if (code == 0) {
        curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, status);
    } else {
        char tmp[128];
        const char *msg = curl_easy_strerror(code);
        snprintf(tmp, sizeof(tmp), "curl#%d: %s", code, msg ? msg : "unknown");
        *errMsg = dup_str(tmp);
    }

    if (list) curl_slist_free_all(list);
    curl_easy_cleanup(h);
    return code == 0 ? 0 : -1;
}

/* GET（弹幕 protobuf 等二进制下载）：结构同 do_post 但**不设 CURLOPT_POST**（默认 GET），
 * write_cb 无损累积任意字节；JS_NewArrayBufferCopy 承载（无 \0 截断风险）。 */
static int do_get(const char *url, const char *headersText, long timeoutMs, int insecure,
                  struct resp_buf *out, long *status, int *curlCode, char **errMsg) {
    CURL *h;
    CURL_slist *list = NULL;
    int code;

    *status = 0;
    *curlCode = 0;
    *errMsg = NULL;

    h = curl_easy_init();
    if (!h) {
        *errMsg = dup_str("curl_easy_init failed");
        return -1;
    }
    if (headersText && headersText[0]) {
        char *copy = dup_str(headersText);
        if (copy) {
            char *save = NULL;
            char *line = strtok_r(copy, "\n", &save);
            while (line) {
                size_t l = strlen(line);
                while (l && (line[l - 1] == '\r' || line[l - 1] == ' ')) line[--l] = '\0';
                if (l) list = curl_slist_append(list, line);
                line = strtok_r(NULL, "\n", &save);
            }
            free(copy);
        }
    }

    curl_easy_setopt(h, CURLOPT_URL, url);
    if (list) curl_easy_setopt(h, CURLOPT_HTTPHEADER, list);
    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(h, CURLOPT_WRITEDATA, out);
    curl_easy_setopt(h, CURLOPT_TIMEOUT_MS, timeoutMs);
    curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT_MS, 15000L);
    curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(h, CURLOPT_NOSIGNAL, 1L);
    /* Accept-Encoding 不再协商：A-E 头会命中 article/view 等接口的风控连接拒绝（2026-10-04 实测），JSON 无压缩需求 */
    /* curl_easy_setopt(h, CURLOPT_ACCEPT_ENCODING, ""); */
    if (insecure) {
        curl_easy_setopt(h, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(h, CURLOPT_SSL_VERIFYHOST, 0L);
    } else {
        curl_easy_setopt(h, CURLOPT_SSL_VERIFYPEER, 1L);
        curl_easy_setopt(h, CURLOPT_SSL_VERIFYHOST, 2L);
    }

    code = curl_easy_perform(h);
    *curlCode = code;
    if (code == 0) {
        curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, status);
    } else {
        char tmp[128];
        const char *msg = curl_easy_strerror(code);
        snprintf(tmp, sizeof(tmp), "curl#%d: %s", code, msg ? msg : "unknown");
        *errMsg = dup_str(tmp);
    }

    if (list) curl_slist_free_all(list);
    curl_easy_cleanup(h);
    return code == 0 ? 0 : -1;
}

/* 证书类错误（设备无 CA 库时命中） */
static int is_cert_error(int code) {
    switch (code) {
        case 35: /* SSL connect error */
        case 51: /* peer certificate cannot be authenticated */
        case 58: /* problem with local certificate */
        case 59: /* could not use specified SSL cipher */
        case 60: /* peer certificate cannot be authenticated with known CA */
        case 66: /* SSL engine init failed */
        case 77: /* problem with the SSL CA cert (path? access rights?) */
        case 83: /* issuer check failed */
        case 90: /* OCSP response invalid */
        case 91: /* OCSP response error */
            return 1;
        default:
            return 0;
    }
}

/* ---------------- JS 绑定 ---------------- */

static JSValue build_result(JSContext *ctx, struct resp_buf *b, long status, int curlCode,
                            const char *errMsg, int insecure) {
    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "status", JS_NewInt32(ctx, (int)status));
    JS_SetPropertyStr(ctx, obj, "body", JS_NewStringLen(ctx, b->data ? b->data : "", b->len));
    JS_SetPropertyStr(ctx, obj, "curlCode", JS_NewInt32(ctx, curlCode));
    JS_SetPropertyStr(ctx, obj, "insecure", JS_NewBool(ctx, insecure ? 1 : 0));
    if (errMsg) JS_SetPropertyStr(ctx, obj, "error", JS_NewString(ctx, errMsg));
    else JS_SetPropertyStr(ctx, obj, "error", JS_NULL);
    return obj;
}

/* postSync(url, headers, body, timeoutMs[, mode])
 *   mode: 0/缺省 = 先校验证书，遇证书错误自动回退（结果 insecure=true）
 *         1       = 直接跳过证书校验（诊断用）
 *         2       = 严格校验，不回退
 * 返回 { status, body, error, curlCode, insecure }
 */
static JSValue js_post_sync(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    const char *url = NULL;
    const char *headers = NULL;
    const char *body = NULL;
    size_t bodyLen = 0;
    int timeoutMs = 60000;
    int mode = 0;
    int insecure = 0;
    struct resp_buf b;
    long status = 0;
    int curlCode = 0;
    char *errMsg = NULL;
    JSValue res;

    (void)this_val;
    if (argc < 3) {
        return JS_ThrowTypeError(ctx, "httpjson.postSync(url, headers, body, timeoutMs[, mode])");
    }
    url = JS_ToCString(ctx, argv[0]);
    if (!url) return JS_EXCEPTION;
    if (!JS_IsUndefined(argv[1]) && !JS_IsNull(argv[1])) headers = JS_ToCString(ctx, argv[1]);
    body = JS_ToCStringLen(ctx, &bodyLen, argv[2]);
    if (!body) {
        JS_FreeCString(ctx, url);
        if (headers) JS_FreeCString(ctx, headers);
        return JS_EXCEPTION;
    }
    if (argc > 3) JS_ToInt32(ctx, &timeoutMs, argv[3]);
    if (timeoutMs <= 0) timeoutMs = 60000;
    if (argc > 4) JS_ToInt32(ctx, &mode, argv[4]);

    memset(&b, 0, sizeof(b));
    if (mode == 1) insecure = 1;
    do_post(url, headers ? headers : "", body, (long)bodyLen, (long)timeoutMs, insecure,
            &b, &status, &curlCode, &errMsg);

    if (errMsg && mode == 0 && is_cert_error(curlCode)) {
        /* 设备无 CA 库：按运行时同样不校验的方式重试一次，并在结果中标记 */
        if (b.data) {
            free(b.data);
            memset(&b, 0, sizeof(b));
        }
        free(errMsg);
        errMsg = NULL;
        do_post(url, headers ? headers : "", body, (long)bodyLen, (long)timeoutMs, 1,
                &b, &status, &curlCode, &errMsg);
        insecure = 1;
    }

    res = build_result(ctx, &b, status, curlCode, errMsg, insecure);

    if (b.data) free(b.data);
    if (errMsg) free(errMsg);
    JS_FreeCString(ctx, url);
    if (headers) JS_FreeCString(ctx, headers);
    JS_FreeCString(ctx, body);
    return res;
}

/* ---------------- 异步任务：后台线程发请求，JS 轮询取结果 ---------------- */

/* getBinary(url, headers, timeoutMs) -> {ok, status, len, insecure, error, buf:ArrayBuffer}
 * 二进制安全出口：弹幕 seg.so(1.2MB protobuf) 等——jsapi http 实测把 body 截成 2 字节，不可用。
 * 证书错误按 postSync 同策略回退一次（结果 insecure=true）。 */
static JSValue js_get_binary(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    const char *url = NULL;
    const char *headers = NULL;
    int timeoutMs = 30000;
    int insecure = 0;
    struct resp_buf b;
    long status = 0;
    int curlCode = 0;
    char *errMsg = NULL;
    JSValue obj;

    (void)this_val;
    if (argc < 1) return JS_ThrowTypeError(ctx, "httpjson.getBinary(url, headers, timeoutMs)");
    url = JS_ToCString(ctx, argv[0]);
    if (!url) return JS_EXCEPTION;
    if (argc > 1 && !JS_IsUndefined(argv[1]) && !JS_IsNull(argv[1])) headers = JS_ToCString(ctx, argv[1]);
    if (argc > 2) JS_ToInt32(ctx, &timeoutMs, argv[2]);
    if (timeoutMs <= 0) timeoutMs = 30000;

    memset(&b, 0, sizeof(b));
    do_get(url, headers ? headers : "", (long)timeoutMs, 0, &b, &status, &curlCode, &errMsg);
    if (errMsg && is_cert_error(curlCode)) {
        if (b.data) {
            free(b.data);
            memset(&b, 0, sizeof(b));
        }
        free(errMsg);
        errMsg = NULL;
        do_get(url, headers ? headers : "", (long)timeoutMs, 1, &b, &status, &curlCode, &errMsg);
        insecure = 1;
    }

    obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "status", JS_NewInt32(ctx, (int)status));
    JS_SetPropertyStr(ctx, obj, "len", JS_NewInt32(ctx, (int)b.len));
    JS_SetPropertyStr(ctx, obj, "insecure", JS_NewBool(ctx, insecure));
    if (errMsg) {
        JS_SetPropertyStr(ctx, obj, "error", JS_NewString(ctx, errMsg));
        JS_SetPropertyStr(ctx, obj, "ok", JS_NewBool(ctx, 0));
    } else {
        JS_SetPropertyStr(ctx, obj, "error", JS_NULL);
        JS_SetPropertyStr(ctx, obj, "ok", JS_NewBool(ctx, status >= 200 && status < 300));
    }
    if (b.data && b.len > 0) {
        JS_SetPropertyStr(ctx, obj, "buf", JS_NewArrayBufferCopy(ctx, (const uint8_t *)b.data, b.len));
    } else {
        JS_SetPropertyStr(ctx, obj, "buf", JS_UNDEFINED);
    }
    if (b.data) free(b.data);
    if (errMsg) free(errMsg);
    JS_FreeCString(ctx, url);
    if (headers) JS_FreeCString(ctx, headers);
    return obj;
}

struct job {
    int id;
    int state;      /* 0=进行中 1=完成 */
    int cancelled;  /* 页面已销毁/丢弃 */
    char *url;
    char *headers;
    char *body;
    long bodyLen;
    long timeoutMs;
    int mode;
    char *result;   /* 完成后的 JSON 字符串 */
    struct job *next;
};

static struct job *g_jobs = NULL;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static int g_next_id = 1;

static void buf_put(struct resp_buf *b, const char *s, size_t n) {
    if (b->len + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 256;
        while (cap < b->len + n + 1) cap *= 2;
        char *p = (char *)realloc(b->data, cap);
        if (!p) return;
        b->data = p;
        b->cap = cap;
    }
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
}

static void buf_putc2(struct resp_buf *b, char ch) {
    buf_put(b, &ch, 1);
}

static void buf_put_int(struct resp_buf *b, long v) {
    char tmp[32];
    snprintf(tmp, sizeof(tmp), "%ld", v);
    buf_put(b, tmp, strlen(tmp));
}

/* 追加一个 JSON 字符串字面量（含转义） */
static void buf_put_json_string(struct resp_buf *b, const char *s, size_t n) {
    size_t i;
    buf_putc2(b, '"');
    for (i = 0; i < n; i++) {
        unsigned char ch = (unsigned char)s[i];
        switch (ch) {
            case '"': buf_put(b, "\\\"", 2); break;
            case '\\': buf_put(b, "\\\\", 2); break;
            case '\n': buf_put(b, "\\n", 2); break;
            case '\r': buf_put(b, "\\r", 2); break;
            case '\t': buf_put(b, "\\t", 2); break;
            default:
                if (ch < 0x20) {
                    char tmp[8];
                    snprintf(tmp, sizeof(tmp), "\\u%04x", ch);
                    buf_put(b, tmp, strlen(tmp));
                } else {
                    buf_putc2(b, (char)ch);
                }
        }
    }
    buf_putc2(b, '"');
}

static char *marshal_result(long status, int curlCode, const char *errMsg, struct resp_buf *b, int insecure) {
    struct resp_buf out;
    memset(&out, 0, sizeof(out));
    buf_put(&out, "{\"status\":", 10);
    buf_put_int(&out, status);
    buf_put(&out, ",\"curlCode\":", 12);
    buf_put_int(&out, curlCode);
    buf_put(&out, ",\"insecure\":", 12);
    buf_put_int(&out, insecure ? 1 : 0);
    buf_put(&out, ",\"error\":", 9);
    if (errMsg) buf_put_json_string(&out, errMsg, strlen(errMsg));
    else buf_put(&out, "null", 4);
    buf_put(&out, ",\"body\":", 8);
    buf_put_json_string(&out, b->data ? b->data : "", b->len);
    buf_putc2(&out, '}');
    return out.data;
}

static void *job_thread(void *arg) {
    struct job *j = (struct job *)arg;
    struct resp_buf b;
    long status = 0;
    int code = 0;
    char *err = NULL;
    int insecure = (j->mode == 1);
    char *res = NULL;

    memset(&b, 0, sizeof(b));
    do_post(j->url, j->headers, j->body, j->bodyLen, j->timeoutMs, insecure, &b, &status, &code, &err);
    if (err && j->mode == 0 && is_cert_error(code)) {
        /* 设备无 CA 库：按运行时同样方式重试一次（结果标记 insecure） */
        if (b.data) {
            free(b.data);
            memset(&b, 0, sizeof(b));
        }
        free(err);
        err = NULL;
        do_post(j->url, j->headers, j->body, j->bodyLen, j->timeoutMs, 1, &b, &status, &code, &err);
        insecure = 1;
    }
    res = marshal_result(status, code, err, &b, insecure);
    if (b.data) free(b.data);
    if (err) free(err);

    pthread_mutex_lock(&g_mu);
    j->result = res;
    j->state = 1;
    if (j->cancelled) {
        /* 已被取消：直接回收，不留在队列里 */
        struct job **pp = &g_jobs;
        while (*pp && *pp != j) pp = &(*pp)->next;
        if (*pp == j) *pp = j->next;
        free(j->url);
        free(j->headers);
        free(j->body);
        free(j->result);
        free(j);
    }
    pthread_mutex_unlock(&g_mu);
    return NULL;
}

/* startPost(url, headers, body, timeoutMs) -> 任务 id（0 表示失败） */
static JSValue js_start_post(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    const char *url = NULL;
    const char *headers = NULL;
    const char *body = NULL;
    size_t bodyLen = 0;
    int timeoutMs = 60000;
    struct job *j = NULL;
    pthread_t th;

    (void)this_val;
    if (argc < 3) return JS_ThrowTypeError(ctx, "startPost(url, headers, body, timeoutMs)");
    url = JS_ToCString(ctx, argv[0]);
    if (!url) return JS_EXCEPTION;
    if (!JS_IsUndefined(argv[1]) && !JS_IsNull(argv[1])) headers = JS_ToCString(ctx, argv[1]);
    body = JS_ToCStringLen(ctx, &bodyLen, argv[2]);
    if (!body) {
        JS_FreeCString(ctx, url);
        if (headers) JS_FreeCString(ctx, headers);
        return JS_EXCEPTION;
    }
    if (argc > 3) JS_ToInt32(ctx, &timeoutMs, argv[3]);
    if (timeoutMs <= 0) timeoutMs = 60000;

    j = (struct job *)calloc(1, sizeof(struct job));
    if (!j) {
        JS_FreeCString(ctx, url);
        if (headers) JS_FreeCString(ctx, headers);
        JS_FreeCString(ctx, body);
        return JS_ThrowInternalError(ctx, "httpjson: out of memory");
    }
    j->url = dup_str(url);
    j->headers = dup_str(headers ? headers : "");
    j->body = (char *)malloc(bodyLen + 1);
    if (j->body) {
        memcpy(j->body, body, bodyLen);
        j->body[bodyLen] = '\0';
    }
    j->bodyLen = (long)bodyLen;
    j->timeoutMs = (long)timeoutMs;
    j->mode = 0;
    j->state = 0;
    JS_FreeCString(ctx, url);
    if (headers) JS_FreeCString(ctx, headers);
    JS_FreeCString(ctx, body);
    if (!j->url || !j->headers || !j->body) {
        free(j->url);
        free(j->headers);
        free(j->body);
        free(j);
        return JS_ThrowInternalError(ctx, "httpjson: out of memory");
    }

    pthread_mutex_lock(&g_mu);
    j->id = g_next_id++;
    j->next = g_jobs;
    g_jobs = j;
    pthread_mutex_unlock(&g_mu);

    if (pthread_create(&th, NULL, job_thread, j) != 0) {
        struct job **pp;
        pthread_mutex_lock(&g_mu);
        pp = &g_jobs;
        while (*pp && *pp != j) pp = &(*pp)->next;
        if (*pp == j) *pp = j->next;
        pthread_mutex_unlock(&g_mu);
        free(j->url);
        free(j->headers);
        free(j->body);
        free(j);
        return JS_ThrowInternalError(ctx, "httpjson: pthread_create failed");
    }
    pthread_detach(th);
    return JS_NewInt32(ctx, j->id);
}

/* takeResult(id) -> 结果 JSON 字符串；未完成返回 null */
static JSValue js_take_result(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    int id = 0;
    struct job *j = NULL;
    struct job **pp = NULL;
    JSValue res;

    (void)this_val;
    if (argc < 1) return JS_NULL;
    JS_ToInt32(ctx, &id, argv[0]);
    if (id <= 0) return JS_NULL;

    pthread_mutex_lock(&g_mu);
    pp = &g_jobs;
    while (*pp && (*pp)->id != id) pp = &(*pp)->next;
    j = *pp;
    if (!j || j->state != 1) {
        pthread_mutex_unlock(&g_mu);
        return JS_NULL;
    }
    *pp = j->next;
    pthread_mutex_unlock(&g_mu);

    res = JS_NewString(ctx, j->result ? j->result : "{}");
    free(j->url);
    free(j->headers);
    free(j->body);
    free(j->result);
    free(j);
    return res;
}

/* cancelAll()：页面销毁时调用；进行中的任务标记取消，完成后自行回收 */
static void cancel_all_downloads(void);
static JSValue js_cancel_all(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    struct job **pp = NULL;
    (void)this_val;
    (void)argc;
    (void)argv;
    pthread_mutex_lock(&g_mu);
    pp = &g_jobs;
    while (*pp) {
        struct job *j = *pp;
        if (j->state == 1) {
            *pp = j->next;
            free(j->url);
            free(j->headers);
            free(j->body);
            free(j->result);
            free(j);
        } else {
            j->cancelled = 1;
            pp = &j->next;
        }
    }
    pthread_mutex_unlock(&g_mu);
    cancel_all_downloads();
    return JS_UNDEFINED;
}

/* ---------------- 文件工具 + 异步下载（PenBili 离线缓存） ----------------
 *
 * 为什么必须 native 做：JSAPI 的 http.downloadFile 只有 {url,savePath,saveName}，
 * **不能带请求头**；而 B 站 CDN 实测无 UA/Referer 一律 403（真机 curl 对拍：
 * 无头 403 / 只带 UA 403 / UA+Referer 206），所以复用 libcurl 自己下载。
 * 异步：worker 线程写文件，JS 轮询 downloadPoll(id) 拿进度，不阻塞 QuickJS 主线程。
 * 落盘策略：先写 <path>.part，成功后 rename —— 半截文件不会被当成可播缓存。 */

static int mkdir_p(const char *path) {
    char tmp[512];
    size_t len;
    size_t i;
    if (!path || !path[0]) return -1;
    len = strlen(path);
    if (len >= sizeof(tmp)) return -1;
    memcpy(tmp, path, len + 1);
    for (i = 1; i < len; i++) {
        if (tmp[i] == '/') {
            tmp[i] = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
            tmp[i] = '/';
        }
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
    return 0;
}

/* ensureDir(path) -> {ok:1} | {ok:0, error} */
static JSValue js_ensure_dir(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    const char *path = NULL;
    JSValue obj;
    (void)this_val;
    if (argc < 1) return JS_ThrowTypeError(ctx, "httpjson.ensureDir(path)");
    path = JS_ToCString(ctx, argv[0]);
    if (!path) return JS_EXCEPTION;
    obj = JS_NewObject(ctx);
    if (mkdir_p(path) == 0) {
        JS_SetPropertyStr(ctx, obj, "ok", JS_NewInt32(ctx, 1));
    } else {
        JS_SetPropertyStr(ctx, obj, "ok", JS_NewInt32(ctx, 0));
        JS_SetPropertyStr(ctx, obj, "error", JS_NewString(ctx, strerror(errno)));
    }
    JS_FreeCString(ctx, path);
    return obj;
}

/* fileSize(path) -> 字节数；不存在/不可读返回 -1 */
static JSValue js_file_size(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    const char *path = NULL;
    struct stat st;
    double size = -1;
    (void)this_val;
    if (argc < 1) return JS_NewInt32(ctx, -1);
    path = JS_ToCString(ctx, argv[0]);
    if (!path) return JS_NewInt32(ctx, -1);
    if (stat(path, &st) == 0 && S_ISREG(st.st_mode)) size = (double)st.st_size;
    JS_FreeCString(ctx, path);
    return JS_NewFloat64(ctx, size);
}

/* removeFile(path) -> {ok:1|0}；不存在也算成功（幂等） */
static JSValue js_remove_file(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    const char *path = NULL;
    JSValue obj;
    int ok = 0;
    (void)this_val;
    if (argc < 1) return JS_ThrowTypeError(ctx, "httpjson.removeFile(path)");
    path = JS_ToCString(ctx, argv[0]);
    if (!path) return JS_EXCEPTION;
    ok = (unlink(path) == 0 || errno == ENOENT) ? 1 : 0;
    obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "ok", JS_NewInt32(ctx, ok));
    JS_FreeCString(ctx, path);
    return obj;
}

struct dljob {
    int id;
    int state;      /* 0=进行中 1=完成 2=失败 */
    int cancelled;
    char *url;
    char *headers;
    char *part_path;
    char *final_path;
    long timeout_ms;
    long long bytes;
    long status;
    char *error;
    struct dljob *next;
};

static struct dljob *g_dls = NULL;
static pthread_mutex_t g_dl_mu = PTHREAD_MUTEX_INITIALIZER;
static int g_next_dl_id = 1;

/* 页面销毁时把进行中的下载标记取消（写回调会中止 curl，完成后自行回收） */
static void cancel_all_downloads(void) {
    struct dljob *d = NULL;
    pthread_mutex_lock(&g_dl_mu);
    for (d = g_dls; d; d = d->next) {
        if (d->state == 0) d->cancelled = 1;
    }
    pthread_mutex_unlock(&g_dl_mu);
}

struct dl_sink {
    FILE *fp;
    struct dljob *job;
};

static size_t dl_write_cb(char *ptr, size_t size, size_t nmemb, void *userdata) {
    struct dl_sink *s = (struct dl_sink *)userdata;
    size_t n = size * nmemb;
    int cancelled = 0;
    pthread_mutex_lock(&g_dl_mu);
    if (s->job) {
        cancelled = s->job->cancelled;
        if (!cancelled) s->job->bytes += (long long)n;
    }
    pthread_mutex_unlock(&g_dl_mu);
    if (cancelled) return 0; /* 返回 0 → curl 以 CURLE_WRITE_ERROR 中止 */
    if (s->fp && n && fwrite(ptr, 1, n, s->fp) != n) return 0;
    return n;
}

/* 同步 GET 到文件；返回 0=成功（HTTP 2xx） */
static int do_download(const char *url, const char *headers_text, const char *path,
                       long timeout_ms, int insecure, struct dljob *job,
                       long *status, int *curl_code, char **err_msg) {
    CURL *h;
    CURL_slist *list = NULL;
    struct dl_sink sink;
    FILE *fp;
    int code;

    *status = 0;
    *curl_code = 0;
    *err_msg = NULL;

    fp = fopen(path, "wb");
    if (!fp) {
        *err_msg = dup_str("open save file failed");
        return -1;
    }
    memset(&sink, 0, sizeof(sink));
    sink.fp = fp;
    sink.job = job;

    h = curl_easy_init();
    if (!h) {
        fclose(fp);
        *err_msg = dup_str("curl_easy_init failed");
        return -1;
    }
    if (headers_text && headers_text[0]) {
        char *copy = dup_str(headers_text);
        if (copy) {
            char *save = NULL;
            char *line = strtok_r(copy, "\n", &save);
            while (line) {
                size_t l = strlen(line);
                while (l && (line[l - 1] == '\r' || line[l - 1] == ' ')) line[--l] = '\0';
                if (l) list = curl_slist_append(list, line);
                line = strtok_r(NULL, "\n", &save);
            }
            free(copy);
        }
    }

    curl_easy_setopt(h, CURLOPT_URL, url);
    if (list) curl_easy_setopt(h, CURLOPT_HTTPHEADER, list);
    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, dl_write_cb);
    curl_easy_setopt(h, CURLOPT_WRITEDATA, &sink);
    curl_easy_setopt(h, CURLOPT_TIMEOUT_MS, timeout_ms);
    curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT_MS, 15000L);
    curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(h, CURLOPT_NOSIGNAL, 1L);
    if (insecure) {
        curl_easy_setopt(h, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(h, CURLOPT_SSL_VERIFYHOST, 0L);
    } else {
        curl_easy_setopt(h, CURLOPT_SSL_VERIFYPEER, 1L);
        curl_easy_setopt(h, CURLOPT_SSL_VERIFYHOST, 2L);
    }

    code = curl_easy_perform(h);
    *curl_code = code;
    if (code == 0) {
        curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, status);
    } else {
        char tmp[128];
        const char *msg = curl_easy_strerror(code);
        snprintf(tmp, sizeof(tmp), "curl#%d: %s", code, msg ? msg : "unknown");
        *err_msg = dup_str(tmp);
    }
    if (list) curl_slist_free_all(list);
    curl_easy_cleanup(h);
    if (fclose(fp) != 0 && code == 0) {
        *err_msg = dup_str("close save file failed");
        return -1;
    }
    return code == 0 ? 0 : -1;
}

static void *dl_thread(void *arg) {
    struct dljob *j = (struct dljob *)arg;
    long status = 0;
    int code = 0;
    char *err = NULL;
    int rc, insecure = 0, cancelled = 0;

    rc = do_download(j->url, j->headers, j->part_path, j->timeout_ms, 0, j, &status, &code, &err);
    if (rc != 0 && err && is_cert_error(code)) {
        /* 设备无 CA 库：与 do_get 同策略，不校验重试一次 */
        free(err);
        err = NULL;
        rc = do_download(j->url, j->headers, j->part_path, j->timeout_ms, 1, j, &status, &code, &err);
        insecure = 1;
    }
    if (rc == 0 && !(status >= 200 && status < 300)) {
        char tmp[96];
        snprintf(tmp, sizeof(tmp), "HTTP %ld", status);
        if (err) free(err);
        err = dup_str(tmp);
        rc = -1;
    }
    (void)insecure;

    pthread_mutex_lock(&g_dl_mu);
    cancelled = j->cancelled;
    j->status = status;
    if (cancelled) {
        unlink(j->part_path);
        j->state = 2;
        if (err) free(err);
        j->error = dup_str("cancelled");
    } else if (rc == 0) {
        if (rename(j->part_path, j->final_path) == 0) {
            j->state = 1;
        } else {
            j->state = 2;
            if (err) free(err);
            j->error = dup_str("rename failed");
            unlink(j->part_path);
        }
    } else {
        j->state = 2;
        j->error = err ? err : dup_str("download failed");
        err = NULL;
        unlink(j->part_path);
    }
    if (err) free(err);
    pthread_mutex_unlock(&g_dl_mu);
    return NULL;
}

/* startDownload(url, headers, filePath, timeoutMs) -> 任务 id（0=失败） */
static JSValue js_start_download(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    const char *url = NULL;
    const char *headers = NULL;
    const char *path = NULL;
    int timeout_ms = 600000;
    struct dljob *j = NULL;
    pthread_t th;
    size_t plen;

    (void)this_val;
    if (argc < 3) return JS_ThrowTypeError(ctx, "startDownload(url, headers, filePath, timeoutMs)");
    url = JS_ToCString(ctx, argv[0]);
    if (!url) return JS_EXCEPTION;
    if (!JS_IsUndefined(argv[1]) && !JS_IsNull(argv[1])) headers = JS_ToCString(ctx, argv[1]);
    path = JS_ToCString(ctx, argv[2]);
    if (!path) {
        JS_FreeCString(ctx, url);
        if (headers) JS_FreeCString(ctx, headers);
        return JS_EXCEPTION;
    }
    if (argc > 3) JS_ToInt32(ctx, &timeout_ms, argv[3]);
    if (timeout_ms <= 0) timeout_ms = 600000;
    plen = strlen(path);
    if (!(strncmp(url, "http://", 7) == 0 || strncmp(url, "https://", 8) == 0) || path[0] != '/' || plen < 2) {
        JS_FreeCString(ctx, url);
        if (headers) JS_FreeCString(ctx, headers);
        JS_FreeCString(ctx, path);
        return JS_ThrowTypeError(ctx, "startDownload: bad url/path");
    }

    j = (struct dljob *)calloc(1, sizeof(*j));
    if (!j) {
        JS_FreeCString(ctx, url);
        if (headers) JS_FreeCString(ctx, headers);
        JS_FreeCString(ctx, path);
        return JS_ThrowInternalError(ctx, "httpjson: out of memory");
    }
    j->url = dup_str(url);
    j->headers = dup_str(headers ? headers : "");
    j->final_path = dup_str(path);
    {
        char *p = (char *)malloc(plen + 6);
        if (p) {
            memcpy(p, path, plen);
            memcpy(p + plen, ".part", 6);
        }
        j->part_path = p;
    }
    j->timeout_ms = timeout_ms;
    JS_FreeCString(ctx, url);
    if (headers) JS_FreeCString(ctx, headers);
    JS_FreeCString(ctx, path);
    if (!j->url || !j->headers || !j->final_path || !j->part_path) {
        free(j->url);
        free(j->headers);
        free(j->final_path);
        free(j->part_path);
        free(j);
        return JS_ThrowInternalError(ctx, "httpjson: out of memory");
    }

    pthread_mutex_lock(&g_dl_mu);
    j->id = g_next_dl_id++;
    j->next = g_dls;
    g_dls = j;
    pthread_mutex_unlock(&g_dl_mu);

    if (pthread_create(&th, NULL, dl_thread, j) != 0) {
        struct dljob **pp;
        pthread_mutex_lock(&g_dl_mu);
        pp = &g_dls;
        while (*pp && *pp != j) pp = &(*pp)->next;
        if (*pp == j) *pp = j->next;
        pthread_mutex_unlock(&g_dl_mu);
        free(j->url);
        free(j->headers);
        free(j->final_path);
        free(j->part_path);
        free(j);
        return JS_ThrowInternalError(ctx, "httpjson: pthread_create failed");
    }
    pthread_detach(th);
    return JS_NewInt32(ctx, j->id);
}

/* downloadPoll(id) -> null=未知 id；{state:0,bytes} 进行中；
 * {state:1,bytes,status} 完成；{state:2,bytes,error} 失败。
 * 终态返回后条目即回收（与 takeResult 同语义，只取一次）。 */
static JSValue js_download_poll(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    int id = 0;
    struct dljob *j = NULL;
    struct dljob **pp = NULL;
    int state = 0;
    long long bytes = 0;
    long status = 0;
    char *error = NULL;
    JSValue obj;
    (void)this_val;
    if (argc < 1) return JS_NULL;
    JS_ToInt32(ctx, &id, argv[0]);
    if (id <= 0) return JS_NULL;

    pthread_mutex_lock(&g_dl_mu);
    pp = &g_dls;
    while (*pp && (*pp)->id != id) pp = &(*pp)->next;
    j = *pp;
    if (!j) {
        pthread_mutex_unlock(&g_dl_mu);
        return JS_NULL;
    }
    state = j->state;
    bytes = j->bytes;
    status = j->status;
    if (state != 0) {
        *pp = j->next;
        error = j->error;
        j->error = NULL;
    }
    pthread_mutex_unlock(&g_dl_mu);

    obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "state", JS_NewInt32(ctx, state));
    JS_SetPropertyStr(ctx, obj, "bytes", JS_NewFloat64(ctx, (double)bytes));
    JS_SetPropertyStr(ctx, obj, "status", JS_NewInt32(ctx, (int)status));
    if (state == 2) JS_SetPropertyStr(ctx, obj, "error", JS_NewString(ctx, error ? error : "download failed"));
    if (state != 0) {
        free(j->url);
        free(j->headers);
        free(j->final_path);
        free(j->part_path);
        if (error) free(error);
        free(j);
    }
    return obj;
}

/* cancelDownload(id) -> 标记取消；进行中的 curl 由写回调中止 */
static JSValue js_cancel_download(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    int id = 0;
    struct dljob *j = NULL;
    (void)this_val;
    if (argc < 1) return JS_UNDEFINED;
    JS_ToInt32(ctx, &id, argv[0]);
    pthread_mutex_lock(&g_dl_mu);
    for (j = g_dls; j; j = j->next) {
        if (j->id == id) {
            j->cancelled = 1;
            break;
        }
    }
    pthread_mutex_unlock(&g_dl_mu);
    return JS_UNDEFINED;
}

static int httpjson_module_init(JSContext *ctx, JSModuleDef *m) {
    JSValue def = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, def, "postSync", JS_NewCFunction(ctx, js_post_sync, "postSync", 5));
    JS_SetPropertyStr(ctx, def, "startPost", JS_NewCFunction(ctx, js_start_post, "startPost", 4));
    JS_SetPropertyStr(ctx, def, "takeResult", JS_NewCFunction(ctx, js_take_result, "takeResult", 1));
    JS_SetPropertyStr(ctx, def, "cancelAll", JS_NewCFunction(ctx, js_cancel_all, "cancelAll", 0));
    JS_SetPropertyStr(ctx, def, "getBinary", JS_NewCFunction(ctx, js_get_binary, "getBinary", 3));
    JS_SetPropertyStr(ctx, def, "ensureDir", JS_NewCFunction(ctx, js_ensure_dir, "ensureDir", 1));
    JS_SetPropertyStr(ctx, def, "fileSize", JS_NewCFunction(ctx, js_file_size, "fileSize", 1));
    JS_SetPropertyStr(ctx, def, "removeFile", JS_NewCFunction(ctx, js_remove_file, "removeFile", 1));
    JS_SetPropertyStr(ctx, def, "startDownload", JS_NewCFunction(ctx, js_start_download, "startDownload", 4));
    JS_SetPropertyStr(ctx, def, "downloadPoll", JS_NewCFunction(ctx, js_download_poll, "downloadPoll", 1));
    JS_SetPropertyStr(ctx, def, "cancelDownload", JS_NewCFunction(ctx, js_cancel_download, "cancelDownload", 1));
    JS_SetModuleExport(ctx, m, "default", def);
    JS_SetModuleExport(ctx, m, "postSync", JS_NewCFunction(ctx, js_post_sync, "postSync", 5));
    JS_SetModuleExport(ctx, m, "startPost", JS_NewCFunction(ctx, js_start_post, "startPost", 4));
    JS_SetModuleExport(ctx, m, "takeResult", JS_NewCFunction(ctx, js_take_result, "takeResult", 1));
    JS_SetModuleExport(ctx, m, "cancelAll", JS_NewCFunction(ctx, js_cancel_all, "cancelAll", 0));
    JS_SetModuleExport(ctx, m, "getBinary", JS_NewCFunction(ctx, js_get_binary, "getBinary", 3));
    JS_SetModuleExport(ctx, m, "ensureDir", JS_NewCFunction(ctx, js_ensure_dir, "ensureDir", 1));
    JS_SetModuleExport(ctx, m, "fileSize", JS_NewCFunction(ctx, js_file_size, "fileSize", 1));
    JS_SetModuleExport(ctx, m, "removeFile", JS_NewCFunction(ctx, js_remove_file, "removeFile", 1));
    JS_SetModuleExport(ctx, m, "startDownload", JS_NewCFunction(ctx, js_start_download, "startDownload", 4));
    JS_SetModuleExport(ctx, m, "downloadPoll", JS_NewCFunction(ctx, js_download_poll, "downloadPoll", 1));
    JS_SetModuleExport(ctx, m, "cancelDownload", JS_NewCFunction(ctx, js_cancel_download, "cancelDownload", 1));
    return 0;
}

static JSModuleDef *httpjson_module_load(JSContext *ctx, const char *moduleName) {
    if (strcmp(moduleName, "httpjson") == 0) {
        JSModuleDef *m = JS_NewCModule(ctx, moduleName, httpjson_module_init);
        if (!m) return NULL;
        JS_AddModuleExport(ctx, m, "default");
        JS_AddModuleExport(ctx, m, "postSync");
        JS_AddModuleExport(ctx, m, "startPost");
        JS_AddModuleExport(ctx, m, "takeResult");
        JS_AddModuleExport(ctx, m, "cancelAll");
        JS_AddModuleExport(ctx, m, "getBinary");
        JS_AddModuleExport(ctx, m, "ensureDir");
        JS_AddModuleExport(ctx, m, "fileSize");
        JS_AddModuleExport(ctx, m, "removeFile");
        JS_AddModuleExport(ctx, m, "startDownload");
        JS_AddModuleExport(ctx, m, "downloadPoll");
        JS_AddModuleExport(ctx, m, "cancelDownload");
        return m;
    }
    return NULL;
}


extern void registerCModuleLoader(const char *moduleName, LoadCModuleFunction loader);

/* 真机 DNS 自愈（2026-11-10 卡顿根因）：
 *
 * 现象：360p 播放每 2~8s 停一次、native 反复 RESYNC，网络吞吐只有 30~40KB/s。
 * 根因：**每次域名解析固定耗 5.0s**（curl 的 time_namelookup=5.006；同一时刻
 * nslookup 打同一个 DNS 只要 0.13s）。/tmp/resolv.conf 由 dhcpcd 生成，第一条是
 * 路由器 192.168.0.102 —— glibc 的查询（带 EDNS）被它静默丢弃，走满 5s 超时才
 * 落到后面的服务器。视频每次重连、每张封面、每个接口都要重付这 5s。
 *
 * 修法：把公共 DNS 放到最前面（保留原第一条做兜底），并压短超时。
 * 只在「还没修过」时写一次（文件里已有 223.5.5.5 就跳过），不做后台轮询。
 * 位置选在 httpjson 模块加载时 —— 它在任何网络请求之前完成，native ffmpeg
 * 起播时 /tmp/resolv.conf 已经是新的（getaddrinfo 每次查文件）。
 * 副作用可逆：重启或重新联网后 dhcpcd 会重新生成该文件。 */
static void ensure_dns_config(void) {
    char buf[4096];
    char original[2][64];
    int kept = 0;
    FILE *f;
    size_t n;
    memset(original, 0, sizeof(original));
    f = fopen("/tmp/resolv.conf", "r");
    if (!f) return;
    n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';
    /* 不能用 strstr 判断「已修过」—— dhcpcd 原文里本来就含 223.5.5.5（第 4 条），
     * 子串匹配会把未修复误判成已修复。必须看第一条 nameserver。 */
    {
        const char *p = strstr(buf, "nameserver");
        if (p) {
            p += 10;
            while (*p == ' ' || *p == '\t') p++;
            if (strncmp(p, "223.5.5.5", 9) == 0) return; /* 已经修过 */
        }
    }
    {
        char *line = buf;
        while (line && *line && kept < 2) {
            char *nl = strchr(line, '\n');
            if (nl) *nl = '\0';
            if (strncmp(line, "nameserver ", 11) == 0) {
                const char *ip = line + 11;
                if (strncmp(ip, "223.5.5.5", 9) != 0 && strncmp(ip, "223.6.6.6", 9) != 0) {
                    snprintf(original[kept], sizeof(original[kept]), "%s", ip);
                    kept++;
                }
            }
            line = nl ? nl + 1 : NULL;
        }
    }
    f = fopen("/tmp/resolv.conf", "w");
    if (!f) return;
    fprintf(f, "nameserver 223.5.5.5\nnameserver 223.6.6.6\n");
    for (n = 0; n < (size_t)kept; n++) fprintf(f, "nameserver %s\n", original[n]);
    fprintf(f, "options timeout:1 attempts:2\n");
    fclose(f);
    {
        FILE *log = fopen("/tmp/vp_fb.log", "a");
        if (log) {
            fprintf(log, "dns fix -> 223.5.5.5 first (kept=%d)\n", kept);
            fclose(log);
        }
    }
}

/* DNS 只在模块加载时修一次即可：
 * 3.9.7 起设备的 /etc/resolv.conf 已是静态文件，且 dhcpcd 加了
 * `nohook resolv.conf`，不会再被网络切换覆盖（见 HANDOFF 3.9.7）。
 * 这里不再起后台轮询线程；这一行只是兜底（文件首条已是 223.5.5.5 时直接返回）。 */
JQUICK_EXPORT void custom_init_jsapis(void) {
    ensure_dns_config();
    registerCModuleLoader("httpjson", &httpjson_module_load);
}
