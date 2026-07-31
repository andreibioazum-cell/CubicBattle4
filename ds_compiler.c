#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

typedef struct {
    FILE* out;
    int indent;
    int temp_count;
    int in_fn;
    char fn_name[64];
    int has_return;
} Compiler;

// === Глобальные переменные для компилятора ===
static Compiler C = {0};
static int line_num = 0;

// === Стриппинг пробелов (минимальный код) ===
static void strip_spaces(char* s) {
    char* dst = s;
    int in_string = 0;
    while (*s) {
        if (*s == '"') in_string = !in_string;
        if (!in_string && (*s == ' ' || *s == '\t')) { s++; continue; }
        *dst++ = *s++;
    }
    *dst = 0;
}

// === Компиляция выражений (максимально оптимизировано) ===
static void compile_expr(const char* expr) {
    char e[1024];
    strcpy(e, expr);
    strip_spaces(e);
    
    // === Векторы: V2(x,y) ===
    if (strncmp(e, "V2(", 3) == 0) {
        char* p = e + 3;
        char* comma = strchr(p, ',');
        if (comma) {
            *comma = 0;
            char* y = comma + 1;
            char* end = strchr(y, ')');
            if (end) *end = 0;
            fprintf(C.out, "V2(%s,%s)", p, y);
            return;
        }
    }
    
    // === Векторы: V3(x,y,z) ===
    if (strncmp(e, "V3(", 3) == 0) {
        char* p = e + 3;
        char* c1 = strchr(p, ',');
        if (c1) {
            *c1 = 0;
            char* c2 = strchr(c1+1, ',');
            if (c2) {
                *c2 = 0;
                char* z = c2 + 1;
                char* end = strchr(z, ')');
                if (end) *end = 0;
                fprintf(C.out, "V3(%s,%s,%s)", p, c1+1, z);
                return;
            }
        }
    }
    
    // === Числа ===
    char* end;
    double n = strtod(e, &end);
    if (*end == 0) { fprintf(C.out, "%f", n); return; }
    
    // === Строки ===
    if (e[0] == '"') { fprintf(C.out, "%s", e); return; }
    
    // === Переменные (оптимизированный доступ) ===
    // Проверяем локальные переменные
    fprintf(C.out, "(*(Val*)T_get(L?L:G,\"%s\",NULL))", e);
}

// === Компиляция условий ===
static void compile_cond(const char* cond) {
    char c[1024];
    strcpy(c, cond);
    strip_spaces(c);
    
    // Операторы сравнения
    char* op = NULL;
    char* p = c;
    while (*p) {
        if (*p == '=' && *(p+1) == '=') { op = "=="; break; }
        if (*p == '!' && *(p+1) == '=') { op = "!="; break; }
        if (*p == '<' && *(p+1) == '=') { op = "<="; break; }
        if (*p == '>' && *(p+1) == '=') { op = ">="; break; }
        if (*p == '<') { op = "<"; break; }
        if (*p == '>') { op = ">"; break; }
        p++;
    }
    
    if (op) {
        *p = 0;
        char* left = c;
        char* right = p + strlen(op);
        fprintf(C.out, "((Val*)T_get(L?L:G,\"%s\",NULL))->num %s ((Val*)T_get(L?L:G,\"%s\",NULL))->num", 
                left, op, right);
        return;
    }
    
    // Простое условие (переменная)
    fprintf(C.out, "((Val*)T_get(L?L:G,\"%s\",NULL))->num != 0", c);
}

// === Компиляция вызова функции ===
static void compile_call(const char* call) {
    char c[1024];
    strcpy(c, call);
    strip_spaces(c);
    
    char* paren = strchr(c, '(');
    if (!paren) { fprintf(C.out, "%s", c); return; }
    
    *paren = 0;
    char* name = c;
    char* args = paren + 1;
    char* end = strchr(args, ')');
    if (end) *end = 0;
    
    // Встроенные функции
    if (strcmp(name, "print") == 0) {
        Val v;
        if (args[0] == '"') {
            fprintf(C.out, "prints(%s)", args);
        } else {
            fprintf(C.out, "printn(%s)", args);
        }
        return;
    }
    
    if (strcmp(name, "tostring") == 0) {
        fprintf(C.out, "tostring(*(Val*)T_get(L?L:G,\"%s\",NULL))", args);
        return;
    }
    
    if (strcmp(name, "tonumber") == 0) {
        fprintf(C.out, "tonumber(*(Val*)T_get(L?L:G,\"%s\",NULL))", args);
        return;
    }
    
    // Обычный вызов
    fprintf(C.out, "%s(", name);
    if (*args) {
        char* arg = args;
        int first = 1;
        while (*arg) {
            if (*arg == ',') { arg++; continue; }
            if (!first) fprintf(C.out, ",");
            first = 0;
            // Определяем тип аргумента
            if (*arg == '"') {
                fprintf(C.out, "%s", arg);
                arg = strchr(arg, '"') + 1;
            } else {
                char* end2 = arg;
                while (*end2 && *end2 != ',') end2++;
                char save = *end2;
                *end2 = 0;
                compile_expr(arg);
                *end2 = save;
                arg = end2;
            }
        }
    }
    fprintf(C.out, ")");
}

