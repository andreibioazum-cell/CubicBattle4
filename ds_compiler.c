#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>

#define DS_MAX_LINE       4096
#define DS_MAX_SOURCE     4096
#define DS_MAX_FUNCTIONS  128
#define DS_MAX_PARAMS     16
#define DS_MAX_LOCALS     64
#define DS_MAX_NAME       64

/* DimScript is compiled directly to the small C runtime.  It is deliberately
 * not an interpreter and it never catches script failures: a malformed
 * program is reported by this compiler and a runtime error is sent to logcat
 * instead of being hidden. */

typedef struct {
    char *text;
    const char *source_name;
    int source_line;
} SourceLine;

typedef struct {
    SourceLine lines[DS_MAX_SOURCE];
    int count;
} Source;

typedef struct {
    char name[DS_MAX_NAME];
    char params[DS_MAX_PARAMS][DS_MAX_NAME];
    int param_count;
    int body_start;
    int body_end;
} Function;

typedef struct {
    FILE *out;
    const char *source_name;
    Source *source;
    Function functions[DS_MAX_FUNCTIONS];
    int function_count;
    int line_number;
    int errors;
    int table_number;
    char locals[DS_MAX_LOCALS][DS_MAX_NAME];
    int local_count;
} Compiler;

static Compiler C;

static char *copy_string(const char *text) {
    size_t length;
    char *copy;

    if (!text) return NULL;
    length = strlen(text) + 1;
    copy = (char *)malloc(length);
    if (!copy) return NULL;
    memcpy(copy, text, length);
    return copy;
}

static char *trim(char *text) {
    char *end;

    while (*text && isspace((unsigned char)*text)) ++text;
    end = text + strlen(text);
    while (end > text && isspace((unsigned char)end[-1])) --end;
    *end = '\0';
    return text;
}

static void remove_comment(char *text) {
    int in_string = 0;
    int escaped = 0;
    size_t i;

    for (i = 0; text[i]; ++i) {
        char current = text[i];
        if (in_string) {
            if (escaped) {
                escaped = 0;
            } else if (current == '\\') {
                escaped = 1;
            } else if (current == '"') {
                in_string = 0;
            }
            continue;
        }
        if (current == '"') {
            in_string = 1;
        } else if ((current == '-' && text[i + 1] == '-') ||
                   (current == '/' && text[i + 1] == '/')) {
            text[i] = '\0';
            return;
        }
    }
}

static int is_identifier_start(char character) {
    return isalpha((unsigned char)character) || character == '_';
}

static int is_identifier_part(char character) {
    return isalnum((unsigned char)character) || character == '_';
}

static int valid_identifier(const char *name) {
    size_t i;

    if (!name || !is_identifier_start(*name)) return 0;
    for (i = 1; name[i]; ++i) {
        if (!is_identifier_part(name[i])) return 0;
    }
    return 1;
}

static int starts_keyword(const char *line, const char *keyword) {
    size_t length = strlen(keyword);
    if (strncmp(line, keyword, length) != 0) return 0;
    return line[length] == '\0' || isspace((unsigned char)line[length]) ||
           line[length] == '(';
}

static void compiler_error(const char *format, ...) {
    va_list args;

    ++C.errors;
    fprintf(stderr, "%s:%d: error: ", C.source_name ? C.source_name : "<source>", C.line_number);
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    fputc('\n', stderr);
}

static int load_source(const char *path, Source *source) {
    FILE *input;
    char buffer[DS_MAX_LINE];
    int line_number = 0;

    input = fopen(path, "r");
    if (!input) {
        fprintf(stderr, "Error: cannot open %s: %s\n", path, strerror(errno));
        return 0;
    }

    while (fgets(buffer, sizeof(buffer), input)) {
        char *line;
        ++line_number;
        if (source->count >= DS_MAX_SOURCE) {
            fprintf(stderr, "Error: %s has more than %d lines\n", path, DS_MAX_SOURCE);
            fclose(input);
            return 0;
        }
        if (!strchr(buffer, '\n') && !feof(input)) {
            fprintf(stderr, "Error: line %d in %s is longer than %d characters\n",
                    line_number, path, DS_MAX_LINE - 1);
            fclose(input);
            return 0;
        }
        remove_comment(buffer);
        line = trim(buffer);
        source->lines[source->count].text = copy_string(line);
        if (!source->lines[source->count].text) {
            fprintf(stderr, "Error: out of memory while reading %s\n", path);
            fclose(input);
            return 0;
        }
        source->lines[source->count].source_name = path;
        source->lines[source->count].source_line = line_number;
        ++source->count;
    }

    if (ferror(input)) {
        fprintf(stderr, "Error: cannot read %s\n", path);
        fclose(input);
        return 0;
    }
    fclose(input);
    return 1;
}

static void free_source(Source *source) {
    int i;
    for (i = 0; i < source->count; ++i) {
        free(source->lines[i].text);
        source->lines[i].text = NULL;
    }
    source->count = 0;
}

