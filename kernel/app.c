#include <stdint.h>
#include "app.h"
#include "console.h"
#include "keyboard.h"
#include "fat.h"
#include "heap.h"
#include "util.h"
#include "io.h"
#include "xhci.h"
#include "hda.h"

#define APP_MAX_SIZE (512u * 1024u)
#define APP_MAX_VARS 256
#define APP_ARRAY_SIZE 4096
#define APP_MAX_STEPS 100000000u
#define APP_MAX_SLEEP_MS 60000u
#define APP_MAX_LINE 2048u
#define APP_NAME_SIZE 48u
#define APP_CALL_DEPTH 64u
#define APP_MAX_LABELS 1024u
#define APP_STRING_SIZE 128u

typedef enum { APP_VALUE_INT, APP_VALUE_FLOAT, APP_VALUE_STRING } app_value_type;
typedef struct {
    char name[APP_NAME_SIZE];
    app_value_type type;
    int32_t integer;
    float real;
    char string[APP_STRING_SIZE];
} app_var;
typedef struct { char name[APP_NAME_SIZE]; uint32_t pc; } app_label;
typedef struct {
    char *source;
    uint32_t size;
    app_var vars[APP_MAX_VARS];
    int32_t array[APP_ARRAY_SIZE];
    int var_count;
    uint32_t steps;
    uint32_t random_state;
    uint32_t call_stack[APP_CALL_DEPTH];
    uint32_t call_depth;
    app_label labels[APP_MAX_LABELS];
    uint32_t label_count;
} app_vm;

static const uint32_t APP_COLORS[16] = {
    0x000000, 0xAA0000, 0x00AA00, 0xAA5500,
    0x0000AA, 0xAA00AA, 0x00AAAA, 0xAAAAAA,
    0x555555, 0xFF5555, 0x55FF55, 0xFFFF55,
    0x5555FF, 0xFF55FF, 0x55FFFF, 0xFFFFFF
};

static int eq(const char *a, const char *b) {
    while (*a && *b && *a == *b) { a++; b++; }
    return *a == 0 && *b == 0;
}

static char *skip_space(char *p) {
    while (*p == ' ' || *p == '\t') p++;
    return p;
}

static void trim_right(char *p) {
    uint32_t n = (uint32_t)kstrlen(p);
    while (n && (p[n - 1] == ' ' || p[n - 1] == '\t' || p[n - 1] == '\r')) p[--n] = 0;
}

static void strip_inline_comment(char *p) {
    int quoted = 0, escaped = 0;
    while (*p) {
        if (escaped) escaped = 0;
        else if (*p == '\\' && quoted) escaped = 1;
        else if (*p == '"') quoted = !quoted;
        else if (*p == '#' && !quoted) { *p = 0; return; }
        p++;
    }
}

/* Tokens may be quoted, which makes string values with spaces practical. */
static char *token(char **cursor) {
    char *p = skip_space(*cursor);
    if (!*p) { *cursor = p; return p; }
    if (*p == '"') {
        char *start = ++p, *write = p;
        while (*p && *p != '"') {
            if (*p == '\\' && p[1]) {
                p++;
                if (*p == 'n') *write++ = '\n';
                else if (*p == 't') *write++ = '\t';
                else *write++ = *p;
                p++;
            } else *write++ = *p++;
        }
        if (*p == '"') p++;
        *write = 0;
        *cursor = p;
        return start;
    }
    char *start = p;
    while (*p && *p != ' ' && *p != '\t') p++;
    if (*p) *p++ = 0;
    *cursor = p;
    return start;
}

static int valid_word(const char *s) {
    int n = 0;
    if (!s || !*s) return 0;
    while (*s) {
        char c = *s++;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_')) return 0;
        if (++n >= (int)APP_NAME_SIZE) return 0;
    }
    return 1;
}

static app_var *find_var(app_vm *vm, const char *name, int create) {
    for (int i = 0; i < vm->var_count; i++)
        if (eq(vm->vars[i].name, name)) return &vm->vars[i];
    if (!create || !valid_word(name) || vm->var_count >= APP_MAX_VARS) return 0;
    app_var *v = &vm->vars[vm->var_count++];
    uint32_t i = 0;
    while (name[i] && i + 1 < sizeof v->name) { v->name[i] = name[i]; i++; }
    v->name[i] = 0;
    v->type = APP_VALUE_INT;
    v->integer = 0;
    v->real = 0.0f;
    v->string[0] = 0;
    return v;
}

static int parse_number(const char *s, int32_t *out) {
    int neg = 0, any = 0;
    int32_t value = 0;
    if (*s == '-') { neg = 1; s++; }
    else if (*s == '+') s++;
    while (*s >= '0' && *s <= '9') {
        value = value * 10 + (*s++ - '0');
        any = 1;
    }
    if (!any || *s) return 0;
    *out = neg ? -value : value;
    return 1;
}

