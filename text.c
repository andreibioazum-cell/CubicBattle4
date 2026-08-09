#include "text.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#if defined(__GNUC__) || defined(__clang__)
/* runtime.c supplies this hook in the Android build.  The weak declaration
 * keeps text.c independently testable on a desktop with `cc test_text.c
 * text.c`. */
#  ifdef DIMSCRIPT_TEXT_EMBEDDED
extern char *ds_track_string(char *string);
#  else
extern char *ds_track_string(char *string) __attribute__((weak));
#  endif
#else
extern char *ds_track_string(char *string);
#endif

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

int ds_ulen(const char *string) {
    int count = 0;

    if (!string) return 0;

    while (*string) {
        /* A byte is a continuation byte when its two high bits are 10.
         * Counting only non-continuation bytes gives the number of code
         * points in well-formed UTF-8. */
        if ((((unsigned char)*string) & 0xC0) != 0x80) {
            count++;
        }
        string++;
    }
    return count;
}

/* --------------------------------------------------------------------- */
/* Substrings and concatenation                                           */
/* --------------------------------------------------------------------- */

char *ds_substr(const char *string, int start, int length) {
    int total;
    char *out;

    if (!string) return ds_strdup_safe("");
    total = (int)strlen(string);
    if (start < 0) start = 0;
    if (start > total) start = total;
    if (length < 0) length = 0;
    if (start + length > total) length = total - start;

    out = (char *)malloc((size_t)length + 1);
    if (!out) return ds_strdup_safe("");
    memcpy(out, string + start, (size_t)length);
    out[length] = '\0';
    return out;
}

char *ds_concat(const char *left, const char *right) {
    size_t la = left ? strlen(left) : 0;
    size_t lb = right ? strlen(right) : 0;
    char *out = (char *)malloc(la + lb + 1);
    if (!out) return ds_strdup_safe("");
    if (la) memcpy(out, left, la);
    if (lb) memcpy(out + la, right, lb);
    out[la + lb] = '\0';
#ifdef DIMSCRIPT_TEXT_EMBEDDED
    return ds_track_string(out);
#else
    if (ds_track_string) return ds_track_string(out);
    return out;
#endif
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

/* --------------------------------------------------------------------- */
/* Transformation                                                         */
/* --------------------------------------------------------------------- */

char *ds_upper(const char *string) {
    char *out;
    char *p;

    if (!string) return ds_strdup_safe("");
    out = ds_strdup_safe(string);
    if (!out) return out;
    for (p = out; *p; p++) {
        *p = (char)toupper((unsigned char)*p);
    }
    return out;
}

char *ds_lower(const char *string) {
    char *out;
    char *p;

    if (!string) return ds_strdup_safe("");
    out = ds_strdup_safe(string);
    if (!out) return out;
    for (p = out; *p; p++) {
        *p = (char)tolower((unsigned char)*p);
    }
    return out;
}

char *ds_trim(const char *string) {
    const char *start;
    const char *end;
    size_t length;
    char *out;

    if (!string) return ds_strdup_safe("");
    start = string;
    while (*start && isspace((unsigned char)*start)) start++;
    end = string + strlen(string);
    while (end > start && isspace((unsigned char)*(end - 1))) end--;

    length = (size_t)(end - start);
    out = (char *)malloc(length + 1);
    if (!out) return ds_strdup_safe("");
    memcpy(out, start, length);
    out[length] = '\0';
    return out;
}

char *ds_replace(const char *string, const char *from, const char *to) {
    size_t from_len;
    size_t to_len;
    size_t out_cap;
    size_t out_len;
    char *out;
    const char *cursor;
    const char *hit;

    if (!string) return ds_strdup_safe("");
    if (!from || !*from) return ds_strdup_safe(string);
    if (!to) to = "";

    from_len = strlen(from);
    to_len = strlen(to);

    /* First pass: count occurrences to size the output buffer. */
    out_cap = strlen(string) + 1;
    cursor = string;
    while ((hit = strstr(cursor, from)) != NULL) {
        out_cap += to_len > from_len ? to_len - from_len : 0;
        cursor = hit + from_len;
    }

    out = (char *)malloc(out_cap);
    if (!out) return ds_strdup_safe("");
    out_len = 0;

    cursor = string;
    while ((hit = strstr(cursor, from)) != NULL) {
        size_t gap = (size_t)(hit - cursor);
        memcpy(out + out_len, cursor, gap);
        out_len += gap;
        memcpy(out + out_len, to, to_len);
        out_len += to_len;
        cursor = hit + from_len;
    }
    /* Tail after the last match. */
    {
        size_t tail = strlen(cursor);
        memcpy(out + out_len, cursor, tail);
        out_len += tail;
    }
    out[out_len] = '\0';
    return out;
}

/* The legacy Android workflow embeds text.c through runtime.c.  Weak
 * standalone definitions keep builds which also list text.c as a separate
 * translation unit link without duplicate symbols. */
#if !defined(DIMSCRIPT_TEXT_EMBEDDED) && (defined(__GNUC__) || defined(__clang__))
#pragma weak ds_len
#pragma weak ds_ulen
#pragma weak ds_substr
#pragma weak ds_concat
#pragma weak ds_find
#pragma weak ds_upper
#pragma weak ds_lower
#pragma weak ds_trim
#pragma weak ds_contains
#pragma weak ds_starts_with
#pragma weak ds_ends_with
#pragma weak ds_replace
#endif