// === Компиляция строки .ds ===
static void compile_line(char* line) {
    // Убираем пробелы в начале/конце
    while (*line == ' ' || *line == '\t') line++;
    if (*line == '\n' || *line == '\r' || *line == 0) {
        fprintf(C.out, "\n");
        return;
    }
    
    // Комментарии
    if (strncmp(line, "--", 2) == 0 || strncmp(line, "//", 2) == 0) {
        fprintf(C.out, "%s\n", line);
        return;
    }
    
    // === function ===
    if (strncmp(line, "function", 8) == 0) {
        char* name = line + 8;
        while (*name == ' ') name++;
        char* paren = strchr(name, '(');
        if (paren) {
            int len = paren - name;
            strncpy(C.fn_name, name, len);
            C.fn_name[len] = 0;
        }
        C.in_fn = 1;
        C.has_return = 0;
        fprintf(C.out, "void %s(", C.fn_name);
        
        // Параметры
        if (paren) {
            char* p = paren + 1;
            char* end = strchr(p, ')');
            if (end) *end = 0;
            int first = 1;
            if (*p) {
                char* arg = p;
                while (*arg) {
                    if (*arg == ',') { arg++; continue; }
                    if (!first) fprintf(C.out, ",");
                    first = 0;
                    char* end2 = arg;
                    while (*end2 && *end2 != ',' && *end2 != ' ') end2++;
                    char save = *end2;
                    *end2 = 0;
                    fprintf(C.out, "Val %s", arg);
                    *end2 = save;
                    arg = end2;
                }
            }
        }
        fprintf(C.out, "){");
        return;
    }
    
    // === end ===
    if (strcmp(line, "end") == 0 || strcmp(line, "end\n") == 0) {
        if (C.in_fn && !C.has_return) {
            fprintf(C.out, "return (Val){0};");
        }
        fprintf(C.out, "}");
        C.in_fn = 0;
        return;
    }
    
    // === return / ret ===
    if (strncmp(line, "return", 6) == 0 || strncmp(line, "ret", 3) == 0) {
        char* val = line + (line[0]=='r' && line[1]=='e' && line[2]=='t' ? 3 : 6);
        while (*val == ' ') val++;
        C.has_return = 1;
        if (*val) {
            fprintf(C.out, "return ");
            compile_expr(val);
            fprintf(C.out, ";");
        } else {
            fprintf(C.out, "return (Val){0};");
        }
        return;
    }
    
    // === if ===
    if (strncmp(line, "if ", 3) == 0) {
        char* cond = line + 3;
        char* then_pos = strstr(cond, " then");
        if (then_pos) *then_pos = 0;
        fprintf(C.out, "if(");
        compile_cond(cond);
        fprintf(C.out, "){");
        return;
    }
    
    // === elseif ===
    if (strncmp(line, "elseif", 6) == 0) {
        char* cond = line + 6;
        while (*cond == ' ') cond++;
        char* then_pos = strstr(cond, " then");
        if (then_pos) *then_pos = 0;
        fprintf(C.out, "}else if(");
        compile_cond(cond);
        fprintf(C.out, "){");
        return;
    }
    
    // === else ===
    if (strcmp(line, "else") == 0) {
        fprintf(C.out, "}else{");
        return;
    }
    
    // === while ===
    if (strncmp(line, "while ", 6) == 0) {
        char* cond = line + 6;
        char* do_pos = strstr(cond, " do");
        if (do_pos) *do_pos = 0;
        fprintf(C.out, "while(");
        compile_cond(cond);
        fprintf(C.out, "){");
        return;
    }
    
    // === for (оптимизированный) ===
    if (strncmp(line, "for ", 4) == 0) {
        char* p = line + 4;
        char* eq = strchr(p, '=');
        if (eq) {
            *eq = 0;
            char* var = p;
            while (*var == ' ') var++;
            char* rest = eq + 1;
            char* comma = strchr(rest, ',');
            if (comma) {
                *comma = 0;
                char* start = rest;
                char* end = comma + 1;
                char* step = strchr(end, ',');
                if (step) {
                    *step = 0;
                    char* step_val = step + 1;
                    while (*step_val == ' ') step_val++;
                    fprintf(C.out, "for(double %s=%s; %s<=%s; %s+=%s){", var, start, var, end, var, step_val);
                } else {
                    fprintf(C.out, "for(double %s=%s; %s<=%s; %s+=1){", var, start, var, end, var);
                }
            }
        }
        return;
    }
    
    // === local (объявление локальной переменной) ===
    if (strncmp(line, "local ", 6) == 0) {
        char* var = line + 6;
        while (*var == ' ') var++;
        char* eq = strchr(var, '=');
        if (eq) {
            *eq = 0;
            char* name = var;
            while (*name == ' ') name++;
            char* val = eq + 1;
            while (*val == ' ') val++;
            if (!L) fprintf(C.out, "L=T_new();");
            fprintf(C.out, "T_set(L,\"%s\",", name);
            // Создаём Val
            fprintf(C.out, "&(Val){");
            // Определяем тип
            if (*val == '"') {
                fprintf(C.out, "2,.str=%s", val);
            } else if (strncmp(val, "V2(", 3) == 0) {
                fprintf(C.out, "5,.v2=");
                compile_expr(val);
            } else if (strncmp(val, "V3(", 3) == 0) {
                fprintf(C.out, "6,.v3=");
                compile_expr(val);
            } else {
                fprintf(C.out, "1,.num=");
                compile_expr(val);
            }
            fprintf(C.out, "},2);");
        }
        return;
    }
    
    // === Присваивание (оптимизированное) ===
    char* eq = strchr(line, '=');
    if (eq && *(eq-1) != '=' && *(eq-1) != '>' && *(eq-1) != ':') {
        *eq = 0;
        char* var = line;
        while (*var == ' ') var++;
        char* val = eq + 1;
        while (*val == ' ') val++;
        
        // Проверяем, есть ли в локальных
        fprintf(C.out, "T_set(L?L:G,\"%s\",", var);
        // Создаём Val
        fprintf(C.out, "&(Val){");
        if (*val == '"') {
            fprintf(C.out, "2,.str=%s", val);
        } else if (strncmp(val, "V2(", 3) == 0) {
            fprintf(C.out, "5,.v2=");
            compile_expr(val);
        } else if (strncmp(val, "V3(", 3) == 0) {
            fprintf(C.out, "6,.v3=");
            compile_expr(val);
        } else {
            fprintf(C.out, "1,.num=");
            compile_expr(val);
        }
        fprintf(C.out, "},1);");
        return;
    }
    
    // === Вызов функции ===
    if (strchr(line, '(') && strchr(line, ')')) {
        fprintf(C.out, "");
        compile_call(line);
        fprintf(C.out, ";");
        return;
    }
    
    // === Остальное ===
    fprintf(C.out, "%s", line);
}