static int value_of(app_vm *vm, const char *word, int32_t *out) {
    if (parse_number(word, out)) return 1;
    app_var *v = find_var(vm, word, 0);
    if (!v) return 0;
    if (v->type != APP_VALUE_INT) return 0;
    *out = v->integer;
    return 1;
}

static int parse_float(const char *s, float *out) {
    int neg = 0, any = 0, exponent = 0, exponent_neg = 0;
    float value = 0.0f, scale = 1.0f;
    if (*s == '-') { neg = 1; s++; }
    else if (*s == '+') s++;
    while (*s >= '0' && *s <= '9') {
        value = value * 10.0f + (float)(*s++ - '0'); any = 1;
    }
    if (*s == '.') {
        s++;
        while (*s >= '0' && *s <= '9') {
            scale *= 0.1f; value += (float)(*s++ - '0') * scale; any = 1;
        }
    }
    if (!any) return 0;
    if (*s == 'e' || *s == 'E') {
        int exponent_any = 0; s++;
        if (*s == '-') { exponent_neg = 1; s++; }
        else if (*s == '+') s++;
        while (*s >= '0' && *s <= '9') {
            if (exponent < 38) exponent = exponent * 10 + (*s - '0');
            s++; exponent_any = 1;
        }
        if (!exponent_any) return 0;
    }
    if (*s) return 0;
    while (exponent-- > 0) value = exponent_neg ? value * 0.1f : value * 10.0f;
    *out = neg ? -value : value;
    return 1;
}

static int float_of(app_vm *vm, const char *word, float *out) {
    if (parse_float(word, out)) return 1;
    app_var *v = find_var(vm, word, 0);
    if (!v) return 0;
    if (v->type == APP_VALUE_FLOAT) { *out = v->real; return 1; }
    if (v->type == APP_VALUE_INT) { *out = (float)v->integer; return 1; }
    return 0;
}

/* An existing string variable wins; otherwise the token itself is a literal. */
static const char *string_of(app_vm *vm, const char *word) {
    app_var *v = find_var(vm, word, 0);
    return v && v->type == APP_VALUE_STRING ? v->string : word;
}

static int string_copy(char *dst, const char *src, uint32_t capacity) {
    uint32_t n = 0;
    while (src[n] && n + 1u < capacity) { dst[n] = src[n]; n++; }
    dst[n] = 0;
    return src[n] == 0;
}