static int parse_function_header(const char *line, Function *function) {
    const char *name_start;
    const char *open;
    const char *close;
    size_t name_length;
    char params[DS_MAX_LINE];
    char *part;
    char *cursor;

    if (!starts_keyword(line, "function")) return 0;
    name_start = line + strlen("function");
    while (isspace((unsigned char)*name_start)) ++name_start;
    open = strchr(name_start, '(');
    close = strrchr(name_start, ')');
    if (!open || !close || close < open) {
        compiler_error("function declaration must look like function name(args)");
        return -1;
    }

    name_length = (size_t)(open - name_start);
    if (name_length == 0 || name_length >= sizeof(function->name)) {
        compiler_error("function name is missing or too long");
        return -1;
    }
    memcpy(function->name, name_start, name_length);
    function->name[name_length] = '\0';
    if (!valid_identifier(function->name)) {
        compiler_error("invalid function name '%s'", function->name);
        return -1;
    }

    memset(params, 0, sizeof(params));
    if ((size_t)(close - open - 1) >= sizeof(params)) {
        compiler_error("parameter list for '%s' is too long", function->name);
        return -1;
    }
    memcpy(params, open + 1, (size_t)(close - open - 1));
    params[close - open - 1] = '\0';

    function->param_count = 0;
    cursor = trim(params);
    if (*cursor) {
        while (cursor && *cursor) {
            char *comma = strchr(cursor, ',');
            if (comma) *comma = '\0';
            part = trim(cursor);
            if (!valid_identifier(part)) {
                compiler_error("invalid parameter '%s' in function '%s'", part, function->name);
                return -1;
            }
            if (function->param_count >= DS_MAX_PARAMS) {
                compiler_error("too many parameters in function '%s'", function->name);
                return -1;
            }
            snprintf(function->params[function->param_count], DS_MAX_NAME, "%s", part);
            ++function->param_count;
            cursor = comma ? comma + 1 : NULL;
        }
    }

    return 1;
}

static int block_starts(const char *line) {
    return starts_keyword(line, "if") || starts_keyword(line, "while") ||
           starts_keyword(line, "for") || starts_keyword(line, "function");
}

static int collect_functions(void) {
    int i = 0;

    while (i < C.source->count) {
        Function function;
        int parsed;
        int depth;
        int j;

        memset(&function, 0, sizeof(function));
        C.source_name = C.source->lines[i].source_name;
        C.line_number = C.source->lines[i].source_line;
        parsed = parse_function_header(C.source->lines[i].text, &function);
        if (parsed <= 0) {
            if (parsed < 0) return 0;
            ++i;
            continue;
        }
        if (C.function_count >= DS_MAX_FUNCTIONS) {
            compiler_error("too many functions");
            return 0;
        }

        function.body_start = i + 1;
        depth = 1;
        function.body_end = -1;
        for (j = i + 1; j < C.source->count; ++j) {
            const char *line = C.source->lines[j].text;
            if (block_starts(line)) {
                ++depth;
            } else if (strcmp(line, "end") == 0) {
                --depth;
                if (depth == 0) {
                    function.body_end = j;
                    break;
                }
            }
        }
        if (function.body_end < 0) {
            C.line_number = C.source->lines[i].source_line;
            compiler_error("function '%s' has no matching end", function.name);
            return 0;
        }

        for (j = 0; j < C.function_count; ++j) {
            if (strcmp(C.functions[j].name, function.name) == 0) {
                compiler_error("function '%s' is defined more than once", function.name);
                return 0;
            }
        }
        C.functions[C.function_count++] = function;
        i = function.body_end + 1;
    }

    return C.errors == 0;
}

static int function_index(const char *name) {
    int i;
    for (i = 0; i < C.function_count; ++i) {
        if (strcmp(C.functions[i].name, name) == 0) return i;
    }
    return -1;
}

static int is_local(const char *name) {
    int i;
    for (i = 0; i < C.local_count; ++i) {
        if (strcmp(C.locals[i], name) == 0) return 1;
    }
    return 0;
}

static int add_local(const char *name) {
    if (is_local(name)) return 1;
    if (C.local_count >= DS_MAX_LOCALS) {
        compiler_error("too many local variables");
        return 0;
    }
    snprintf(C.locals[C.local_count], DS_MAX_NAME, "%s", name);
    ++C.local_count;
    return 1;
}

static void emit_indent(void) {
    fputs("    ", C.out);
}

static int is_builtin_function(const char *name) {
    static const char *const names[] = {
        "cls", "rect", "circle", "ring", "tex", "text",
        "print", "printn", "prints", "sqrt", "sin", "cos", "atan2",
        NULL
    };
    int i;
    for (i = 0; names[i]; ++i) {
        if (strcmp(name, names[i]) == 0) return 1;
    }
    return 0;
}

static void emit_expression(const char *expression);

static void emit_identifier(const char *name, const char *member) {
    if (member) {
        if (strcmp(name, "joy") == 0 &&
            (strcmp(member, "x") == 0 || strcmp(member, "y") == 0 ||
             strcmp(member, "dx") == 0 || strcmp(member, "dy") == 0 ||
             strcmp(member, "ox") == 0 || strcmp(member, "oy") == 0 ||
             strcmp(member, "r") == 0)) {
            fprintf(C.out, "joy.%s", member);
        } else {
            fprintf(C.out, "ds_read_field(\"%s\", \"%s\")", name, member);
        }
        return;
    }

    if (is_local(name)) {
        fputs(name, C.out);
    } else if (strcmp(name, "screen_w") == 0 || strcmp(name, "screen_h") == 0 ||
               strcmp(name, "fps") == 0) {
        fputs(name, C.out);
    } else if (strcmp(name, "true") == 0) {
        fputs("1.0", C.out);
    } else if (strcmp(name, "false") == 0 || strcmp(name, "nil") == 0) {
        fputs("0.0", C.out);
    } else {
        fprintf(C.out, "ds_read(\"%s\")", name);
    }
}

