#include "runtime.h"
#include <stdarg.h>
#include <stdio.h>
#include <time.h>

#ifdef __ANDROID__
#include <android/log.h>
#endif

#define DS_ERROR_MESSAGE_SIZE 1024

Joy joy = {0};
int screen_w = 0;
int screen_h = 0;
double dt = 0.0;

static jmp_buf ds_error_jump;
static int ds_error_handler_active = 0;
static int ds_has_error = 0;
static int ds_restart_requested = 0;
static char ds_last_error[DS_ERROR_MESSAGE_SIZE] = {0};

typedef struct DSStringNode DSStringNode;
struct DSStringNode { DSStringNode *next; char *string; };
static DSStringNode *ds_strings = NULL;

void ds_log(const char *format, ...) {
    va_list args;
    va_start(args, format);
    __android_log_vprint(ANDROID_LOG_INFO, "DimScript", format, args);
    va_end(args);
}

void ds_runtime_error(const char *format, ...) {
    va_list args, copy;
    va_start(args, format);
    va_copy(copy, args);
    vsnprintf(ds_last_error, sizeof(ds_last_error), format, copy);
    va_end(copy);
    __android_log_vprint(ANDROID_LOG_ERROR, "DimScript", format, args);
    va_end(args);
    ds_has_error = 1;
    if (ds_error_handler_active) longjmp(ds_error_jump, 1);
}

int ds_call_protected(DSProtectedFunction function, void *userdata, const char *label) {
    int jumped;
    if (!function) {
        if (label && *label) ds_runtime_error("cannot call an empty script hook '%s'", label);
        else ds_runtime_error("cannot call an empty script hook");
        return 0;
    }
    if (ds_error_handler_active) {
        function(userdata);
        return !ds_has_error;
    }
    ds_error_handler_active = 1;
    jumped = setjmp(ds_error_jump);
    if (jumped == 0) {
        function(userdata);
        ds_error_handler_active = 0;
        return !ds_has_error;
    }
    ds_error_handler_active = 0;
    if (label && *label && ds_last_error[0] == '\0') {
        snprintf(ds_last_error, sizeof(ds_last_error), "script hook '%s' failed", label);
    }
    return 0;
}

const char *ds_runtime_error_message(void) { return ds_last_error[0] ? ds_last_error : "unknown DimScript runtime error"; }
int ds_script_has_error(void) { return ds_has_error; }
void ds_clear_runtime_error(void) { ds_has_error = 0; ds_last_error[0] = '\0'; }
void ds_request_script_restart(void) { ds_restart_requested = 1; }
int ds_script_restart_requested(void) { return ds_restart_requested; }
void ds_clear_script_restart(void) { ds_restart_requested = 0; }

static char *ds_strdup(const char *s) {
    if (!s) s = "";
    size_t n = strlen(s)+1;
    char *c = (char*)malloc(n);
    if (c) memcpy(c, s, n);
    return c;
}
static char *ds_track_string(char *s) {
    if (!s) { ds_runtime_error("out of memory string"); return NULL; }
    DSStringNode *node = (DSStringNode*)malloc(sizeof(*node));
    if (!node) { free(s); ds_runtime_error("out of memory tracking"); return NULL; }
    node->string = s; node->next = ds_strings; ds_strings = node;
    return s;
}
char *ds_num_to_string(double number) {
    char buf[96];
    if (snprintf(buf, sizeof(buf), "%g", number) < 0) return NULL;
    return ds_track_string(ds_strdup(buf));
}
void ds_string_pool_reset(void) {
    DSStringNode *node = ds_strings;
    while (node) { DSStringNode *next = node->next; free(node->string); free(node); node = next; }
    ds_strings = NULL;
}
char *ds_concat(const char *left, const char *right) {
    size_t la = left ? strlen(left) : 0, lb = right ? strlen(right) : 0;
    char *out = (char*)malloc(la+lb+1);
    if (!out) return ds_track_string(ds_strdup(""));
    if (la) memcpy(out, left, la);
    if (lb) memcpy(out+la, right, lb);
    out[la+lb] = '\0';
    return ds_track_string(out);
}

