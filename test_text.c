/* Standalone sanity check for the text utilities.
 *
 * Build locally (not for Android):
 *   cc -std=c99 -Wall -Wextra -o test_text test_text.c text.c
 *   ./test_text
 */

#include "text.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;

#define CHECK(expr)                                                          \
    do {                                                                     \
        if (!(expr)) {                                                       \
            fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #expr);  \
            failures++;                                                      \
        }                                                                    \
    } while (0)

static void check_str(const char *actual, const char *expected, const char *label) {
    if (!actual || strcmp(actual, expected) != 0) {
        fprintf(stderr,
                "FAIL %s: got \"%s\", expected \"%s\"\n",
                label,
                actual ? actual : "(null)",
                expected);
        failures++;
    }
}

int main(void) {
    char *tmp;

    /* ds_len / ds_ulen */
    CHECK(ds_len("hello") == 5);
    CHECK(ds_len("") == 0);
    CHECK(ds_len(NULL) == 0);

    /* Cyrillic "Привет" is 12 bytes but 6 code points in UTF-8. */
    CHECK(ds_len("\xd0\x9f\xd1\x80\xd0\xb8\xd0\xb2\xd0\xb5\xd1\x82") == 12);
    CHECK(ds_ulen("\xd0\x9f\xd1\x80\xd0\xb8\xd0\xb2\xd0\xb5\xd1\x82") == 6);
    CHECK(ds_ulen("ascii") == 5);

    /* ds_substr */
    tmp = ds_substr("hello world", 6, 5);
    check_str(tmp, "world", "ds_substr basic");
    free(tmp);

    tmp = ds_substr("hello", 0, 100);
    check_str(tmp, "hello", "ds_substr clamp");
    free(tmp);

    /* ds_concat */
    tmp = ds_concat("foo", "bar");
    check_str(tmp, "foobar", "ds_concat");
    free(tmp);

    /* ds_find / ds_contains */
    CHECK(ds_find("hello world", "world", 0) == 6);
    CHECK(ds_find("hello world", "xyz", 0) == -1);
    CHECK(ds_find("aaa", "a", 1) == 1);
    CHECK(ds_contains("hello", "ell") == 1);
    CHECK(ds_contains("hello", "xyz") == 0);

    /* ds_starts_with / ds_ends_with */
    CHECK(ds_starts_with("hello world", "hello") == 1);
    CHECK(ds_starts_with("hello", "world") == 0);
    CHECK(ds_ends_with("hello world", "world") == 1);
    CHECK(ds_ends_with("hello", "world") == 0);

    /* ds_upper / ds_lower */
    tmp = ds_upper("Hello World");
    check_str(tmp, "HELLO WORLD", "ds_upper");
    free(tmp);

    tmp = ds_lower("Hello World");
    check_str(tmp, "hello world", "ds_lower");
    free(tmp);

    /* ds_trim */
    tmp = ds_trim("  \t hello \n ");
    check_str(tmp, "hello", "ds_trim");
    free(tmp);

    /* ds_replace */
    tmp = ds_replace("foo bar foo", "foo", "baz");
    check_str(tmp, "baz bar baz", "ds_replace multi");
    free(tmp);

    tmp = ds_replace("hello", "xyz", "abc");
    check_str(tmp, "hello", "ds_replace no match");
    free(tmp);

    tmp = ds_replace("aaa", "a", "bb");
    check_str(tmp, "bbbbbb", "ds_replace grow");
    free(tmp);

    /* NULL safety */
    tmp = ds_substr(NULL, 0, 5);
    check_str(tmp, "", "ds_substr NULL");
    free(tmp);

    tmp = ds_replace(NULL, "a", "b");
    check_str(tmp, "", "ds_replace NULL");
    free(tmp);

    if (failures == 0) {
        printf("All text utility tests passed.\n");
    } else {
        printf("%d test(s) failed.\n", failures);
    }

    return failures == 0 ? 0 : 1;
}