static void emit_expression(const char *expression) {
    size_t i = 0;

    while (expression[i]) {
        char current = expression[i];
        if (isspace((unsigned char)current)) {
            fputc(current, C.out);
            ++i;
            continue;
        }
        if (current == '"') {
            int escaped = 0;
            fputc(expression[i++], C.out);
            while (expression[i]) {
                char character = expression[i++];
                fputc(character, C.out);
                if (escaped) {
                    escaped = 0;
                } else if (character == '\\') {
                    escaped = 1;
                } else if (character == '"') {
                    break;
                }
            }
            continue;
        }
        if (isdigit((unsigned char)current) ||
            (current == '.' && isdigit((unsigned char)expression[i + 1]))) {
            char *end;
            (void)strtod(expression + i, &end);
            if (end == expression + i) {
                fputc(current, C.out);
                ++i;
            } else {
                fwrite(expression + i, 1, (size_t)(end - (expression + i)), C.out);
                i = (size_t)(end - expression);
            }
            continue;
        }
        if (is_identifier_start(current)) {
            char name[DS_MAX_NAME];
            char member[DS_MAX_NAME];
            size_t length = 0;
            size_t start;

            start = i;
            while (is_identifier_part(expression[i])) {
                if (length + 1 < sizeof(name)) name[length++] = expression[i];
                ++i;
            }
            name[length] = '\0';

            if (strcmp(name, "and") == 0 || strcmp(name, "or") == 0 ||
                strcmp(name, "not") == 0) {
                fputs(strcmp(name, "and") == 0 ? " && " :
                      (strcmp(name, "or") == 0 ? " || " : " !"), C.out);
                continue;
            }

            if (expression[i] == '(') {
                if (!is_builtin_function(name) && function_index(name) < 0) {
                    compiler_error("unknown function '%s'", name);
                }
                if (function_index(name) >= 0) {
                    fprintf(C.out, "ds_fn_%s", name);
                } else {
                    fputs(name, C.out);
                }
                (void)start;
                continue;
            }

            if (expression[i] == '.') {
                size_t member_length = 0;
                ++i;
                while (is_identifier_part(expression[i])) {
                    if (member_length + 1 < sizeof(member)) member[member_length++] = expression[i];
                    ++i;
                }
                member[member_length] = '\0';
                if (!valid_identifier(member)) {
                    compiler_error("invalid member access after '%s'", name);
                }
                emit_identifier(name, member);
            } else {
                emit_identifier(name, NULL);
            }
            continue;
        }

        fputc(current, C.out);
        ++i;
    }
}

static int split_arguments(const char *input, char parts[][DS_MAX_LINE], int maximum) {
    int count = 0;
    int depth = 0;
    int in_string = 0;
    int escaped = 0;
    size_t start = 0;
    size_t i;

    for (i = 0; input[i] && isspace((unsigned char)input[i]); ++i) {
        /* An argument list containing only whitespace is empty. */
    }
    if (!input[i]) return 0;
    i = 0;
    for (i = 0; ; ++i) {
        char current = input[i];
        if (in_string) {
            if (escaped) escaped = 0;
            else if (current == '\\') escaped = 1;
            else if (current == '"') in_string = 0;
        } else if (current == '"') {
            in_string = 1;
        } else if (current == '(' || current == '{' || current == '[') {
            ++depth;
        } else if (current == ')' || current == '}' || current == ']') {
            if (depth > 0) --depth;
        }
        if ((current == ',' && depth == 0 && !in_string) || current == '\0') {
            size_t length = i - start;
            char temporary[DS_MAX_LINE];
            char *part;
            if (count >= maximum || length >= sizeof(temporary)) return -1;
            memcpy(temporary, input + start, length);
            temporary[length] = '\0';
            part = trim(temporary);
            snprintf(parts[count], DS_MAX_LINE, "%s", part);
            ++count;
            start = i + 1;
        }
        if (current == '\0') break;
    }
    return count;
}

static int parse_call(const char *line, char *name, size_t name_size,
                      char *arguments, size_t arguments_size) {
    const char *open = strchr(line, '(');
    const char *close = strrchr(line, ')');
    size_t name_length;
    size_t argument_length;

    if (!open || !close || close < open || close[1] != '\0') return 0;
    name_length = (size_t)(open - line);
    if (name_length == 0 || name_length >= name_size) return 0;
    memcpy(name, line, name_length);
    name[name_length] = '\0';
    trim(name);
    argument_length = (size_t)(close - open - 1);
    if (argument_length >= arguments_size) return 0;
    memcpy(arguments, open + 1, argument_length);
    arguments[argument_length] = '\0';
    return valid_identifier(name);
}

