#include "runtime.h"

#include <stdarg.h>
#include <stdio.h>

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

/* Все строки, созданные на лету (конкатенация, число в строку), живут в
 * этом пуле и освобождаются разом при перезапуске скрипта. */
typedef struct DSStringNode DSStringNode;
struct DSStringNode {
    DSStringNode *next;
    char *string;
};
static DSStringNode *ds_strings = NULL;

void ds_runtime_error(const char *format, ...) {
    va_list args;
    va_list copy;
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

const char *ds_runtime_error_message(void) {
    return ds_last_error[0] ? ds_last_error : "unknown DimScript runtime error";
}

int ds_script_has_error(void) { return ds_has_error; }

void ds_clear_runtime_error(void) {
    ds_has_error = 0;
    ds_last_error[0] = '\0';
}

void ds_request_script_restart(void) { ds_restart_requested = 1; }
int ds_script_restart_requested(void) { return ds_restart_requested; }
void ds_clear_script_restart(void) { ds_restart_requested = 0; }

static char *ds_strdup(const char *string) {
    size_t length;
    char *copy;
    if (!string) string = "";
    length = strlen(string) + 1;
    copy = (char *)malloc(length);
    if (copy) memcpy(copy, string, length);
    return copy;
}

static char *ds_track_string(char *string) {
    DSStringNode *node;
    if (!string) {
        ds_runtime_error("out of memory while creating a string");
        return NULL;
    }
    node = (DSStringNode *)malloc(sizeof(*node));
    if (!node) {
        free(string);
        ds_runtime_error("out of memory while tracking a string");
        return NULL;
    }
    node->string = string;
    node->next = ds_strings;
    ds_strings = node;
    return string;
}

char *ds_num_to_string(double number) {
    char buffer[96];
    if (snprintf(buffer, sizeof(buffer), "%g", number) < 0) return NULL;
    return ds_track_string(ds_strdup(buffer));
}

void ds_string_pool_reset(void) {
    DSStringNode *node = ds_strings;
    while (node) {
        DSStringNode *next = node->next;
        free(node->string);
        free(node);
        node = next;
    }
    ds_strings = NULL;
}

char *ds_concat(const char *left, const char *right) {
    size_t la = left ? strlen(left) : 0;
    size_t lb = right ? strlen(right) : 0;
    char *out = (char *)malloc(la + lb + 1);
    if (!out) return ds_track_string(ds_strdup(""));
    if (la) memcpy(out, left, la);
    if (lb) memcpy(out + lb, right, lb);
    out[la + lb] = '\0';
    return ds_track_string(out);
}