static int string_compare(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

static void set_integer(app_var *v, int32_t value) {
    v->type = APP_VALUE_INT;
    v->integer = value;
}

static void set_real(app_var *v, float value) {
    v->type = APP_VALUE_FLOAT;
    v->real = value;
}

static void put_i32(int32_t value) {
    char buf[16];
    uint32_t magnitude;
    if (value < 0) {
        console_putc('-');
        magnitude = (uint32_t)(-(value + 1)) + 1u;
    } else magnitude = (uint32_t)value;
    kutoa(magnitude, buf);
    console_puts(buf);
}

static void put_f32(float value) {
    int exponent = 0;
    if (value != value) { console_puts("nan"); return; }
    if (value < 0.0f) { console_putc('-'); value = -value; }
    if (value == 0.0f) { console_putc('0'); return; }
    while (value >= 10000000.0f && exponent < 38) { value *= 0.1f; exponent++; }
    if (value >= 10000000.0f) { console_puts("overflow"); return; }
    while (value < 0.001f && exponent > -38) { value *= 10.0f; exponent--; }
    uint32_t whole = (uint32_t)value;
    uint32_t fraction = (uint32_t)((value - (float)whole) * 1000000.0f + 0.5f);
    if (fraction >= 1000000u) { whole++; fraction = 0; }
    char buf[16]; kutoa(whole, buf); console_puts(buf);
    if (fraction) {
        char digits[7]; digits[6] = 0;
        for (int i = 5; i >= 0; i--) { digits[i] = (char)('0' + fraction % 10u); fraction /= 10u; }
        int end = 6; while (end && digits[end - 1] == '0') digits[--end] = 0;
        console_putc('.'); console_puts(digits);
    }
    if (exponent) {
        console_putc('e');
        if (exponent < 0) { console_putc('-'); exponent = -exponent; }
        kutoa((uint32_t)exponent, buf); console_puts(buf);
    }
}

static void put_text(const char *s) {
    while (*s) {
        if (s[0] == '\\' && s[1] == 'n') { console_putc('\n'); s += 2; }
        else if (s[0] == '\\' && s[1] == 't') { console_putc('\t'); s += 2; }
        else if (s[0] == '\\' && s[1] == 's') { console_putc(' '); s += 2; }
        else if (s[0] == '\\' && s[1] == '\\') { console_putc('\\'); s += 2; }
        else console_putc(*s++);
    }
}

static void read_line(char *buf, int max) {
    int n = 0;
    buf[0] = 0;
    for (;;) {
        char c = kbd_getchar();
        if (c == '\n') { console_putc('\n'); break; }
        if (c == '\b') {
            if (n) { n--; buf[n] = 0; console_puts("\b \b"); }
            continue;
        }
        if ((unsigned char)c < 32 || (unsigned char)c > 126 || n >= max - 1) continue;
        buf[n++] = c; buf[n] = 0; console_putc(c);
    }
}

static uint32_t random_next(app_vm *vm) {
    uint32_t x = vm->random_state;
    if (!x) x = 0xA341316Cu;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    vm->random_state = x;
    return x;
}

static void app_sleep(uint32_t ms) {
    uint64_t until = rdtsc() + kbd_tsc_per_ms() * (uint64_t)ms;
    while ((int64_t)(rdtsc() - until) < 0) {
        xhci_idle_drain();
        hda_bg_poll();
        __asm__ volatile ("pause");
    }
}

static int first_line_ok(const char *source, uint32_t size) {
    uint32_t i = 0;
    while (i < size && (source[i] == ' ' || source[i] == '\t' || source[i] == '\r')) i++;
    return i + 4 <= size && source[i] == 'F' && source[i + 1] == 'U' &&
           source[i + 2] == 'K' && source[i + 3] == '1' &&
           (i + 4 == size || source[i + 4] == '\n' || source[i + 4] == '\r');
}

static uint32_t read_source_line(const char *source, uint32_t size, uint32_t pc,
                                 char *line, uint32_t capacity, int *too_long) {
    uint32_t n = 0;
    *too_long = 0;
    while (pc < size && source[pc] != '\n') {
        if (n + 1u < capacity) line[n++] = source[pc];
        else *too_long = 1;
        pc++;
    }
    if (pc < size && source[pc] == '\n') pc++;
    line[n] = 0;
    return pc;
}

static int index_labels(app_vm *vm) {
    uint32_t pc = 0;
    while (pc < vm->size) {
        char line[APP_MAX_LINE]; int too_long;
        uint32_t next = read_source_line(vm->source, vm->size, pc,
                                         line, sizeof line, &too_long);
        if (too_long) return 0;
        char *p = skip_space(line);
        strip_inline_comment(p);
        trim_right(p);
        if (*p && *p != '#') {
            char *op = token(&p);
            if (eq(op, "label")) {
                char *name = token(&p);
                if (!valid_word(name) || vm->label_count >= APP_MAX_LABELS) return 0;
                for (uint32_t i = 0; i < vm->label_count; i++)
                    if (eq(vm->labels[i].name, name)) return 0;
                app_label *label = &vm->labels[vm->label_count++];
                uint32_t i = 0;
                while (name[i] && i + 1u < sizeof label->name) {
                    label->name[i] = name[i]; i++;
                }
                label->name[i] = 0;
                label->pc = next;
            }
        }
        pc = next;
    }
    return 1;
}

static int find_label(app_vm *vm, const char *wanted, uint32_t *destination) {
    for (uint32_t i = 0; i < vm->label_count; i++) {
        if (eq(vm->labels[i].name, wanted)) {
            *destination = vm->labels[i].pc;
            return 1;
        }
    }
    return 0;
}

/* Branch targets may use the reserved word "exit" for a compact if/else. */
static int branch_to(app_vm *vm, const char *target, uint32_t *pc) {
    if (eq(target, "exit")) return 2;
    return find_label(vm, target, pc) ? 1 : 0;
}

static int runtime_error(uint32_t line_no, const char *message) {
    console_puts("app: line ");
    put_i32((int32_t)line_no);
    console_puts(": ");
    console_puts(message);
    console_putc('\n');
    return 0;
}

static int run_vm(app_vm *vm) {
    uint32_t pc = 0, line_no = 0;
    while (pc < vm->size) {
        char line[APP_MAX_LINE]; int too_long;
        pc = read_source_line(vm->source, vm->size, pc,
                              line, sizeof line, &too_long);
        line_no++;
        if (too_long) return runtime_error(line_no, "line is longer than 2047 bytes");
        char *p = skip_space(line);
        strip_inline_comment(p);
        trim_right(p);
        if (!*p || *p == '#') continue;
        char *op = token(&p);
        if (eq(op, "FUK1") || eq(op, "label")) continue;
        if (++vm->steps > APP_MAX_STEPS) return runtime_error(line_no, "instruction limit exceeded");

        if (eq(op, "print") || eq(op, "println")) {
            p = skip_space(p); put_text(p);
            if (eq(op, "println")) console_putc('\n');
        } else if (eq(op, "printv")) {
            int32_t value; char *a = token(&p);
            if (!value_of(vm, a, &value)) return runtime_error(line_no, "unknown value");
            put_i32(value);
        } else if (eq(op, "printf") || eq(op, "printfn")) {
            float value; char *a = token(&p);
            if (!float_of(vm, a, &value)) return runtime_error(line_no, "unknown float value");
            put_f32(value);
            if (eq(op, "printfn")) console_putc('\n');
        } else if (eq(op, "prints") || eq(op, "printsn")) {
            char *a = token(&p);
            if (!*a) return runtime_error(line_no, "string value expected");
            put_text(string_of(vm, a));
            if (eq(op, "printsn")) console_putc('\n');
        } else if (eq(op, "putc")) {
            int32_t value; char *a = token(&p);
            if (!value_of(vm, a, &value) || value < 0 || value > 255)
                return runtime_error(line_no, "putc value must be 0..255");
            console_putc((char)value);
        } else if (eq(op, "input") || eq(op, "inputc")) {
            char *name = token(&p); app_var *dst = find_var(vm, name, 1);
            if (!dst) return runtime_error(line_no, "invalid variable");
            p = skip_space(p); put_text(p);
            if (*p) console_putc(' ');
            char in[64]; read_line(in, sizeof in);
            dst->type = APP_VALUE_INT;
            if (eq(op, "inputc")) {
                if (!in[0]) return runtime_error(line_no, "empty character input");
                dst->integer = (unsigned char)in[0];
            } else if (!parse_number(skip_space(in), &dst->integer))
                return runtime_error(line_no, "integer expected");
        } else if (eq(op, "inputf")) {
            char *name = token(&p); app_var *dst = find_var(vm, name, 1);
            if (!dst) return runtime_error(line_no, "invalid float variable");
            p = skip_space(p); put_text(p); if (*p) console_putc(' ');
            char in[64]; float value; read_line(in, sizeof in);
            if (!parse_float(skip_space(in), &value)) return runtime_error(line_no, "float expected");
            set_real(dst, value);
        } else if (eq(op, "inputs")) {
            char *name = token(&p); app_var *dst = find_var(vm, name, 1);
            if (!dst) return runtime_error(line_no, "invalid string variable");
            p = skip_space(p); put_text(p); if (*p) console_putc(' ');
            dst->type = APP_VALUE_STRING; read_line(dst->string, sizeof dst->string);
        } else if (eq(op, "clear")) {
            console_clear();
        } else if (eq(op, "cursor")) {
            char *xw = token(&p), *yw = token(&p); int32_t x, y;
            if (!value_of(vm, xw, &x) || !value_of(vm, yw, &y))
                return runtime_error(line_no, "bad cursor coordinates");
            if (!console_set_cursor((int)x, (int)y))
                return runtime_error(line_no, "cursor outside screen");
        } else if (eq(op, "color")) {
            char *iw = token(&p); int32_t id;
            if (!value_of(vm, iw, &id) || id < 0 || id > 15)
                return runtime_error(line_no, "color id must be 0..15");
            console_set_foreground(APP_COLORS[id]);
        } else if (eq(op, "array_set")) {
            char *iw = token(&p), *vw = token(&p); int32_t index, value;
            if (!value_of(vm, iw, &index) || !value_of(vm, vw, &value))
                return runtime_error(line_no, "bad array_set operands");
            if (index < 0 || index >= APP_ARRAY_SIZE)
                return runtime_error(line_no, "array index outside 0..4095");
            vm->array[index] = value;
        } else if (eq(op, "array_get")) {
            char *name = token(&p), *iw = token(&p); int32_t index;
            app_var *dst = find_var(vm, name, 1);
            if (!dst || !value_of(vm, iw, &index))
                return runtime_error(line_no, "bad array_get operands");
            if (index < 0 || index >= APP_ARRAY_SIZE)
                return runtime_error(line_no, "array index outside 0..4095");
            set_integer(dst, vm->array[index]);
        } else if (eq(op, "array_fill")) {
            char *vw = token(&p); int32_t value;
            if (!value_of(vm, vw, &value)) return runtime_error(line_no, "bad array_fill value");
            for (uint32_t i = 0; i < APP_ARRAY_SIZE; i++) vm->array[i] = value;
        } else if (eq(op, "array_size")) {
            char *name = token(&p); app_var *dst = find_var(vm, name, 1);
            if (!dst) return runtime_error(line_no, "invalid array_size variable");
            set_integer(dst, APP_ARRAY_SIZE);
        } else if (eq(op, "rand")) {
            char *name = token(&p), *minw = token(&p), *maxw = token(&p);
            int32_t minimum, maximum; app_var *dst = find_var(vm, name, 1);
            if (!dst || !value_of(vm, minw, &minimum) || !value_of(vm, maxw, &maximum))
                return runtime_error(line_no, "bad rand operands");
            if (minimum > maximum) return runtime_error(line_no, "rand min is greater than max");
            uint64_t span = (uint64_t)((int64_t)maximum - (int64_t)minimum) + 1u;
            uint32_t r = random_next(vm);
            dst->type = APP_VALUE_INT;
            dst->integer = span == 0x100000000ULL
                       ? (int32_t)r
                       : (int32_t)((int64_t)minimum + (int64_t)(r % (uint32_t)span));
        } else if (eq(op, "key_poll")) {
            char *name = token(&p); app_var *dst = find_var(vm, name, 1);
            if (!dst) return runtime_error(line_no, "invalid key_poll variable");
            set_integer(dst, (uint8_t)kbd_poll());
        } else if (eq(op, "sleep")) {
            char *mw = token(&p); int32_t ms;
            if (!value_of(vm, mw, &ms) || ms < 0 || (uint32_t)ms > APP_MAX_SLEEP_MS)
                return runtime_error(line_no, "sleep must be 0..60000 ms");
            app_sleep((uint32_t)ms);
        } else if (eq(op, "screen_cols") || eq(op, "screen_rows")) {
            char *name = token(&p); app_var *dst = find_var(vm, name, 1);
            if (!dst) return runtime_error(line_no, "invalid screen size variable");
            set_integer(dst, eq(op, "screen_cols") ? console_columns() : console_rows());
        } else if (eq(op, "time_ms")) {
            char *name = token(&p); app_var *dst = find_var(vm, name, 1);
            if (!dst) return runtime_error(line_no, "invalid time_ms variable");
            set_integer(dst, (int32_t)(rdtsc() / kbd_tsc_per_ms()));
        } else if (eq(op, "set") || eq(op, "mov")) {
            char *name = token(&p), *a = token(&p); int32_t value;
            app_var *dst = find_var(vm, name, 1);
            if (!dst || !value_of(vm, a, &value)) return runtime_error(line_no, "bad assignment operands");
            set_integer(dst, value);
        } else if (eq(op, "float") || eq(op, "fset")) {
            char *name = token(&p), *aw = token(&p); float a;
            app_var *dst = find_var(vm, name, 1);
            if (!dst || !float_of(vm, aw, &a)) return runtime_error(line_no, "bad float assignment");
            set_real(dst, a);
        } else if (eq(op, "str") || eq(op, "sset")) {
            char *name = token(&p), *aw = token(&p); app_var *dst = find_var(vm, name, 1);
            if (!dst || !*aw) return runtime_error(line_no, "bad string assignment");
            const char *a = string_of(vm, aw); char temporary[APP_STRING_SIZE];
            if (!string_copy(temporary, a, sizeof temporary)) return runtime_error(line_no, "string is longer than 127 bytes");
            dst->type = APP_VALUE_STRING; string_copy(dst->string, temporary, sizeof dst->string);
        } else if (eq(op, "strlen")) {
            char *name = token(&p), *aw = token(&p); app_var *dst = find_var(vm, name, 1);
            if (!dst || !*aw) return runtime_error(line_no, "bad strlen operands");
            set_integer(dst, (int32_t)kstrlen(string_of(vm, aw)));
        } else if (eq(op, "strcmp")) {
            char *name = token(&p), *aw = token(&p), *bw = token(&p);
            app_var *dst = find_var(vm, name, 1);
            if (!dst || !*aw || !*bw) return runtime_error(line_no, "bad strcmp operands");
            int result = string_compare(string_of(vm, aw), string_of(vm, bw));
            set_integer(dst, result < 0 ? -1 : result > 0 ? 1 : 0);
        } else if (eq(op, "strcat")) {
            char *name = token(&p), *aw = token(&p), *bw = token(&p);
            app_var *dst = find_var(vm, name, 1); char temporary[APP_STRING_SIZE];
            if (!dst || !*aw || !*bw) return runtime_error(line_no, "bad strcat operands");
            const char *a = string_of(vm, aw), *b = string_of(vm, bw); uint32_t n = 0;
            while (*a && n + 1u < sizeof temporary) temporary[n++] = *a++;
            while (*b && n + 1u < sizeof temporary) temporary[n++] = *b++;
            temporary[n] = 0;
            if (*a || *b) return runtime_error(line_no, "string result is longer than 127 bytes");
            dst->type = APP_VALUE_STRING; string_copy(dst->string, temporary, sizeof dst->string);
        } else if (eq(op, "stoi")) {
            char *name = token(&p), *aw = token(&p); int32_t value;
            app_var *dst = find_var(vm, name, 1);
            if (!dst || !parse_number(string_of(vm, aw), &value)) return runtime_error(line_no, "stoi needs an integer string");
            set_integer(dst, value);
        } else if (eq(op, "stof")) {
            char *name = token(&p), *aw = token(&p); float value;
            app_var *dst = find_var(vm, name, 1);
            if (!dst || !parse_float(string_of(vm, aw), &value)) return runtime_error(line_no, "stof needs a float string");
            set_real(dst, value);
        } else if (eq(op, "itof")) {
            char *name = token(&p), *aw = token(&p); int32_t value;
            app_var *dst = find_var(vm, name, 1);
            if (!dst || !value_of(vm, aw, &value)) return runtime_error(line_no, "itof needs an integer");
            set_real(dst, (float)value);
        } else if (eq(op, "ftoi")) {
            char *name = token(&p), *aw = token(&p); float value;
            app_var *dst = find_var(vm, name, 1);
            if (!dst || !float_of(vm, aw, &value) || value > 2147483000.0f || value < -2147483000.0f)
                return runtime_error(line_no, "ftoi value is outside int32 range");
            set_integer(dst, (int32_t)value);
        } else if (eq(op, "fneg") || eq(op, "fabs")) {
            char *name = token(&p), *aw = token(&p); float a;
            app_var *dst = find_var(vm, name, 1);
            if (!dst || !float_of(vm, aw, &a)) return runtime_error(line_no, "bad float unary operands");
            set_real(dst, eq(op, "fneg") ? -a : (a < 0.0f ? -a : a));
        } else if (eq(op, "fadd") || eq(op, "fsub") || eq(op, "fmul") ||
                   eq(op, "fdiv") || eq(op, "fmin") || eq(op, "fmax")) {
            char *name = token(&p), *aw = token(&p), *bw = token(&p); float a, b;
            app_var *dst = find_var(vm, name, 1);
            if (!dst || !float_of(vm, aw, &a) || !float_of(vm, bw, &b))
                return runtime_error(line_no, "bad float arithmetic operands");
            if (eq(op, "fdiv") && b == 0.0f) return runtime_error(line_no, "float division by zero");
            if (eq(op, "fadd")) set_real(dst, a + b);
            else if (eq(op, "fsub")) set_real(dst, a - b);
            else if (eq(op, "fmul")) set_real(dst, a * b);
            else if (eq(op, "fdiv")) set_real(dst, a / b);
            else if (eq(op, "fmin")) set_real(dst, a < b ? a : b);
            else set_real(dst, a > b ? a : b);
        } else if (eq(op, "clamp")) {
            char *name = token(&p), *vw = token(&p), *loww = token(&p), *highw = token(&p);
            int32_t value, low, high; app_var *dst = find_var(vm, name, 1);
            if (!dst || !value_of(vm, vw, &value) || !value_of(vm, loww, &low) ||
                !value_of(vm, highw, &high) || low > high) return runtime_error(line_no, "bad clamp operands");
            set_integer(dst, value < low ? low : value > high ? high : value);
        } else if (eq(op, "inc") || eq(op, "dec")) {
            char *name = token(&p); app_var *dst = find_var(vm, name, 0);
            if (!dst || dst->type != APP_VALUE_INT) return runtime_error(line_no, "integer variable expected");
            dst->integer += eq(op, "inc") ? 1 : -1;
        } else if (eq(op, "neg") || eq(op, "abs") || eq(op, "bit_not")) {
            char *name = token(&p), *aw = token(&p); int32_t a;
            app_var *dst = find_var(vm, name, 1);
            if (!dst || !value_of(vm, aw, &a)) return runtime_error(line_no, "bad unary operands");
            dst->type = APP_VALUE_INT;
            if (eq(op, "neg")) dst->integer = -a;
            else if (eq(op, "abs")) dst->integer = a < 0 ? -a : a;
            else dst->integer = ~a;
        } else if (eq(op, "add") || eq(op, "sub") || eq(op, "mul") ||
                   eq(op, "div") || eq(op, "mod") || eq(op, "bit_and") ||
                   eq(op, "bit_or") || eq(op, "bit_xor") || eq(op, "shl") ||
                   eq(op, "shr") || eq(op, "min") || eq(op, "max")) {
            char *name = token(&p), *aw = token(&p), *bw = token(&p);
            int32_t a, b; app_var *dst = find_var(vm, name, 1);
            if (!dst || !value_of(vm, aw, &a) || !value_of(vm, bw, &b))
                return runtime_error(line_no, "bad arithmetic operands");
            dst->type = APP_VALUE_INT;
            if ((eq(op, "div") || eq(op, "mod")) && b == 0)
                return runtime_error(line_no, "division by zero");
            if ((eq(op, "shl") || eq(op, "shr")) && (b < 0 || b > 31))
                return runtime_error(line_no, "shift must be 0..31");
            if (eq(op, "add")) dst->integer = a + b;
            else if (eq(op, "sub")) dst->integer = a - b;
            else if (eq(op, "mul")) dst->integer = a * b;
            else if (eq(op, "div")) dst->integer = a / b;
            else if (eq(op, "mod")) dst->integer = a % b;
            else if (eq(op, "bit_and")) dst->integer = a & b;
            else if (eq(op, "bit_or")) dst->integer = a | b;
            else if (eq(op, "bit_xor")) dst->integer = a ^ b;
            else if (eq(op, "shl")) dst->integer = (int32_t)((uint32_t)a << b);
            else if (eq(op, "shr")) dst->integer = (int32_t)((uint32_t)a >> b);
            else if (eq(op, "min")) dst->integer = a < b ? a : b;
            else dst->integer = a > b ? a : b;
        } else if (eq(op, "call")) {
            char *label = token(&p); uint32_t destination;
            if (!find_label(vm, label, &destination)) return runtime_error(line_no, "label not found");
            if (vm->call_depth >= APP_CALL_DEPTH) return runtime_error(line_no, "call stack overflow");
            vm->call_stack[vm->call_depth++] = pc;
            pc = destination;
        } else if (eq(op, "return")) {
            if (vm->call_depth == 0) return runtime_error(line_no, "call stack underflow");
            pc = vm->call_stack[--vm->call_depth];
        } else if (eq(op, "goto")) {
            char *label = token(&p);
            if (!find_label(vm, label, &pc)) return runtime_error(line_no, "label not found");
        } else if (eq(op, "cmp_eq") || eq(op, "cmp_ne") || eq(op, "cmp_lt") ||
                   eq(op, "cmp_le") || eq(op, "cmp_gt") || eq(op, "cmp_ge")) {
            char *name = token(&p), *aw = token(&p), *bw = token(&p);
            int32_t a, b; app_var *dst = find_var(vm, name, 1);
            if (!dst || !value_of(vm, aw, &a) || !value_of(vm, bw, &b))
                return runtime_error(line_no, "bad comparison operands");
            dst->type = APP_VALUE_INT;
            if (eq(op, "cmp_eq")) dst->integer = a == b;
            else if (eq(op, "cmp_ne")) dst->integer = a != b;
            else if (eq(op, "cmp_lt")) dst->integer = a < b;
            else if (eq(op, "cmp_le")) dst->integer = a <= b;
            else if (eq(op, "cmp_gt")) dst->integer = a > b;
            else dst->integer = a >= b;
        } else if (eq(op, "cmp_feq") || eq(op, "cmp_fne") || eq(op, "cmp_flt") ||
                   eq(op, "cmp_fle") || eq(op, "cmp_fgt") || eq(op, "cmp_fge")) {
            char *name = token(&p), *aw = token(&p), *bw = token(&p); float a, b;
            app_var *dst = find_var(vm, name, 1); int result;
            if (!dst || !float_of(vm, aw, &a) || !float_of(vm, bw, &b))
                return runtime_error(line_no, "bad float comparison operands");
            if (eq(op, "cmp_feq")) result = a == b;
            else if (eq(op, "cmp_fne")) result = a != b;
            else if (eq(op, "cmp_flt")) result = a < b;
            else if (eq(op, "cmp_fle")) result = a <= b;
            else if (eq(op, "cmp_fgt")) result = a > b;
            else result = a >= b;
            set_integer(dst, result);
        } else if (eq(op, "cmp_fnear")) {
            char *name = token(&p), *aw = token(&p), *bw = token(&p), *ew = token(&p);
            float a, b, epsilon, difference; app_var *dst = find_var(vm, name, 1);
            if (!dst || !float_of(vm, aw, &a) || !float_of(vm, bw, &b) ||
                !float_of(vm, ew, &epsilon) || epsilon < 0.0f)
                return runtime_error(line_no, "bad cmp_fnear operands");
            difference = a - b; if (difference < 0.0f) difference = -difference;
            set_integer(dst, difference <= epsilon);
        } else if (eq(op, "cmp_seq") || eq(op, "cmp_sne") || eq(op, "cmp_slt") ||
                   eq(op, "cmp_sle") || eq(op, "cmp_sgt") || eq(op, "cmp_sge")) {
            char *name = token(&p), *aw = token(&p), *bw = token(&p);
            app_var *dst = find_var(vm, name, 1);
            if (!dst || !*aw || !*bw) return runtime_error(line_no, "bad string comparison operands");
            int comparison = string_compare(string_of(vm, aw), string_of(vm, bw)), result;
            if (eq(op, "cmp_seq")) result = comparison == 0;
            else if (eq(op, "cmp_sne")) result = comparison != 0;
            else if (eq(op, "cmp_slt")) result = comparison < 0;
            else if (eq(op, "cmp_sle")) result = comparison <= 0;
            else if (eq(op, "cmp_sgt")) result = comparison > 0;
            else result = comparison >= 0;
            set_integer(dst, result);
        } else if (eq(op, "if_eq") || eq(op, "if_ne") || eq(op, "if_lt") ||
                   eq(op, "if_le") || eq(op, "if_gt") || eq(op, "if_ge")) {
            char *aw = token(&p), *bw = token(&p), *true_label = token(&p), *false_label = token(&p);
            int32_t a, b; int take = 0;
            if (!value_of(vm, aw, &a) || !value_of(vm, bw, &b) || !*true_label)
                return runtime_error(line_no, "bad branch operands");
            if (eq(op, "if_eq")) take = a == b;
            else if (eq(op, "if_ne")) take = a != b;
            else if (eq(op, "if_lt")) take = a < b;
            else if (eq(op, "if_le")) take = a <= b;
            else if (eq(op, "if_gt")) take = a > b;
            else take = a >= b;
            const char *target = take ? true_label : false_label;
            if (*target) { int result = branch_to(vm, target, &pc); if (result == 2) return 1; if (!result) return runtime_error(line_no, "label not found"); }
        } else if (eq(op, "if_feq") || eq(op, "if_fne") || eq(op, "if_flt") ||
                   eq(op, "if_fle") || eq(op, "if_fgt") || eq(op, "if_fge")) {
            char *aw = token(&p), *bw = token(&p), *true_label = token(&p), *false_label = token(&p);
            float a, b; int take;
            if (!float_of(vm, aw, &a) || !float_of(vm, bw, &b) || !*true_label)
                return runtime_error(line_no, "bad float branch operands");
            if (eq(op, "if_feq")) take = a == b;
            else if (eq(op, "if_fne")) take = a != b;
            else if (eq(op, "if_flt")) take = a < b;
            else if (eq(op, "if_fle")) take = a <= b;
            else if (eq(op, "if_fgt")) take = a > b;
            else take = a >= b;
            const char *target = take ? true_label : false_label;
            if (*target) { int result = branch_to(vm, target, &pc); if (result == 2) return 1; if (!result) return runtime_error(line_no, "label not found"); }
        } else if (eq(op, "if_fnear")) {
            char *aw = token(&p), *bw = token(&p), *ew = token(&p);
            char *true_label = token(&p), *false_label = token(&p); float a, b, epsilon, difference;
            if (!float_of(vm, aw, &a) || !float_of(vm, bw, &b) || !float_of(vm, ew, &epsilon) ||
                epsilon < 0.0f || !*true_label) return runtime_error(line_no, "bad if_fnear operands");
            difference = a - b; if (difference < 0.0f) difference = -difference;
            const char *target = difference <= epsilon ? true_label : false_label;
            if (*target) { int result = branch_to(vm, target, &pc); if (result == 2) return 1; if (!result) return runtime_error(line_no, "label not found"); }
        } else if (eq(op, "if_seq") || eq(op, "if_sne") || eq(op, "if_slt") ||
                   eq(op, "if_sle") || eq(op, "if_sgt") || eq(op, "if_sge")) {
            char *aw = token(&p), *bw = token(&p), *true_label = token(&p), *false_label = token(&p);
            if (!*aw || !*bw || !*true_label) return runtime_error(line_no, "bad string branch operands");
            int comparison = string_compare(string_of(vm, aw), string_of(vm, bw)), take;
            if (eq(op, "if_seq")) take = comparison == 0;
            else if (eq(op, "if_sne")) take = comparison != 0;
            else if (eq(op, "if_slt")) take = comparison < 0;
            else if (eq(op, "if_sle")) take = comparison <= 0;
            else if (eq(op, "if_sgt")) take = comparison > 0;
            else take = comparison >= 0;
            const char *target = take ? true_label : false_label;
            if (*target) { int result = branch_to(vm, target, &pc); if (result == 2) return 1; if (!result) return runtime_error(line_no, "label not found"); }
        } else if (eq(op, "if_true") || eq(op, "if_false")) {
            char *aw = token(&p), *true_label = token(&p), *false_label = token(&p); int32_t value;
            if (!value_of(vm, aw, &value) || !*true_label) return runtime_error(line_no, "bad boolean branch operands");
            int take = eq(op, "if_true") ? value != 0 : value == 0;
            const char *target = take ? true_label : false_label;
            if (*target) { int result = branch_to(vm, target, &pc); if (result == 2) return 1; if (!result) return runtime_error(line_no, "label not found"); }
        } else if (eq(op, "exit")) {
            return 1;
        } else return runtime_error(line_no, "unknown instruction");
    }
    return 1;
}

int app_start(const char *name) {
    if (!fat_mounted()) { console_puts("start: no storage mounted.\n"); return 0; }
    if (!valid_word(name)) { console_puts("Usage: start <app-name>\n"); return 0; }

    char path[160] = "/apps/";
    kstrcat(path, name);
    kstrcat(path, ".fuk");
    fat_file file;
    if (!fat_open_path(path, &file)) {
        console_puts("start: app not found: "); console_puts(path); console_putc('\n');
        return 0;
    }
    if (file.size == 0 || file.size > APP_MAX_SIZE) {
        console_puts("start: app must be 1..524288 bytes.\n"); return 0;
    }
    char *source = (char *)kheap_alloc(file.size + 1u);
    if (!source) { console_puts("start: not enough memory.\n"); return 0; }
    uint32_t got = fat_read(&file, (uint8_t *)source, file.size);
    source[got] = 0;
    if (got != file.size || !first_line_ok(source, got)) {
        console_puts("start: invalid .fuk file (missing FUK1 header).\n");
        kheap_free(source); return 0;
    }
    app_vm *vm = (app_vm *)kheap_calloc(1u, sizeof *vm);
    if (!vm) {
        console_puts("start: not enough memory for VM state.\n");
        kheap_free(source); return 0;
    }
    vm->source = source; vm->size = got;
    vm->random_state = (uint32_t)rdtsc() ^ file.first_cluster ^ file.size;
    if (!index_labels(vm)) {
        console_puts("start: invalid, duplicate, or excessive labels.\n");
        kheap_free(vm); kheap_free(source); return 0;
    }
    int ok = run_vm(vm);
    console_set_colors(0xD8E0F0, 0x0B0F1A);
    kheap_free(vm);
    kheap_free(source);
    return ok;
}