static void emit_call(const char *line) {
    char name[DS_MAX_NAME];
    char arguments[DS_MAX_LINE];
    char parts[DS_MAX_PARAMS][DS_MAX_LINE];
    int count;
    int i;

    if (!parse_call(line, name, sizeof(name), arguments, sizeof(arguments))) {
        compiler_error("invalid function call");
        return;
    }
    count = split_arguments(arguments, parts, DS_MAX_PARAMS);
    if (count < 0) {
        compiler_error("too many or too-long function arguments");
        return;
    }

    if (strcmp(name, "print") == 0) {
        if (count != 1) {
            compiler_error("print expects one argument");
            return;
        }
        emit_indent();
        if (parts[0][0] == '"') {
            fputs("prints(", C.out);
            emit_expression(parts[0]);
        } else {
            fputs("printn(", C.out);
            emit_expression(parts[0]);
        }
        fputs(");\n", C.out);
        return;
    }

    if (strcmp(name, "printn") == 0 || strcmp(name, "prints") == 0 ||
        strcmp(name, "cls") == 0 || strcmp(name, "rect") == 0 ||
        strcmp(name, "circle") == 0 || strcmp(name, "ring") == 0 ||
        strcmp(name, "tex") == 0 || strcmp(name, "text") == 0 ||
        strcmp(name, "sqrt") == 0 || strcmp(name, "sin") == 0 ||
        strcmp(name, "cos") == 0 || strcmp(name, "atan2") == 0) {
        emit_indent();
        fprintf(C.out, "%s(", name);
        for (i = 0; i < count; ++i) {
            if (i) fputs(", ", C.out);
            emit_expression(parts[i]);
        }
        fputs(");\n", C.out);
        return;
    }

    if (function_index(name) >= 0) {
        emit_indent();
        fprintf(C.out, "ds_fn_%s(", name);
        for (i = 0; i < count; ++i) {
            if (i) fputs(", ", C.out);
            emit_expression(parts[i]);
        }
        fputs(");\n", C.out);
        return;
    }

    compiler_error("unknown function '%s'", name);
}

static int find_assignment(const char *line, size_t *position, size_t *operator_length) {
    int depth = 0;
    int in_string = 0;
    int escaped = 0;
    size_t i;

    for (i = 0; line[i]; ++i) {
        char current = line[i];
        if (in_string) {
            if (escaped) escaped = 0;
            else if (current == '\\') escaped = 1;
            else if (current == '"') in_string = 0;
            continue;
        }
        if (current == '"') {
            in_string = 1;
        } else if (current == '(' || current == '{' || current == '[') {
            ++depth;
        } else if (current == ')' || current == '}' || current == ']') {
            if (depth > 0) --depth;
        } else if (depth == 0 && (current == '=' || current == '+' || current == '-' ||
                                  current == '*' || current == '/')) {
            if (current == '=' && (line[i + 1] == '=' || (i > 0 && line[i - 1] == '<') ||
                                   (i > 0 && line[i - 1] == '>') || (i > 0 && line[i - 1] == '!'))) {
                continue;
            }
            if (line[i + 1] == '=') {
                *position = i;
                *operator_length = 2;
                return 1;
            }
            if (current == '=') {
                *position = i;
                *operator_length = 1;
                return 1;
            }
        }
    }
    return 0;
}

static int parse_lvalue(const char *text, char *object, size_t object_size,
                        char *member, size_t member_size) {
    const char *dot = strchr(text, '.');
    char copy[DS_MAX_NAME * 2];
    char *clean;

    if (strlen(text) >= sizeof(copy)) return 0;
    snprintf(copy, sizeof(copy), "%s", text);
    clean = trim(copy);
    if (dot) {
        size_t object_length = (size_t)(dot - text);
        const char *member_start = dot + 1;
        if (object_length == 0 || object_length >= object_size ||
            strlen(member_start) >= member_size) return 0;
        memcpy(object, text, object_length);
        object[object_length] = '\0';
        snprintf(member, member_size, "%s", member_start);
        trim(object);
        trim(member);
        return valid_identifier(object) && valid_identifier(member);
    }
    if (strlen(clean) >= object_size) return 0;
    snprintf(object, object_size, "%s", clean);
    member[0] = '\0';
    return valid_identifier(object);
}

static void emit_read_lvalue(const char *object, const char *member) {
    if (*member) {
        fprintf(C.out, "ds_read_field(\"%s\", \"%s\")", object, member);
    } else if (is_local(object)) {
        fputs(object, C.out);
    } else {
        fprintf(C.out, "ds_read(\"%s\")", object);
    }
}

