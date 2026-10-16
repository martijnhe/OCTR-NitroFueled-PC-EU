/* SacredFire: local online mode (BB_ONLINE=local). The game's online services answered inside
 * this process: the network is up, the user is signed in to PSN with a local account, and the
 * game's HTTPS requests (sceHttp: Demonware UNO, umbrella auth, objectstore) reach
 * online_handle() instead of a server. Nothing leaves the machine. Without BB_ONLINE=local this
 * file resolves nothing and runtime_services.c keeps the offline answers (signed out, no network).
 * Every request is logged to stdout and to logs/online.log (BB_ONLINE_LOG overrides the path). */
#include "runtime.h"
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HTTP_INVALID_ID ((int32_t)0x80431100)
#define HTTP_INVALID_VALUE ((int32_t)0x804311fe)
#define HTTP_NOT_FOUND ((int32_t)0x80432025) /* ParseResponseHeader: no such field */
#define NP_INVALID_ARGUMENT ((int32_t)0x80550003)
#define MAX_OBJECTS 256

/* Portable (C11, MinGW) string helpers. */
static char *copy_string(const char *text) {
    size_t n = strlen(text) + 1;
    char *copy = malloc(n);
    if (copy) memcpy(copy, text, n);
    return copy;
}
static int prefix_equal_nocase(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        int x = (unsigned char)a[i], y = (unsigned char)b[i];
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y) return 0;
    }
    return 1;
}

static int enabled(void) {
    static int value = -1;
    if (value < 0) {
        const char *mode = getenv("BB_ONLINE");
        value = mode && !strcmp(mode, "local");
        if (value) printf("Online: local mode (BB_ONLINE=local): signed in locally, requests answered in-process\n");
    }
    return value;
}

/* ---- log ---- */
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static FILE *log_file;
static void online_log(const char *format, ...) __attribute__((format(printf, 1, 2)));
#include <stdarg.h>
static void online_log(const char *format, ...) {
    if (!log_file) {
        const char *path = getenv("BB_ONLINE_LOG");
        log_file = fopen(path && *path ? path : "logs/online.log", "a");
    }
    va_list args;
    va_start(args, format);
    vprintf(format, args);
    va_end(args);
    if (log_file) {
        va_start(args, format);
        vfprintf(log_file, format, args);
        va_end(args);
        fflush(log_file);
    }
}

/* ---- the local account ---- */
static const char *online_id(void) {
    const char *id = getenv("BB_ONLINE_ID");
    return id && *id ? id : "CTRPlayer";
}
static uint64_t account_id(void) {
    uint64_t h = 1469598103934665603ull; /* FNV-1a of the online id: stable per name */
    for (const char *p = online_id(); *p; ++p) h = (h ^ (unsigned char)*p) * 1099511628211ull;
    return h | 1;
}

/* ---- NetCtl: connected, wired, 192.168.1.2 ---- */
static ABI int32_t netctl_state(int32_t *state) { if (!state) return (int32_t)0x80412107; *state = 3; return 0; } /* IPOBTAINED */
static ABI int32_t netctl_info(int code, void *info) {
    if (!info) return (int32_t)0x80412107;
    switch (code) {
    case 1: case 11: case 19: *(uint32_t *)info = 0; break;        /* device wired, DHCP, no proxy */
    case 2: memcpy(info, "\x02\x00\x00\x00\x00\x01", 6); break;    /* MAC */
    case 3: *(uint32_t *)info = 1500; break;                        /* MTU */
    case 4: *(uint32_t *)info = 1; break;                           /* link up */
    case 14: strcpy(info, "192.168.1.2"); break;
    case 15: strcpy(info, "255.255.255.0"); break;
    case 16: case 17: strcpy(info, "192.168.1.1"); break;
    case 18: strcpy(info, "0.0.0.0"); break;
    case 12: case 20: *(char *)info = 0; break;
    case 21: *(uint16_t *)info = 0; break;
    default: memset(info, 0, 4); break;
    }
    return 0;
}