/* ---------- новые типы: массивы ---------- */
struct DSArray { double *data; size_t len, cap; };
DSArray* arr_new(void) {
    DSArray *a = (DSArray*)calloc(1, sizeof(*a));
    if (!a) { ds_runtime_error("arr_new OOM"); return NULL; }
    a->cap = 8; a->data = (double*)malloc(a->cap*sizeof(double));
    if (!a->data) { free(a); ds_runtime_error("arr_new OOM"); return NULL; }
    return a;
}
void arr_push(DSArray* a, double v) {
    if (!a) return;
    if (a->len >= a->cap) {
        size_t nc = a->cap*2; if (nc<8) nc=8;
        double *nd = (double*)realloc(a->data, nc*sizeof(double));
        if (!nd) { ds_runtime_error("arr_push OOM"); return; }
        a->data = nd; a->cap = nc;
    }
    a->data[a->len++] = v;
}
double arr_pop(DSArray* a) {
    if (!a || a->len==0) return 0;
    return a->data[--a->len];
}
double arr_get(DSArray* a, double idx) {
    if (!a) return 0;
    long i = (long)idx;
    if (i<0 || (size_t)i>=a->len) return 0;
    return a->data[i];
}
void arr_set(DSArray* a, double idx, double v) {
    if (!a) return;
    long i = (long)idx;
    if (i<0) return;
    if ((size_t)i>=a->len) {
        // расширяем нулями
        while (a->len <= (size_t)i) arr_push(a, 0);
    }
    a->data[i] = v;
}
double arr_len(DSArray* a) { return a ? (double)a->len : 0; }
void arr_clear(DSArray* a) { if (a) a->len=0; }
void arr_free(DSArray* a) { if (!a) return; free(a->data); free(a); }

/* ---------- словари ---------- */
typedef struct DSDictEntry { char *key; double val; struct DSDictEntry *next; } DSDictEntry;
struct DSDict { DSDictEntry *head; };
DSDict* dict_new(void) { DSDict *d = (DSDict*)calloc(1,sizeof(*d)); if(!d) ds_runtime_error("dict_new OOM"); return d; }
void dict_set(DSDict* d, const char* key, double val) {
    if (!d||!key) return;
    for (DSDictEntry *e=d->head; e; e=e->next) if (strcmp(e->key,key)==0) { e->val=val; return; }
    DSDictEntry *e = (DSDictEntry*)malloc(sizeof(*e)); if(!e){ ds_runtime_error("dict_set OOM"); return; }
    e->key=ds_strdup(key); e->val=val; e->next=d->head; d->head=e;
}
double dict_get(DSDict* d, const char* key) {
    if (!d||!key) return 0;
    for (DSDictEntry *e=d->head; e; e=e->next) if (strcmp(e->key,key)==0) return e->val;
    return 0;
}
int dict_has(DSDict* d, const char* key) {
    if (!d||!key) return 0;
    for (DSDictEntry *e=d->head; e; e=e->next) if (strcmp(e->key,key)==0) return 1;
    return 0;
}
void dict_del(DSDict* d, const char* key) {
    if (!d||!key) return;
    DSDictEntry **pp=&d->head;
    while(*pp){ if(strcmp((*pp)->key,key)==0){ DSDictEntry *t=*pp; *pp=t->next; free(t->key); free(t); return; } pp=&(*pp)->next; }
}
void dict_free(DSDict* d) {
    if (!d) return;
    DSDictEntry *e=d->head;
    while(e){ DSDictEntry *n=e->next; free(e->key); free(e); e=n; }
    free(d);
}

/* ---------- таймеры ---------- */
struct DSTimer { long long start_ms; };
static long long now_ms(void){
    struct timespec ts;
#ifdef CLOCK_MONOTONIC
    if (clock_gettime(CLOCK_MONOTONIC,&ts)==0) return (long long)ts.tv_sec*1000 + ts.tv_nsec/1000000;
#endif
    return (long long)time(NULL)*1000;
}
DSTimer* timer_new(void){ DSTimer *t=(DSTimer*)malloc(sizeof(*t)); if(!t){ ds_runtime_error("timer_new OOM"); return NULL; } t->start_ms=now_ms(); return t; }
void timer_start(DSTimer* t){ if(t) t->start_ms=now_ms(); }
double timer_elapsed(DSTimer* t){ if(!t) return 0; return (now_ms()-t->start_ms)/1000.0; }
void timer_reset(DSTimer* t){ if(t) t->start_ms=now_ms(); }
void timer_free(DSTimer* t){ free(t); }