static int emit_table_literal(const char *object, const char *literal) {
    char contents[DS_MAX_LINE];
    char parts[DS_MAX_PARAMS][DS_MAX_LINE];
    size_t length;
    int count;
    int i;
    int table_number;

    length = strlen(literal);
    if (length < 2 || literal[0] != '{' || literal[length - 1] != '}') return 0;
    if (length - 2 >= sizeof(contents)) {
        compiler_error("table literal is too long");
        return 1;
    }
    memcpy(contents, literal + 1, length - 2);
    contents[length - 2] = '\0';
    count = split_arguments(contents, parts, DS_MAX_PARAMS);
    if (count < 0) {
        compiler_error("table literal has too many fields");
        return 1;
    }

    table_number = C.table_number++;
    emit_indent();
    fprintf(C.out, "{ Table *ds_table_%d = T_new();\n", table_number);
    emit_indent();
    fprintf(C.out, "    if (!ds_table_%d) return;\n", table_number);
    for (i = 0; i < count; ++i) {
        char key[DS_MAX_NAME];
        char value[DS_MAX_LINE];
        char *equals = strchr(parts[i], '=');
        if (!equals) {
            compiler_error("table field '%s' must have a name and value", parts[i]);
            continue;
        }
        *equals = '\0';
        snprintf(key, sizeof(key), "%s", trim(parts[i]));
        snprintf(value, sizeof(value), "%s", trim(equals + 1));
        if (!valid_identifier(key)) {
            compiler_error("invalid table field '%s'", key);
            continue;
        }
        emit_indent();
        fputs("    if (!T_set(ds_table_", C.out);
        fprintf(C.out, "%d, \"%s\", ", table_number, key);
        if (value[0] == '"') {
            fprintf(C.out, "&(Val){DS_STRING, {.str = %s}}, DS_STRING", value);
        } else if (value[0] == '{') {
            compiler_error("nested table literals are not supported yet");
            fprintf(C.out, "&(Val){0}, 0");
        } else {
            fputs("&(Val){DS_NUMBER, {.num = ", C.out);
            emit_expression(value);
            fputs("}}, DS_NUMBER", C.out);
        }
        fprintf(C.out, ")) { ds_runtime_error(\"could not assign field '%s'\"); return; }\n", key);
    }
    emit_indent();
    fprintf(C.out, "    if (!T_set(ds_active_scope(), \"%s\", ds_table_%d, DS_TABLE)) return;\n", object, table_number);
    emit_indent();
    fputs("}\n", C.out);
    return 1;
}

static void emit_assignment(const char *line) {
    size_t position;
    size_t operator_length;
    char left[DS_MAX_LINE];
    char right[DS_MAX_LINE];
    char object[DS_MAX_NAME];
    char member[DS_MAX_NAME];
    char *left_trimmed;
    char *right_trimmed;
    const char *operator;

    if (!find_assignment(line, &position, &operator_length)) {
        compiler_error("unsupported statement '%s'", line);
        return;
    }
    if (position >= sizeof(left) || strlen(line) - position - operator_length >= sizeof(right)) {
        compiler_error("assignment is too long");
        return;
    }
    memcpy(left, line, position);
    left[position] = '\0';
    memcpy(right, line + position + operator_length, strlen(line) - position - operator_length + 1);
    left_trimmed = trim(left);
    right_trimmed = trim(right);
    if (!parse_lvalue(left_trimmed, object, sizeof(object), member, sizeof(member))) {
        compiler_error("invalid assignment target '%s'", left_trimmed);
        return;
    }

    if (operator_length == 1) operator = "=";
    else {
        static char operators[4][3] = {"+=", "-=", "*=", "/="};
        char operation[3] = {line[position], '=', '\0'};
        operator = NULL;
        if (strcmp(operation, "+=") == 0) operator = operators[0];
        if (strcmp(operation, "-=") == 0) operator = operators[1];
        if (strcmp(operation, "*=") == 0) operator = operators[2];
        if (strcmp(operation, "/=") == 0) operator = operators[3];
        if (!operator) {
            compiler_error("unsupported assignment operator '%s'", operation);
            return;
        }
    }

    if (!*member && strcmp(operator, "=") == 0 && right_trimmed[0] == '{') {
        if (emit_table_literal(object, right_trimmed)) return;
    }

    if (*member && strcmp(object, "joy") == 0) {
        emit_indent();
        fprintf(C.out, "joy.%s %s ", member, operator);
        emit_expression(right_trimmed);
        fputs(";\n", C.out);
        return;
    }

    if (!*member && is_local(object)) {
        emit_indent();
        fprintf(C.out, "%s %s ", object, operator);
        emit_expression(right_trimmed);
        fputs(";\n", C.out);
        return;
    }

    if (strcmp(operator, "=") == 0 && right_trimmed[0] == '"') {
        emit_indent();
        if (*member) {
            fprintf(C.out, "ds_write_string_field(\"%s\", \"%s\", %s);\n",
                    object, member, right_trimmed);
        } else {
            fprintf(C.out, "ds_write_string(\"%s\", %s);\n", object, right_trimmed);
        }
        return;
    }

    emit_indent();
    if (*member) {
        fprintf(C.out, "ds_write_field(\"%s\", \"%s\", ", object, member);
    } else {
        fprintf(C.out, "ds_write(\"%s\", ", object);
    }
    if (strcmp(operator, "=") == 0) {
        emit_expression(right_trimmed);
    } else {
        emit_read_lvalue(object, member);
        fprintf(C.out, " %.*s ", (int)(operator_length - 1), operator);
        emit_expression(right_trimmed);
    }
    fputs(");\n", C.out);
}

