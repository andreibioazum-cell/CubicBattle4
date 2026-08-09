#include "runtime.h"

#include <stdarg.h>
#include <stdio.h>

#define DS_INITIAL_TABLE_CAPACITY 16u
#define DS_MAX_TABLE_LOAD_NUMERATOR 3u
#define DS_MAX_TABLE_LOAD_DENOMINATOR 4u
#define DS_ERROR_MESSAGE_SIZE 1024

typedef struct DSStringNode DSStringNode;
struct DSStringNode {
    DSStringNode *next;
    char *string;
};

Table *G = NULL;
Table *L = NULL;
Joy joy = {0};
int screen_w = 0;
int screen_h = 0;
double fps = 0.0;

static jmp_buf ds_error_jump;
static int ds_error_handler_active = 0;
static int ds_has_error = 0;
static int ds_restart_requested = 0;
static char ds_last_error[DS_ERROR_MESSAGE_SIZE] = {0};
static DSStringNode *ds_strings = NULL;

static void ds_log_v(int priority, const char *format, va_list args) {
    __android_log_vprint(priority, "DimScript", format, args);
}

static char *ds_strdup(const char *string) {
    size_t length;
    char *copy;

    if (!string) string = "";
    length = strlen(string) + 1;
    copy = (char *)malloc(length);
    if (!copy) return NULL;
    memcpy(copy, string, length);
    return copy;
}

static uint32_t ds_hash(const char *string) {
    uint32_t hash = 2166136261u;
    while (string && *string) {
        hash ^= (unsigned char)*string++;
        hash *= 16777619u;
    }
    return hash;
}