/* ---------- файлы ---------- */
const char* file_read(const char* path){
    if(!path) return ds_track_string(ds_strdup(""));
    FILE *f=fopen(path,"rb");
    if(!f){
        // пробуем с префиксами
        char buf[512];
        snprintf(buf,sizeof(buf),"game/assets/%s",path);
        f=fopen(buf,"rb");
        if(!f){ snprintf(buf,sizeof(buf),"assets/%s",path); f=fopen(buf,"rb"); }
    }
    if(!f) return ds_track_string(ds_strdup(""));
    fseek(f,0,SEEK_END); long sz=ftell(f); fseek(f,0,SEEK_SET);
    if(sz<0) sz=0; if(sz>10*1024*1024) sz=10*1024*1024;
    char *data=(char*)malloc(sz+1);
    if(!data){ fclose(f); return ds_track_string(ds_strdup("")); }
    size_t r=fread(data,1,sz,f); fclose(f);
    data[r]='\0';
    return ds_track_string(data);
}
int file_write(const char* path, const char* content){
    if(!path) return 0;
    FILE *f=fopen(path,"wb");
    if(!f) return 0;
    size_t len=content?strlen(content):0;
    size_t w=fwrite(content?content:"",1,len,f);
    fclose(f);
    return w==len;
}
int file_exists(const char* path){
    if(!path) return 0;
    FILE *f=fopen(path,"rb");
    if(f){ fclose(f); return 1; }
    char buf[512];
    snprintf(buf,sizeof(buf),"game/assets/%s",path);
    f=fopen(buf,"rb"); if(f){ fclose(f); return 1; }
    snprintf(buf,sizeof(buf),"assets/%s",path);
    f=fopen(buf,"rb"); if(f){ fclose(f); return 1; }
    return 0;
}
int file_del(const char* path){ if(!path) return 0; return remove(path)==0; }

/* ---------- json (мини-парсер, использует логику из net.c) ---------- */
static const char* skip_ws(const char* p){ while(p&&*p&&( *p==' '||*p=='\n'||*p=='\r'||*p=='\t')) p++; return p; }
static double parse_number(const char* p){ char *e; double v=strtod(p,&e); return v; }
double json_get_num(const char* json, const char* path){
    if(!json||!path) return 0;
    // очень простой: ищем "path": number   путь вида a/b/c
    // для простоты ищем последнее имя
    const char *key = strrchr(path,'/');
    if (key) key++; else key=path;
    char pattern[128];
    snprintf(pattern,sizeof(pattern),"\"%s\"",key);
    const char *pos=strstr(json,pattern);
    if(!pos) return 0;
    pos=strchr(pos,':');
    if(!pos) return 0;
    pos=skip_ws(pos+1);
    return parse_number(pos);
}
const char* json_get_str(const char* json, const char* path){
    if(!json||!path) return ds_track_string(ds_strdup(""));
    const char *key = strrchr(path,'/');
    if (key) key++; else key=path;
    char pattern[128];
    snprintf(pattern,sizeof(pattern),"\"%s\"",key);
    const char *pos=strstr(json,pattern);
    if(!pos) return ds_track_string(ds_strdup(""));
    pos=strchr(pos,':');
    if(!pos) return ds_track_string(ds_strdup(""));
    pos=skip_ws(pos+1);
    if(*pos!='"') return ds_track_string(ds_strdup(""));
    pos++;
    const char *end=strchr(pos,'"');
    if(!end) return ds_track_string(ds_strdup(""));
    size_t len=end-pos;
    char *out=(char*)malloc(len+1);
    if(!out) return ds_track_string(ds_strdup(""));
    memcpy(out,pos,len); out[len]='\0';
    return ds_track_string(out);
}
int json_get_bool(const char* json, const char* path){
    if(!json||!path) return 0;
    const char *key = strrchr(path,'/');
    if (key) key++; else key=path;
    char pattern[128];
    snprintf(pattern,sizeof(pattern),"\"%s\"",key);
    const char *pos=strstr(json,pattern);
    if(!pos) return 0;
    pos=strchr(pos,':');
    if(!pos) return 0;
    pos=skip_ws(pos+1);
    if(strncmp(pos,"true",4)==0) return 1;
    if(strncmp(pos,"false",5)==0) return 0;
    return parse_number(pos)!=0;
}

/* ---------- сеть высокого уровня (обёртка над http в net.c) ---------- */
const char* http_get(const char* url);
const char* http_post(const char* url, const char* body);
const char* http_get(const char* url){
    // используем file_read как заглушку для десктопа, на Android — net.c сделает
    // для простоты пробуем через file_read если url — путь
    if(!url) return ds_track_string(ds_strdup(""));
    if(strncmp(url,"http://",7)!=0 && strncmp(url,"https://",8)!=0){
        return file_read(url);
    }
    // если сеть доступна через net.c, можно было бы вызвать, но пока возвращаем пусто
    // реальная реализация есть в net.c через net_http, но требует java vm
    return ds_track_string(ds_strdup(""));
}
const char* http_post(const char* url, const char* body){
    (void)url; (void)body;
    return ds_track_string(ds_strdup(""));
}

/* ---------- утилиты ---------- */
double clamp(double v, double lo, double hi){ if(v<lo) return lo; if(v>hi) return hi; return v; }
double lerp(double a, double b, double t){ return a + (b-a)*t; }
double dist(double x1, double y1, double x2, double y2){ double dx=x2-x1, dy=y2-y1; return sqrt(dx*dx+dy*dy); }
double now(void){ return now_ms()/1000.0; }