/* ---- NP: signed in ---- */
static ABI int32_t np_state(int32_t user, int32_t *state) { (void)user; if (!state) return NP_INVALID_ARGUMENT; *state = 2; return 0; }
static ABI int32_t np_online_id(int32_t user, char *id) {
    (void)user;
    if (!id) return NP_INVALID_ARGUMENT;
    memset(id, 0, 20); /* SceNpOnlineId: data[16], term, dummy[3] */
    strncpy(id, online_id(), 16);
    return 0;
}
static ABI int32_t np_np_id(int32_t user, char *id) {
    if (!id) return NP_INVALID_ARGUMENT;
    memset(id, 0, 36); /* SceNpId: handle (SceNpOnlineId), opt[8], reserved[8] */
    return np_online_id(user, id);
}
static ABI int32_t np_account(int32_t user, uint64_t *id) { (void)user; if (!id) return NP_INVALID_ARGUMENT; *id = account_id(); return 0; }
static ABI int32_t np_user_by_account(uint64_t account, int32_t *user) { (void)account; if (!user) return NP_INVALID_ARGUMENT; *user = 1; return 0; }
static int next_id = 0x4000;
static int new_id(void) { pthread_mutex_lock(&lock); int id = next_id++; pthread_mutex_unlock(&lock); return id; }
static ABI int32_t np_request(void) { return new_id(); }
static ABI int32_t np_ok(void) { return 0; }
static ABI int32_t np_poll(int32_t request, int32_t *result) { (void)request; if (result) *result = 0; return 0; } /* finished, OK */
static ABI int32_t np_parental(int32_t request, int32_t user, int8_t *age, void *info) {
    (void)request; (void)user;
    if (age) *age = 30;
    if (info) memset(info, 0, 4); /* no restrictions */
    return 0;
}
static ABI int32_t np_check_plus(int32_t request, const void *param, uint8_t *result) {
    (void)request; (void)param;
    if (result) memset(result, 0, 1); /* not authorized: no PlayStation Plus (online racing checks this) */
    return 0;
}
static ABI int32_t np_auth_code(int32_t request, const void *param, char *code, int32_t *issuer) {
    (void)request; (void)param;
    if (code) { memset(code, 0, 136); strcpy(code, "LOCALCODE"); } /* SceNpAuthorizationCode */
    if (issuer) *issuer = 256; /* production */
    online_log("Online: PSN authorization code requested (local)\n");
    return 0;
}
static ABI int32_t np_id_token(int32_t request, const void *param, char *token) {
    (void)request; (void)param;
    if (token) { memset(token, 0, 4104); strcpy(token, "LOCALTOKEN"); } /* SceNpAuthIdToken */
    online_log("Online: PSN ID token requested (local)\n");
    return 0;
}

/* ---- sceHttp: requests answered by online_handle ---- */
typedef struct {
    int used, id, kind; /* 1 template, 2 connection, 3 request */
    char method[16];
    char *url;
    char *headers; size_t headers_len;           /* request headers "Name: value\r\n" */
    char *body; size_t body_len;
    int status;
    char *response_headers; size_t response_headers_len;
    char *response; size_t response_len, read_pos;
} HttpObject;
static HttpObject objects[MAX_OBJECTS];

static HttpObject *object_new(int kind) {
    pthread_mutex_lock(&lock);
    for (int i = 0; i < MAX_OBJECTS; ++i) {
        if (!objects[i].used) {
            memset(&objects[i], 0, sizeof objects[i]);
            objects[i].used = 1; objects[i].kind = kind; objects[i].id = next_id++;
            pthread_mutex_unlock(&lock);
            return &objects[i];
        }
    }
    pthread_mutex_unlock(&lock);
    return NULL;
}
static HttpObject *object_get(int id) {
    for (int i = 0; i < MAX_OBJECTS; ++i) if (objects[i].used && objects[i].id == id) return &objects[i];
    return NULL;
}
static void object_free(HttpObject *o) {
    free(o->url); free(o->headers); free(o->body); free(o->response_headers); free(o->response);
    memset(o, 0, sizeof *o);
}
static void append(char **buffer, size_t *length, const void *data, size_t size) {
    char *grown = realloc(*buffer, *length + size + 1);
    if (!grown) return;
    memcpy(grown + *length, data, size);
    *length += size;
    grown[*length] = 0;
    *buffer = grown;
}