// === Основной компилятор ===
void compile_ds(const char* src, const char* dst) {
    FILE* in = fopen(src, "r");
    if (!in) { fprintf(stderr, "Error: Cannot open %s\n", src); return; }
    
    C.out = fopen(dst, "w");
    if (!C.out) { fprintf(stderr, "Error: Cannot create %s\n", dst); fclose(in); return; }
    
    // Заголовок
    fprintf(C.out, "#include \"runtime.h\"\n");
    fprintf(C.out, "Table* L=NULL;\n");
    fprintf(C.out, "void init(AAssetManager* assets){G=T_new();");
    
    char line[4096];
    int in_multiline = 0;
    char multiline[4096] = {0};
    
    while (fgets(line, sizeof(line), in)) {
        line_num++;
        
        // Убираем \n в конце
        char* nl = strchr(line, '\n');
        if (nl) *nl = 0;
        char* cr = strchr(line, '\r');
        if (cr) *cr = 0;
        
        // Многострочные строки (упрощённо)
        if (in_multiline) {
            strcat(multiline, line);
            if (strstr(line, "]]")) {
                in_multiline = 0;
                compile_line(multiline);
                multiline[0] = 0;
            }
            continue;
        }
        if (strstr(line, "[[")) {
            in_multiline = 1;
            strcpy(multiline, line);
            continue;
        }
        
        compile_line(line);
        fprintf(C.out, "\n");
    }
    
    // Закрываем init
    fprintf(C.out, "}");
    fprintf(C.out, "void update(){");
    fprintf(C.out, "}");
    fprintf(C.out, "void draw(Buffer* rb){");
    fprintf(C.out, "}");
    fprintf(C.out, "void touch(float x,float y,int action){");
    fprintf(C.out, "}");
    
    fclose(in);
    fclose(C.out);
}

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "Usage: %s input.ds output.c\n", argv[0]);
        return 1;
    }
    compile_ds(argv[1], argv[2]);
    return 0;
}