static void compile_statement(const char *raw_line, int *block_depth) {
    char line[DS_MAX_LINE];
    char condition[DS_MAX_LINE];
    char *suffix;

    snprintf(line, sizeof(line), "%s", raw_line);
    trim(line);
    if (!*line) return;

    if (starts_keyword(line, "if")) {
        snprintf(condition, sizeof(condition), "%s", trim(line + 2));
        suffix = strstr(condition, " then");
        if (suffix) *suffix = '\0';
        if (!suffix) {
            compiler_error("if statement must end with 'then'");
            return;
        }
        emit_indent();
        fputs("if (", C.out);
        emit_expression(trim(condition));
        fputs(") {\n", C.out);
        ++*block_depth;
        return;
    }
    if (starts_keyword(line, "elseif")) {
        snprintf(condition, sizeof(condition), "%s", trim(line + strlen("elseif")));
        suffix = strstr(condition, " then");
        if (suffix) *suffix = '\0';
        if (!suffix || *block_depth <= 0) {
            compiler_error("invalid elseif statement");
            return;
        }
        emit_indent();
        fputs("} else if (", C.out);
        emit_expression(trim(condition));
        fputs(") {\n", C.out);
        return;
    }
    if (strcmp(line, "else") == 0) {
        if (*block_depth <= 0) {
            compiler_error("else without a matching if");
            return;
        }
        emit_indent();
        fputs("} else {\n", C.out);
        return;
    }
    if (starts_keyword(line, "while")) {
        snprintf(condition, sizeof(condition), "%s", trim(line + strlen("while")));
        suffix = strstr(condition, " do");
        if (suffix) *suffix = '\0';
        if (!suffix) {
            compiler_error("while statement must end with 'do'");
            return;
        }
        emit_indent();
        fputs("while (", C.out);
        emit_expression(trim(condition));
        fputs(") {\n", C.out);
        ++*block_depth;
        return;
    }
    if (starts_keyword(line, "for")) {
        char rest[DS_MAX_LINE];
        char parts[3][DS_MAX_LINE];
        char variable[DS_MAX_NAME];
        char *equals;
        int count;

        snprintf(rest, sizeof(rest), "%s", trim(line + strlen("for")));
        equals = strchr(rest, '=');
        if (!equals) {
            compiler_error("for statement must have an initializer");
            return;
        }
        *equals = '\0';
        snprintf(variable, sizeof(variable), "%s", trim(rest));
        if (!valid_identifier(variable)) {
            compiler_error("invalid for variable '%s'", variable);
            return;
        }
        count = split_arguments(trim(equals + 1), parts, 3);
        if (count < 2 || count > 3) {
            compiler_error("for expects start, end, and optional step");
            return;
        }
        add_local(variable);
        emit_indent();
        fprintf(C.out, "for (double %s = ", variable);
        emit_expression(parts[0]);
        fprintf(C.out, "; %s <= ", variable);
        emit_expression(parts[1]);
        fprintf(C.out, "; %s += ", variable);
        if (count == 3) emit_expression(parts[2]);
        else fputs("1.0", C.out);
        fputs(") {\n", C.out);
        ++*block_depth;
        return;
    }
    if (strcmp(line, "end") == 0) {
        if (*block_depth <= 0) {
            compiler_error("unexpected end");
            return;
        }
        --*block_depth;
        emit_indent();
        fputs("}\n", C.out);
        return;
    }
    if (starts_keyword(line, "local")) {
        char declaration[DS_MAX_LINE];
        size_t position;
        size_t operator_length;
        char name[DS_MAX_NAME];

        snprintf(declaration, sizeof(declaration), "%s", trim(line + strlen("local")));
        if (!find_assignment(declaration, &position, &operator_length) || operator_length != 1) {
            compiler_error("local declaration must initialise a variable");
            return;
        }
        declaration[position] = '\0';
        snprintf(name, sizeof(name), "%s", trim(declaration));
        if (!valid_identifier(name) || !add_local(name)) {
            compiler_error("invalid local variable '%s'", name);
            return;
        }
        {
            const char *value = trim(declaration + position + 1);
            emit_indent();
            fprintf(C.out, "double %s = ", name);
            emit_expression(value);
            fputs(";\n", C.out);
        }
        return;
    }
    if (starts_keyword(line, "return")) {
        if (*trim(line + strlen("return"))) {
            compiler_error("return values are not supported by void hooks yet");
            return;
        }
        emit_indent();
        fputs("return;\n", C.out);
        return;
    }
    if (strcmp(line, "break") == 0 || strcmp(line, "continue") == 0) {
        if (*block_depth <= 0) {
            compiler_error("%s is outside a loop", line);
            return;
        }
        emit_indent();
        fprintf(C.out, "%s;\n", line);
        return;
    }
    if (strchr(line, '(') && line[strlen(line) - 1] == ')') {
        emit_call(line);
        return;
    }

    emit_assignment(line);
}

/* A semicolon is a statement separator.  It keeps .ds files compact without
 * changing the block syntax: function/if/else/while/end still stay on their
 * own lines, while assignments and calls can share a line. */
static void compile_statement_list(const char *raw_line, int *block_depth) {
    char statement[DS_MAX_LINE];
    int in_string = 0;
    int escaped = 0;
    int nesting = 0;
    size_t start = 0;
    size_t i;

    for (i = 0; ; ++i) {
        char current = raw_line[i];
        if (in_string) {
            if (escaped) escaped = 0;
            else if (current == '\\') escaped = 1;
            else if (current == '"') in_string = 0;
        } else if (current == '"') {
            in_string = 1;
        } else if (current == '(' || current == '{' || current == '[') {
            ++nesting;
        } else if (current == ')' || current == '}' || current == ']') {
            if (nesting > 0) --nesting;
        }

        if ((current == ';' && !in_string && nesting == 0) || current == '\0') {
            size_t length = i - start;
            if (length >= sizeof(statement)) {
                compiler_error("statement is too long");
                return;
            }
            memcpy(statement, raw_line + start, length);
            statement[length] = '\0';
            compile_statement(statement, block_depth);
            start = i + 1;
        }
        if (current == '\0') break;
    }
}