/* The answer to one request: status, extra response headers, body. Stage 1: everything is logged
 * and answered 503, so the game's sequence of requests can be seen. */
static void online_handle(HttpObject *r) {
    static const char body[] = "{\"error\":{\"code\":503,\"msg\":\"SacredFire local online: not implemented yet\"}}";
    r->status = 503;
    free(r->response);
    r->response = copy_string(body);
    r->response_len = strlen(body);
    r->read_pos = 0;
    char headers[256];
    int n = snprintf(headers, sizeof headers, "HTTP/1.1 %d\r\nContent-Type: application/json\r\nContent-Length: %zu\r\n\r\n",
                     r->status, r->response_len);
    free(r->response_headers);
    r->response_headers = copy_string(headers);
    r->response_headers_len = (size_t)n;
}

static const char *const methods[] = {"GET", "POST", "HEAD", "OPTIONS", "PUT", "DELETE", "TRACE", "CONNECT"};

static ABI int32_t http_template(int32_t ctx, const char *agent, int32_t version, int32_t proxy) {
    (void)ctx; (void)version; (void)proxy;
    HttpObject *o = object_new(1);
    if (!o) return HTTP_INVALID_VALUE;
    online_log("Online: HTTP template %d (agent %s)\n", o->id, agent ? agent : "-");
    return o->id;
}
static ABI int32_t http_connection(int32_t tmpl, const char *url, int keepalive) {
    (void)tmpl; (void)keepalive;
    HttpObject *o = object_new(2);
    if (!o) return HTTP_INVALID_VALUE;
    o->url = copy_string(url ? url : "");
    return o->id;
}
static int32_t request_new(int32_t conn, const char *method, const char *url, uint64_t length) {
    (void)conn; (void)length;
    HttpObject *o = object_new(3);
    if (!o) return HTTP_INVALID_VALUE;
    snprintf(o->method, sizeof o->method, "%s", method);
    o->url = copy_string(url ? url : "");
    return o->id;
}
static ABI int32_t http_request(int32_t conn, int32_t method, const char *url, uint64_t length) {
    return request_new(conn, method >= 0 && method < 8 ? methods[method] : "GET", url, length);
}
static ABI int32_t http_request2(int32_t conn, const char *method, const char *url, uint64_t length) {
    return request_new(conn, method ? method : "GET", url, length);
}
static ABI int32_t http_header(int32_t id, const char *name, const char *value, uint32_t mode) {
    (void)mode;
    HttpObject *o = object_get(id);
    if (!o) return HTTP_INVALID_ID;
    if (o->kind == 3 && name && value) {
        append(&o->headers, &o->headers_len, name, strlen(name));
        append(&o->headers, &o->headers_len, ": ", 2);
        append(&o->headers, &o->headers_len, value, strlen(value));
        append(&o->headers, &o->headers_len, "\r\n", 2);
    }
    return 0;
}
static ABI int32_t http_send(int32_t id, const void *data, size_t size) {
    HttpObject *o = object_get(id);
    if (!o || o->kind != 3) return HTTP_INVALID_ID;
    if (data && size) append(&o->body, &o->body_len, data, size);
    online_handle(o);
    online_log("Online: %s %s -> %d\n%s%s%.*s\n", o->method, o->url, o->status, o->headers ? o->headers : "",
               o->body_len ? "  body: " : "", (int)(o->body_len > 4000 ? 4000 : o->body_len), o->body ? o->body : "");
    return 0;
}
static ABI int32_t http_status(int32_t id, int32_t *status) {
    HttpObject *o = object_get(id);
    if (!o) return HTTP_INVALID_ID;
    if (status) *status = o->status;
    return 0;
}
static ABI int32_t http_length(int32_t id, int32_t *result, uint64_t *length) {
    HttpObject *o = object_get(id);
    if (!o) return HTTP_INVALID_ID;
    if (result) *result = 0; /* the length is known */
    if (length) *length = o->response_len;
    return 0;
}
static ABI int32_t http_read(int32_t id, void *data, size_t size) {
    HttpObject *o = object_get(id);
    if (!o) return HTTP_INVALID_ID;
    size_t left = o->response_len - o->read_pos;
    size_t n = left < size ? left : size;
    if (n && data) memcpy(data, o->response + o->read_pos, n);
    o->read_pos += n;
    return (int32_t)n;
}
static ABI int32_t http_all_headers(int32_t id, char **header, size_t *size) {
    HttpObject *o = object_get(id);
    if (!o) return HTTP_INVALID_ID;
    if (header) *header = o->response_headers;
    if (size) *size = o->response_headers_len;
    return 0;
}
static ABI int32_t http_parse_header(const char *header, size_t length, const char *field, const char **value, size_t *value_length) {
    if (!header || !field) return HTTP_INVALID_VALUE;
    size_t flen = strlen(field);
    for (const char *line = header; line < header + length;) {
        const char *end = memchr(line, '\n', (size_t)(header + length - line));
        if (!end) end = header + length;
        if ((size_t)(end - line) > flen && prefix_equal_nocase(line, field, flen) && line[flen] == ':') {
            const char *v = line + flen + 1;
            while (v < end && *v == ' ') ++v;
            const char *e = end;
            while (e > v && (e[-1] == '\r' || e[-1] == ' ')) --e;
            if (value) *value = v;
            if (value_length) *value_length = (size_t)(e - v);
            return (int32_t)(line - header);
        }
        line = end + 1;
    }
    return HTTP_NOT_FOUND;
}
static ABI int32_t http_delete(int32_t id) {
    HttpObject *o = object_get(id);
    if (!o) return HTTP_INVALID_ID;
    pthread_mutex_lock(&lock);
    object_free(o);
    pthread_mutex_unlock(&lock);
    return 0;
}
static ABI int32_t http_ok(void) { return 0; }

