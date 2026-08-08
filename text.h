#ifndef DS_TEXT_H
#define DS_TEXT_H

/* Lightweight text utilities for the DimScript runtime.
 *
 * These deliberately depend only on <string.h>, <ctype.h> and <stdlib.h> so
 * the Android NDK build does not pick up a heavy third-party dependency such
 * as ICU or GLib.  Strings are plain NUL-terminated UTF-8 byte arrays; the
 * Unicode-aware helpers count code points instead of raw bytes where it
 * matters to script authors.
 *
 * Every function that allocates returns a malloc'd buffer the caller must
 * free.  Functions returning a boolean use int (0/1) to match the rest of
 * the runtime. */

#include <stddef.h>

/* Length in bytes, excluding the NUL terminator.  Mirrors strlen but takes
 * NULL gracefully so script code cannot crash the engine on a nil string. */
int ds_len(const char *string);

/* Length in Unicode code points (UTF-8).  ASCII strings give the same
 * answer as ds_len; multibyte characters (Cyrillic, emoji, etc.) are
 * counted as one instead of two or four bytes. */
int ds_ulen(const char *string);

/* Substring extraction.  start is a zero-based byte offset; length is the
 * number of bytes to copy.  Out-of-range values are clamped instead of
 * raising, which keeps script code predictable. */
char *ds_substr(const char *string, int start, int length);

/* Concatenation of two strings.  The caller frees the result. */
char *ds_concat(const char *left, const char *right);

/* Search for the first occurrence of needle inside haystack starting at
 * byte offset from.  Returns the byte index or -1 when absent. */
int ds_find(const char *haystack, const char *needle, int from);

/* Case conversion.  Only the ASCII range is folded; non-ASCII bytes pass
 * through unchanged, which is the safe default for UTF-8 text. */
char *ds_upper(const char *string);
char *ds_lower(const char *string);

/* Strip leading and trailing ASCII whitespace. */
char *ds_trim(const char *string);

/* Predicates. */
int ds_contains(const char *haystack, const char *needle);
int ds_starts_with(const char *string, const char *prefix);
int ds_ends_with(const char *string, const char *suffix);

/* Replace every occurrence of `from` with `to`.  When `from` is empty the
 * original string is duplicated so the engine does not loop forever. */
char *ds_replace(const char *string, const char *from, const char *to);

#endif