static void emit_runtime_helpers(void) {
    fprintf(C.out,
        "#if defined(__GNUC__)\n"
        "#define DS_UNUSED __attribute__((unused))\n"
        "#else\n"
        "#define DS_UNUSED\n"
        "#endif\n\n"
        "static Table *ds_active_scope(void) { return L ? L : G; }\n"
        "static double ds_read(const char *name) {\n"
        "    Val *value;\n"
        "    if (strcmp(name, \"screen_w\") == 0) return (double)screen_w;\n"
        "    if (strcmp(name, \"screen_h\") == 0) return (double)screen_h;\n"
        "    if (strcmp(name, \"fps\") == 0) return fps;\n"
        "    value = T_get(ds_active_scope(), name, NULL);\n"
        "    if (!value || value->type != DS_NUMBER) {\n"
        "        ds_runtime_error(\"'%%s' is not a number\", name);\n"
        "        return 0.0;\n"
        "    }\n"
        "    return value->num;\n"
        "}\n"
        "static double ds_read_field(const char *object, const char *field) {\n"
        "    Val *object_value = T_get(ds_active_scope(), object, NULL);\n"
        "    Val *value;\n"
        "    if (!object_value || object_value->type != DS_TABLE || !object_value->table) {\n"
        "        ds_runtime_error(\"'%%s' is not a table\", object);\n"
        "        return 0.0;\n"
        "    }\n"
        "    value = T_get(object_value->table, field, NULL);\n"
        "    if (!value || value->type != DS_NUMBER) {\n"
        "        ds_runtime_error(\"field '%%s.%%s' is not a number\", object, field);\n"
        "        return 0.0;\n"
        "    }\n"
        "    return value->num;\n"
        "}\n"
        "static void ds_write(const char *name, double number) {\n"
        "    if (!T_set(ds_active_scope(), name, &(Val){DS_NUMBER, {.num = number}}, DS_NUMBER))\n"
        "        ds_runtime_error(\"cannot assign '%%s'\", name);\n"
        "}\n"
        "static DS_UNUSED void ds_write_string(const char *name, const char *string) {\n"
        "    if (!T_set(ds_active_scope(), name, &(Val){DS_STRING, {.str = (char *)string}}, DS_STRING))\n"
        "        ds_runtime_error(\"cannot assign '%%s'\", name);\n"
        "}\n"
        "static void ds_write_field(const char *object, const char *field, double number) {\n"
        "    Val *object_value = T_get(ds_active_scope(), object, NULL);\n"
        "    if (!object_value || object_value->type != DS_TABLE || !object_value->table) {\n"
        "        ds_runtime_error(\"'%%s' is not a table\", object);\n"
        "        return;\n"
        "    }\n"
        "    if (!T_set(object_value->table, field, &(Val){DS_NUMBER, {.num = number}}, DS_NUMBER))\n"
        "        ds_runtime_error(\"cannot assign '%%s.%%s'\", object, field);\n"
        "}\n"
        "static DS_UNUSED void ds_write_string_field(const char *object, const char *field, const char *string) {\n"
        "    Val *object_value = T_get(ds_active_scope(), object, NULL);\n"
        "    if (!object_value || object_value->type != DS_TABLE || !object_value->table) {\n"
        "        ds_runtime_error(\"'%%s' is not a table\", object);\n"
        "        return;\n"
        "    }\n"
        "    if (!T_set(object_value->table, field, &(Val){DS_STRING, {.str = (char *)string}}, DS_STRING))\n"
        "        ds_runtime_error(\"cannot assign '%%s.%%s'\", object, field);\n"
        "}\n\n");
}

static int line_is_inside_function(int line_number) {
    int i;
    for (i = 0; i < C.function_count; ++i) {
        int header = C.functions[i].body_start - 1;
        if (line_number >= header && line_number <= C.functions[i].body_end) return 1;
    }
    return 0;
}

static void emit_function_prototypes(void) {
    int i;
    int parameter;

    for (i = 0; i < C.function_count; ++i) {
        const Function *function = &C.functions[i];
        fprintf(C.out, "static void ds_fn_%s(", function->name);
        for (parameter = 0; parameter < function->param_count; ++parameter) {
            if (parameter) fputs(", ", C.out);
            fprintf(C.out, "double %s", function->params[parameter]);
        }
        fputs(");\n", C.out);
    }
    if (C.function_count > 0) fputc('\n', C.out);
}

static void emit_function(const Function *function) {
    int i;
    int block_depth = 0;

    fprintf(C.out, "static void ds_fn_%s(", function->name);
    for (i = 0; i < function->param_count; ++i) {
        if (i) fputs(", ", C.out);
        fprintf(C.out, "double %s", function->params[i]);
    }
    fputs(") {\n", C.out);

    C.local_count = 0;
    for (i = 0; i < function->param_count; ++i) add_local(function->params[i]);
    for (i = function->body_start; i < function->body_end; ++i) {
        C.source_name = C.source->lines[i].source_name;
        C.line_number = C.source->lines[i].source_line;
        compile_statement_list(C.source->lines[i].text, &block_depth);
    }
    if (block_depth != 0) {
        C.source_name = C.source->lines[function->body_end].source_name;
        C.line_number = C.source->lines[function->body_end].source_line;
        compiler_error("unclosed control block in function '%s'", function->name);
    }
    fputs("}\n\n", C.out);
}