static const RuntimeExport exports[] = {
    {"sceNetCtlGetState", netctl_state}, {"sceNetCtlGetInfo", netctl_info},
    {"sceNpGetState", np_state}, {"sceNpGetOnlineId", np_online_id}, {"sceNpGetNpId", np_np_id},
    {"sceNpGetAccountIdA", np_account}, {"sceNpGetUserIdByAccountId", np_user_by_account},
    {"sceNpCheckNpAvailabilityA", np_ok}, {"sceNpCheckNpAvailability", np_ok},
    {"sceNpGetParentalControlInfoA", np_parental}, {"sceNpCheckPlus", np_check_plus},
    {"sceNpPollAsync", np_poll}, {"sceNpCreateAsyncRequest", np_request}, {"sceNpCreateRequest", np_request},
    {"sceNpAuthCreateRequest", np_request}, {"sceNpAuthCreateAsyncRequest", np_request},
    {"sceNpAuthPollAsync", np_poll}, {"sceNpAuthGetAuthorizationCodeA", np_auth_code},
    {"sceNpAuthGetAuthorizationCode", np_auth_code}, {"sceNpAuthGetIdTokenA", np_id_token},
    {"sceHttpCreateTemplate", http_template}, {"sceHttpCreateConnectionWithURL", http_connection},
    {"sceHttpCreateRequestWithURL", http_request}, {"sceHttpCreateRequestWithURL2", http_request2},
    {"sceHttpAddRequestHeader", http_header}, {"sceHttpSendRequest", http_send},
    {"sceHttpGetStatusCode", http_status}, {"sceHttpGetResponseContentLength", http_length},
    {"sceHttpReadData", http_read}, {"sceHttpGetAllResponseHeaders", http_all_headers},
    {"sceHttpParseResponseHeader", http_parse_header},
    {"sceHttpDeleteRequest", http_delete}, {"sceHttpDeleteConnection", http_delete},
    {"sceHttpDeleteTemplate", http_delete}, {"sceHttpSetRequestContentLength", http_ok},
    {"sceHttpSetChunkedTransferEnabled", http_ok}, {"sceHttpsSetSslCallback", http_ok},
    {"sceHttpSetConnectTimeOut", http_ok}, {"sceHttpAbortRequest", http_ok},
};

uintptr_t runtime_online_resolve(const char *name) {
    if (!enabled()) return 0;
    return RUNTIME_LOOKUP(exports, name);
}
