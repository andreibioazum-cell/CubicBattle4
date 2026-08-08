#include "runtime.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>

Table *G = NULL;
Table *L = NULL;
Joy joy = {0};
int screen_w = 0;
int screen_h = 0;
double fps = 0.0;

static char *ds_strdup(const char *string) {
    size_t length;
    char *copy;

    if (!string) {
        return NULL;
    }

    length = strlen(string) + 1;
    copy = (char *)malloc(length);
    if (!copy) {
        ds_runtime_error("out of memory while copying a string");
        return NULL;
    }

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

    va_start(args, format);
    __android_log_vprint(ANDROID_LOG_ERROR, "DimScript", format, args);
    va_end(args);
}

Table *T_new(void) {
    Table *table = (Table *)calloc(1, sizeof(*table));

    if (!table) {
        ds_runtime_error("out of memory while creating a table");
    }

    return table;
}

/* Values passed by the generated C code are either pointers to Val (numbers,
 * strings, vectors, and nil) or a raw Table* for DS_TABLE.  Always copying a
 * Val here gives table entries a stable lifetime instead of retaining a
 * pointer to a block-scope compound literal. */
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

int T_set(Table *table, const char *key, const void *value, int type) {
    uint32_t hash;
    Entry *entry;
    Val *new_value;

    if (!table || !key || !*key) {
        ds_runtime_error("cannot assign a value without a table and key");
        return 0;
    }

    new_value = copy_value(value, type);
    if (!new_value) {
        return 0;
    }

    hash = ds_hash(key);
    entry = table->buckets[hash % DS_TABLE_SIZE];
    while (entry) {
        if (entry->hash == hash && strcmp(entry->key, key) == 0) {
            /* Keep the Val address stable.  Script code may retain the result
             * of T_get while assigning the same key again. */
            if (entry->value) {
                *entry->value = *new_value;
                free(new_value);
            } else {
                entry->value = new_value;
            }
            return 1;
        }
        entry = entry->next;
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
        return 0;
    }

    entry->hash = hash;
    entry->value = new_value;
    entry->next = table->buckets[hash % DS_TABLE_SIZE];
    table->buckets[hash % DS_TABLE_SIZE] = entry;
    table->count++;
    return 1;
}

Val *T_get(Table *table, const char *key, int *type) {
    uint32_t hash;
    Entry *entry;

    if (type) {
        *type = DS_NIL;
    }

    if (!table || !key || !*key) {
        return NULL;
    }

    hash = ds_hash(key);
    entry = table->buckets[hash % DS_TABLE_SIZE];
    while (entry) {
        if (entry->hash == hash && strcmp(entry->key, key) == 0) {
            if (type) {
                *type = entry->value ? entry->value->type : DS_NIL;
            }
            return entry->value;
        }
        entry = entry->next;
    }

    return NULL;
}

typedef struct {
    Table **items;
    size_t count;
    size_t capacity;
} FreedTables;

static int remember_table(FreedTables *freed, Table *table) {
    size_t i;
    Table **new_items;

    for (i = 0; i < freed->count; ++i) {
        if (freed->items[i] == table) return 0;
    }

    if (freed->count == freed->capacity) {
        size_t new_capacity = freed->capacity ? freed->capacity * 2 : 8;
        new_items = (Table **)realloc(freed->items, new_capacity * sizeof(*new_items));
        if (!new_items) {
            ds_runtime_error("out of memory while releasing tables");
            return 0;
        }
        freed->items = new_items;
        freed->capacity = new_capacity;
    }

    freed->items[freed->count++] = table;
    return 1;
}

static void free_table_recursive(Table *table, FreedTables *freed) {
    int bucket;

    if (!table || !remember_table(freed, table)) return;

    for (bucket = 0; bucket < DS_TABLE_SIZE; ++bucket) {
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

    free(table);
}

/* T_free releases the table, its values, and nested tables.  The visited set
 * makes shared and cyclic table references safe instead of turning cleanup
 * into a double-free. */
void T_free(Table *table) {
    FreedTables freed = {0};
    free_table_recursive(table, &freed);
    free(freed.items);
}

void print(Val value) {
    switch (value.type) {
        case DS_NUMBER:
            __android_log_print(ANDROID_LOG_INFO, "DimScript", "%g", value.num);
            break;
        case DS_STRING:
            __android_log_print(ANDROID_LOG_INFO, "DimScript", "%s", value.str ? value.str : "");
            break;
        case DS_TABLE:
            __android_log_print(ANDROID_LOG_INFO, "DimScript", "<table>");
            break;
        case DS_VEC2:
            __android_log_print(ANDROID_LOG_INFO, "DimScript", "(%g, %g)", value.v2.x, value.v2.y);
            break;
        case DS_VEC3:
            __android_log_print(ANDROID_LOG_INFO, "DimScript", "(%g, %g, %g)", value.v3.x, value.v3.y, value.v3.z);
            break;
        default:
            __android_log_print(ANDROID_LOG_INFO, "DimScript", "nil");
            break;
    }
}

void printn(double number) {
    __android_log_print(ANDROID_LOG_INFO, "DimScript", "%g", number);
}

void prints(const char *string) {
    __android_log_print(ANDROID_LOG_INFO, "DimScript", "%s", string ? string : "");
}

double tonumber(Val value) {
    char *end;

    if (value.type == DS_NUMBER) {
        return value.num;
    }
    if (value.type != DS_STRING || !value.str) {
        return 0.0;
    }

    errno = 0;
    end = NULL;
    {
        double number = strtod(value.str, &end);
        if (errno != 0 || end == value.str || (end && *end != '\0')) {
            return 0.0;
        }
        return number;
    }
}

const char *tostring(Val value) {
    static char buffer[64];

    if (value.type == DS_STRING && value.str) {
        return value.str;
    }
    if (value.type == DS_NUMBER) {
        snprintf(buffer, sizeof(buffer), "%g", value.num);
        return buffer;
    }

    return "nil";
}

const char *ds_str_cat(const char *prefix, double val) {
    static char buffer[128];
    if (val == (int)val) {
        snprintf(buffer, sizeof(buffer), "%s%d", prefix ? prefix : "", (int)val);
    } else {
        snprintf(buffer, sizeof(buffer), "%s%.2f", prefix ? prefix : "", val);
    }
    return buffer;
}