static const Function *find_function(const char *name) {
    int index = function_index(name);
    return index >= 0 ? &C.functions[index] : NULL;
}

static void emit_hooks(void) {
    const Function *init_function = find_function("init");
    const Function *update_function = find_function("update");
    const Function *draw_function = find_function("draw");
    const Function *touch_function = find_function("touch");
    int i;
    int block_depth = 0;

    fprintf(C.out, "void init(AAssetManager *assets) {\n");
    fputs("    (void)assets;\n    T_free(G);\n    G = T_new();\n    L = NULL;\n    if (!G) return;\n", C.out);
    C.local_count = 0;
    for (i = 0; i < C.source->count; ++i) {
        if (line_is_inside_function(i)) continue;
        C.source_name = C.source->lines[i].source_name;
        C.line_number = C.source->lines[i].source_line;
        compile_statement_list(C.source->lines[i].text, &block_depth);
    }
    if (block_depth != 0) compiler_error("unclosed top-level control block");
    if (init_function) {
        if (init_function->param_count != 0) compiler_error("init() cannot have parameters");
        else fputs("    ds_fn_init();\n", C.out);
    }
    fputs("}\n\nvoid update(void) {\n", C.out);
    if (update_function) {
        if (update_function->param_count != 0) compiler_error("update() cannot have parameters");
        else fputs("    ds_fn_update();\n", C.out);
    }
    fputs("}\n\nvoid draw(Buffer *buffer) {\n    (void)buffer;\n", C.out);
    if (draw_function) {
        if (draw_function->param_count != 0) compiler_error("draw() cannot have parameters");
        else fputs("    ds_fn_draw();\n", C.out);
    }
    fputs("}\n\nvoid touch(float x, float y, int action) {\n    (void)x;\n    (void)y;\n    (void)action;\n", C.out);
    if (touch_function) {
        if (touch_function->param_count == 0) {
            fputs("    ds_fn_touch();\n", C.out);
        } else if (touch_function->param_count == 3) {
            fputs("    ds_fn_touch((double)x, (double)y, (double)action);\n", C.out);
        } else {
            compiler_error("touch() must have zero or three parameters");
        }
    }
    fputs("}\n", C.out);
}

static int compile_sources(const char *destination_path, int source_count,
                           const char *const source_paths[]) {
    Source source = {0};
    FILE *output;
    int i;

    if (source_count <= 0) {
        fprintf(stderr, "Error: no DimScript source files were provided\n");
        return 0;
    }

    memset(&C, 0, sizeof(C));
    C.source_name = source_paths[0];
    C.source = &source;
    for (i = 0; i < source_count; ++i) {
        if (!load_source(source_paths[i], &source)) {
            free_source(&source);
            return 0;
        }
    }
    if (!collect_functions()) {
        free_source(&source);
        return 0;
    }

    output = fopen(destination_path, "w");
    if (!output) {
        fprintf(stderr, "Error: cannot create %s: %s\n", destination_path, strerror(errno));
        free_source(&source);
        return 0;
    }
    C.out = output;

    fputs("#include \"runtime.h\"\n#include <math.h>\n#include <string.h>\n\n", output);
    emit_runtime_helpers();
    emit_function_prototypes();
    for (i = 0; i < C.function_count; ++i) emit_function(&C.functions[i]);
    emit_hooks();
    fclose(output);
    C.out = NULL;

    if (C.errors != 0) {
        remove(destination_path);
        free_source(&source);
        return 0;
    }
    free_source(&source);
    return 1;
}

int compile_ds(const char *source_path, const char *destination_path) {
    const char *source_paths[1] = {source_path};
    return compile_sources(destination_path, 1, source_paths);
}

static void print_usage(const char *program) {
    fprintf(stderr,
            "Usage: %s input.ds output.c\n"
            "       %s --output output.c file1.ds [file2.ds ...]\n",
            program, program);
}

int main(int argc, char **argv) {
    const char *destination;
    int first_source;
    int source_count;
    const char **source_paths;
    int i;
    int result;

    if (argc == 3) {
        return compile_ds(argv[1], argv[2]) ? 0 : 1;
    }
    if (argc < 4) {
        print_usage(argv[0]);
        return 2;
    }

    if (strcmp(argv[1], "--output") == 0) {
        destination = argv[2];
        first_source = 3;
    } else {
        destination = argv[1];
        first_source = 2;
    }
    source_count = argc - first_source;
    if (source_count <= 0) {
        print_usage(argv[0]);
        return 2;
    }

    source_paths = (const char **)calloc((size_t)source_count, sizeof(*source_paths));
    if (!source_paths) {
        fprintf(stderr, "Error: out of memory while preparing source files\n");
        return 1;
    }
    for (i = 0; i < source_count; ++i) {
        source_paths[i] = argv[first_source + i];
    }

    result = compile_sources(destination, source_count, source_paths) ? 0 : 1;
    free(source_paths);
    return result;
}