void ds_runtime_error(const char *format, ...) {
    va_list args;
    va_list copy;

    va_start(args, format);
    va_copy(copy, args);
    vsnprintf(ds_last_error, sizeof(ds_last_error), format, copy);
    va_end(copy);
    ds_has_error = 1;
    ds_log_v(ANDROID_LOG_ERROR, format, args);
    va_end(args);

    /* A script error is a controlled exception.  Do not return to generated
     * code after reporting it: partially written state is not safe to draw. */
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
        /* Hook calls are normally not nested.  If an embedding host does nest
         * them, the outer boundary still owns the longjmp target. */
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
void ds_restart_script(void) { ds_request_script_restart(); }
int ds_script_restart_requested(void) { return ds_restart_requested; }
void ds_clear_script_restart(void) { ds_restart_requested = 0; }

static int table_init(Table *table, size_t capacity) {
    table->buckets = (Entry **)calloc(capacity, sizeof(*table->buckets));
    if (!table->buckets) return 0;
    table->capacity = capacity;
    table->count = 0;
    table->version = 1;
    return 1;
}

Table *T_new(void) {
    Table *table = (Table *)calloc(1, sizeof(*table));
    if (!table || !table_init(table, DS_INITIAL_TABLE_CAPACITY)) {
        free(table);
        ds_runtime_error("out of memory while creating a table");
        return NULL;
    }
    return table;
}

static Val *copy_value(const void *value, int type) {
    Val *copy = (Val *)calloc(1, sizeof(*copy));
    if (!copy) {
        ds_runtime_error("out of memory while storing a value");
        return NULL;
    }
    if (type == DS_TABLE) {
        copy->type = DS_TABLE;
        copy->table = (Table *)value;
    } else if (value) {
        *copy = *(const Val *)value;
        copy->type = type;
    } else {
        copy->type = DS_NIL;
    }
    return copy;
}

static int table_resize(Table *table, size_t capacity) {
    Entry **buckets;
    size_t i;

    buckets = (Entry **)calloc(capacity, sizeof(*buckets));
    if (!buckets) {
        ds_runtime_error("out of memory while growing a table");
        return 0;
    }
    for (i = 0; i < table->capacity; ++i) {
        Entry *entry = table->buckets[i];
        while (entry) {
            Entry *next = entry->next;
            size_t slot = entry->hash % capacity;
            entry->next = buckets[slot];
            buckets[slot] = entry;
            entry = next;
        }
    }
    free(table->buckets);
    table->buckets = buckets;
    table->capacity = capacity;
    table->version++;
    return 1;
}

int T_set(Table *table, const char *key, const void *value, int type) {
    uint32_t hash;
    Entry *entry;
    Val *new_value;
    size_t slot;

    if (!table || !key || !*key || !table->buckets || table->capacity == 0) {
        ds_runtime_error("cannot assign a value without a table and key");
        return 0;
    }
    new_value = copy_value(value, type);
    if (!new_value) return 0;

    hash = ds_hash(key);
    slot = hash % table->capacity;
    entry = table->buckets[slot];
    while (entry) {
        if (entry->hash == hash && strcmp(entry->key, key) == 0) {
            if (entry->value) {
                *entry->value = *new_value;
                free(new_value);
            } else {
                entry->value = new_value;
            }
            table->version++;
            return 1;
        }
        entry = entry->next;
    }

    if ((table->count + 1) * DS_MAX_TABLE_LOAD_DENOMINATOR >
        table->capacity * DS_MAX_TABLE_LOAD_NUMERATOR) {
        if (!table_resize(table, table->capacity * 2)) {
            free(new_value);
            return 0;
        }
        slot = hash % table->capacity;
    }

    entry = (Entry *)calloc(1, sizeof(*entry));
    if (!entry) {
        free(new_value);
        ds_runtime_error("out of memory while creating table entry for '%s'", key);
        return 0;
    }
    entry->key = ds_strdup(key);
    if (!entry->key) {
        free(new_value);
        free(entry);
        ds_runtime_error("out of memory while copying table key '%s'", key);
        return 0;
    }
    entry->hash = hash;
    entry->value = new_value;
    entry->next = table->buckets[slot];
    table->buckets[slot] = entry;
    table->count++;
    table->version++;
    return 1;
}

Val *T_get(Table *table, const char *key, int *type) {
    uint32_t hash;
    Entry *entry;
    size_t slot;

    if (type) *type = DS_NIL;
    if (!table || !table->buckets || !key || !*key || table->capacity == 0) return NULL;
    hash = ds_hash(key);
    slot = hash % table->capacity;
    entry = table->buckets[slot];
    while (entry) {
        if (entry->hash == hash && strcmp(entry->key, key) == 0) {
            if (type) *type = entry->value ? entry->value->type : DS_NIL;
            return entry->value;
        }
        entry = entry->next;
    }
    return NULL;
}

Val *T_get_cached(Table *table, const char *key, DSLookupCache *cache, int *type) {
    uint32_t hash;
    Val *value;

    if (type) *type = DS_NIL;
    if (!table || !key) return NULL;
    hash = ds_hash(key);
    if (cache && cache->table == table && cache->version == table->version &&
        cache->hash == hash && cache->key && strcmp(cache->key, key) == 0) {
        if (type) *type = cache->value ? cache->value->type : DS_NIL;
        return cache->value;
    }
    value = T_get(table, key, type);
    if (cache) {
        cache->table = table;
        cache->key = key;
        cache->hash = hash;
        cache->version = table->version;
        cache->value = value;
    }
    return value;
}

typedef struct {
    Table **items;
    size_t count;
    size_t capacity;
} FreedTables;

static int remember_table(FreedTables *freed, Table *table) {
    size_t i;
    Table **new_items;
    for (i = 0; i < freed->count; ++i) if (freed->items[i] == table) return 0;
    if (freed->count == freed->capacity) {
        size_t capacity = freed->capacity ? freed->capacity * 2 : 8;
        new_items = (Table **)realloc(freed->items, capacity * sizeof(*new_items));
        if (!new_items) return 0;
        freed->items = new_items;
        freed->capacity = capacity;
    }
    freed->items[freed->count++] = table;
    return 1;
}

static void free_table_recursive(Table *table, FreedTables *freed) {
    size_t bucket;
    if (!table || !remember_table(freed, table)) return;
    for (bucket = 0; bucket < table->capacity; ++bucket) {
        Entry *entry = table->buckets[bucket];
        while (entry) {
            Entry *next = entry->next;
            if (entry->value && entry->value->type == DS_TABLE) {
                free_table_recursive(entry->value->table, freed);
            }
            free(entry->key);
            free(entry->value);
            free(entry);
            entry = next;
        }
    }
    free(table->buckets);
    free(table);
}

void T_free(Table *table) {
    FreedTables freed = {0};
    free_table_recursive(table, &freed);
    free(freed.items);
}

char *ds_track_string(char *string) {
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
    char *result;
    int length = snprintf(buffer, sizeof(buffer), "%g", number);
    if (length < 0) return NULL;
    result = (char *)malloc((size_t)length + 1);
    if (!result) {
        ds_runtime_error("out of memory while formatting a number");
        return NULL;
    }
    memcpy(result, buffer, (size_t)length + 1);
    return ds_track_string(result);
}

char *ds_bool_to_string(int value) {
    return ds_track_string(ds_strdup(value ? "true" : "false"));
}

void ds_string_release(char *string) {
    DSStringNode **cursor = &ds_strings;
    if (!string) return;
    while (*cursor) {
        if ((*cursor)->string == string) {
            DSStringNode *node = *cursor;
            *cursor = node->next;
            free(node->string);
            free(node);
            return;
        }
        cursor = &(*cursor)->next;
    }
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

/* --------------------------------------------------------------------- */
/* Helpers                                                                */
/* --------------------------------------------------------------------- */

static char *ds_strdup_safe(const char *string) {
    size_t length;
    char *copy;

    if (!string) {
        copy = (char *)malloc(1);
        if (copy) copy[0] = '\0';
        return copy;
    }

    length = strlen(string) + 1;
    copy = (char *)malloc(length);
    if (copy) memcpy(copy, string, length);
    return copy;
}

/* --------------------------------------------------------------------- */
/* Length                                                                 */
/* --------------------------------------------------------------------- */

int ds_len(const char *string) {
    return string ? (int)strlen(string) : 0;
}

char *ds_concat(const char *left, const char *right) {
    size_t la = left ? strlen(left) : 0;
    size_t lb = right ? strlen(right) : 0;
    char *out = (char *)malloc(la + lb + 1);
    if (!out) return ds_strdup_safe("");
    if (la) memcpy(out, left, la);
    if (lb) memcpy(out + la, right, lb);
    out[la + lb] = '\0';
    return ds_track_string(out);
}

/* --------------------------------------------------------------------- */
/* Search                                                                 */
/* --------------------------------------------------------------------- */

int ds_find(const char *haystack, const char *needle, int from) {
    const char *hit;

    if (!haystack || !needle || !*needle) return -1;
    if (from < 0) from = 0;
    if (from > (int)strlen(haystack)) return -1;

    hit = strstr(haystack + from, needle);
    return hit ? (int)(hit - haystack) : -1;
}

int ds_contains(const char *haystack, const char *needle) {
    return ds_find(haystack, needle, 0) >= 0;
}

int ds_starts_with(const char *string, const char *prefix) {
    size_t plen;
    if (!string || !prefix) return 0;
    plen = strlen(prefix);
    return strncmp(string, prefix, plen) == 0;
}

int ds_ends_with(const char *string, const char *suffix) {
    size_t slen, xlen;
    if (!string || !suffix) return 0;
    slen = strlen(string);
    xlen = strlen(suffix);
    if (xlen > slen) return 0;
    return memcmp(string + slen - xlen, suffix, xlen) == 0;
}

