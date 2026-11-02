/*
 * settings_store.c — read a program's settings file, change only the values.
 *
 * The whole of this file exists to keep one promise: a save replaces the
 * VALUE of the settings the panel was asked to change and copies every other
 * byte of the file through untouched. The comments a person wrote, the
 * settings this program does not know, the parts of the file it has no
 * business touching — they survive.
 *
 * It does that by scanning, not by rewriting. A setting is found by its token,
 * the span that holds its value is remembered, and the file is rebuilt as
 * "everything before the value, the new value, everything after" — repeated
 * for each change. The scanner knows four ways a value is written (see
 * settings_types.h) and one rule everywhere: what is inside a quoted string is
 * never a comment and never a delimiter.
 *
 * A row may be written differently from the rest of its program, and a file may
 * write the same call more than once — the terminal splits its settings across
 * several `gcl_Terminal.call(...)` blocks. Both are handled here: the writing
 * style and the call prefix are resolved per row, and every call block is
 * walked rather than only the first.
 */
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "settings_store.h"

#define SETTINGS_FILE_MAX_SIZE (1024 * 1024)

/* The most edits one save may make: a value change for every row, and an
   insertion for every row the file did not name. Twice the row ceiling is far
   above both. */
#define SETTINGS_MAX_EDITS (SETTINGS_MAX_ROWS * 2)

/* One change to the file: replace [start, end) with `text`, or — when
   start == end — insert `text` at that point. */
typedef struct Edit {
    size_t start;
    size_t end;
    char   text[SETTINGS_TEXT_LENGTH * 4 + 33];
} Edit;

/* The style a row is written with: its own when it names one, the program's
   otherwise. */
static SettingStyle row_style(const AppDef *app, const SettingDef *def) {
    return def->style == SETTING_STYLE_INHERIT ? app->style : def->style;
}

/* The call prefix a row is written under: its own when it names one, the
   program's otherwise. */
static const char *row_call(const AppDef *app, const SettingDef *def) {
    return def->call ? def->call : app->call;
}

/* --- small scanning helpers ------------------------------------------------ */

static int is_name_char(int c) {
    return isalnum((unsigned char)c) || c == '_';
}

/* Past a comment that starts at `i` (which points at '#'), to the newline that
   ends it, or to `end`. */
static size_t skip_comment(const char *text, size_t i, size_t end) {
    while (i < end && text[i] != '\n') {
        i++;
    }
    return i;
}

/* Past a double-quoted string that starts at `i` (which points at the opening
   quote), to the character after the closing one. A backslash escapes the next
   character, so an escaped quote does not close the string. */
static size_t skip_string(const char *text, size_t i, size_t end) {
    if (i >= end || text[i] != '"') {
        return i;
    }
    i++;
    while (i < end) {
        if (text[i] == '\\' && i + 1 < end) {
            i += 2;
            continue;
        }
        if (text[i] == '"') {
            return i + 1;
        }
        i++;
    }
    return end;
}

/* The index one past the end of the value that begins at `i`, stopping at the
   comma, the closing bracket or the comment that ends it. Nested brackets and
   braces are stepped over, so a value written `[a, b]` is not cut at its own
   comma, and a '#' inside a string is not read as a comment. */
static size_t scan_value_end(const char *text, size_t i, size_t end) {
    int depth = 0;
    while (i < end) {
        char c = text[i];
        if (c == '"') {
            i = skip_string(text, i, end);
            continue;
        }
        if (depth == 0 && c == '#') {
            break;
        }
        if (depth == 0 && c == '\n') {
            break;
        }
        if (c == '(' || c == '[' || c == '{') {
            depth++;
        } else if (c == ')' || c == ']' || c == '}') {
            if (depth == 0) {
                break;
            }
            depth--;
        } else if (c == ',' && depth == 0) {
            break;
        }
        i++;
    }
    return i;
}

static void trim_span(const char *text, size_t *start, size_t *end) {
    while (*end > *start && isspace((unsigned char)text[*end - 1])) {
        (*end)--;
    }
    while (*start < *end && isspace((unsigned char)text[*start])) {
        (*start)++;
    }
}

/* Whether `token` appears at `pos` as a whole word: the character before it is
   not part of a name, and the character after it is not either. */
static int token_at(const char *text, size_t len, size_t pos, const char *token) {
    size_t n = strlen(token);
    if (pos + n > len) {
        return 0;
    }
    if (memcmp(text + pos, token, n) != 0) {
        return 0;
    }
    if (pos > 0 && is_name_char((unsigned char)text[pos - 1])) {
        return 0;
    }
    if (pos + n < len && is_name_char((unsigned char)text[pos + n])) {
        return 0;
    }
    return 1;
}

/* --- finding a value ------------------------------------------------------- */

/* The calling block named `call`, searched from `from` onwards: the span
   between its '(' and its matching ')'. Returns 1 and fills the two when the
   call is found. The caller walks the matches because a file may write the
   same call more than once. */
static int find_call_from(const char *text, size_t len, const char *call,
                          size_t from, size_t *args_start, size_t *args_end) {
    size_t n = strlen(call);
    for (size_t p = from; p + n <= len; p++) {
        if (!token_at(text, len, p, call)) {
            continue;
        }
        size_t q = p + n;
        while (q < len && isspace((unsigned char)text[q])) {
            q++;
        }
        if (q >= len || text[q] != '(') {
            continue;
        }
        /* The matching close, stepping over strings and comments. */
        size_t i = q + 1;
        int depth = 1;
        while (i < len) {
            char c = text[i];
            if (c == '"') {
                i = skip_string(text, i, len);
                continue;
            }
            if (c == '#') {
                i = skip_comment(text, i, len);
                continue;
            }
            if (c == '(') {
                depth++;
            } else if (c == ')') {
                depth--;
                if (depth == 0) {
                    *args_start = q + 1;
                    *args_end = i;
                    return 1;
                }
            }
            i++;
        }
    }
    return 0;
}

/* The first calling block named `call`. */
static int find_call(const char *text, size_t len, const char *call,
                     size_t *args_start, size_t *args_end) {
    return find_call_from(text, len, call, 0, args_start, args_end);
}

/* The value of the named argument inside an argument list. Returns 1 and fills
   the raw span — the quotes still around a string — when it is there. */
static int find_arg_value(const char *text, size_t start, size_t end,
                          const char *key, size_t *vstart, size_t *vend) {
    size_t i = start;
    while (i < end) {
        while (i < end && (isspace((unsigned char)text[i]) || text[i] == ',')) {
            i++;
        }
        if (i >= end) {
            break;
        }
        if (text[i] == '#') {
            i = skip_comment(text, i, end);
            continue;
        }
        /* The argument's name: a bare identifier. */
        size_t name_start = i;
        while (i < end && is_name_char((unsigned char)text[i])) {
            i++;
        }
        size_t name_len = i - name_start;
        while (i < end && isspace((unsigned char)text[i])) {
            i++;
        }
        if (i < end && text[i] == '=') {
            i++;
            while (i < end && isspace((unsigned char)text[i])) {
                i++;
            }
            size_t vs = i;
            size_t ve = scan_value_end(text, i, end);
            if (name_len == strlen(key) &&
                strncmp(text + name_start, key, name_len) == 0) {
                *vstart = vs;
                *vend = ve;
                return 1;
            }
            i = ve;
        } else {
            /* A positional argument, or something this reader does not name:
               step past it so the next one is found. */
            i = scan_value_end(text, i, end);
        }
    }
    return 0;
}

/* The first positional value of an argument list — a function's own argument —
   used by SETTING_FUNC, whose call takes the value and not a named one. */
static int find_first_value(const char *text, size_t start, size_t end,
                            size_t *vstart, size_t *vend) {
    size_t i = start;
    while (i < end && (isspace((unsigned char)text[i]) || text[i] == ',')) {
        i++;
    }
    if (i >= end || text[i] == '#') {
        return 0;
    }
    *vstart = i;
    *vend = scan_value_end(text, i, end);
    return 1;
}

/* The value of a line `left_side = value`. `left_side` is matched at the start
   of a line, after any leading whitespace. */
static int find_line_value(const char *text, size_t len, const char *left_side,
                           size_t *vstart, size_t *vend) {
    size_t n = strlen(left_side);
    size_t i = 0;
    while (i < len) {
        size_t j = i;
        while (j < len && (text[j] == ' ' || text[j] == '\t')) {
            j++;
        }
        if (j < len && text[j] != '#' && text[j] != '\n' &&
            j + n <= len && memcmp(text + j, left_side, n) == 0 &&
            (j + n >= len || (!is_name_char((unsigned char)text[j + n]) &&
                              text[j + n] != '.'))) {
            size_t k = j + n;
            while (k < len && (text[k] == ' ' || text[k] == '\t')) {
                k++;
            }
            if (k < len && text[k] == '=') {
                k++;
                while (k < len && (text[k] == ' ' || text[k] == '\t')) {
                    k++;
                }
                *vstart = k;
                *vend = scan_value_end(text, k, len);
                return 1;
            }
        }
        while (i < len && text[i] != '\n') {
            i++;
        }
        if (i < len) {
            i++;
        }
    }
    return 0;
}

/* Find a section body in a GKeyFile-style config and optionally the value of
   one key inside it. Section bounds exclude the header and the next section. */
static int find_ini_section(const char *text, size_t len, const char *section,
                            size_t *body_start, size_t *body_end) {
    size_t i = 0;
    size_t section_start = (size_t)-1;
    size_t section_end = len;
    size_t section_len = strlen(section);

    while (i < len) {
        size_t line_start = i;
        size_t line_end = i;
        while (line_end < len && text[line_end] != '\n') {
            line_end++;
        }
        size_t first = line_start;
        while (first < line_end &&
               (text[first] == ' ' || text[first] == '\t' ||
                text[first] == '\r')) {
            first++;
        }
        if (first < line_end && text[first] == '[') {
            size_t close = first + 1;
            while (close < line_end && text[close] != ']') {
                close++;
            }
            if (close < line_end) {
                size_t after = close + 1;
                while (after < line_end &&
                       (text[after] == ' ' || text[after] == '\t' ||
                        text[after] == '\r')) {
                    after++;
                }
                if (after == line_end || text[after] == '#' ||
                    text[after] == ';') {
                    size_t name_len = close - first - 1;
                    if (section_start != (size_t)-1) {
                        section_end = line_start;
                        break;
                    }
                    if (name_len == section_len &&
                        memcmp(text + first + 1, section, section_len) == 0) {
                        section_start = line_end < len ? line_end + 1 : len;
                    }
                }
            }
        }
        i = line_end < len ? line_end + 1 : len;
    }

    if (section_start == (size_t)-1) {
        return 0;
    }
    *body_start = section_start;
    *body_end = section_end;
    return 1;
}

static int find_ini_value(const char *text, size_t len, const char *section,
                          const char *key, size_t *vstart, size_t *vend) {
    size_t start = 0;
    size_t end = 0;
    if (!find_ini_section(text, len, section, &start, &end)) {
        return 0;
    }
    size_t value_start = 0;
    size_t value_end = 0;
    if (!find_line_value(text + start, end - start, key,
                         &value_start, &value_end)) {
        return 0;
    }
    *vstart = start + value_start;
    *vend = start + value_end;
    return 1;
}

/* Where a setting's value is, for the row's writing style. Returns 1 and fills
   the raw span. */
static int locate(const char *text, size_t len, const AppDef *app,
                  const SettingDef *def, size_t *vstart, size_t *vend) {
    char name[SETTINGS_TEXT_LENGTH * 2];
    SettingStyle style = row_style(app, def);
    const char *call = row_call(app, def);

    if (style == SETTING_CALL) {
        /* Walk every block of the call: the terminal writes one call per
           group of settings, and a key may be in any of them. */
        size_t scan = 0;
        size_t as = 0;
        size_t ae = 0;
        while (find_call_from(text, len, call, scan, &as, &ae)) {
            if (find_arg_value(text, as, ae, def->key, vstart, vend)) {
                return 1;
            }
            scan = ae + 1;
        }
        return 0;
    }
    if (style == SETTING_FLAT) {
        return find_line_value(text, len, def->key, vstart, vend);
    }
    if (style == SETTING_INI) {
        return find_ini_value(text, len, call, def->key, vstart, vend);
    }
    if (style == SETTING_FUNC) {
        /* gcl_Window.set_active_window_border_color("#..."): a call whose name
           is <call>.<key>, and the value is its first argument. */
        snprintf(name, sizeof(name), "%s.%s", call, def->key);
        size_t as = 0;
        size_t ae = 0;
        if (!find_call(text, len, name, &as, &ae)) {
            return 0;
        }
        return find_first_value(text, as, ae, vstart, vend);
    }
    /* SETTING_ASSIGN: gcl_Switcher.background = "#...". */
    snprintf(name, sizeof(name), "%s.%s", call, def->key);
    return find_line_value(text, len, name, vstart, vend);
}

/* --- turning a raw span into a value, and back ----------------------------- */

/* Copy the string between a pair of quotes into `out`, turning the two escapes
   that matter — \" and \\ — back into what they stand for, and leaving every
   other backslash sequence exactly as written: a prompt's \e, \[ and \] are
   the shell's own and must reach it unchanged. */
static void copy_unquoted(char *out, unsigned int size,
                          const char *text, size_t start, size_t end) {
    trim_span(text, &start, &end);
    if (start < end && text[start] == '"') {
        start++;
    }
    if (end > start && text[end - 1] == '"') {
        end--;
    }
    unsigned int o = 0;
    for (size_t i = start; i < end && o + 1 < size; i++) {
        if (text[i] == '\\' && i + 1 < end &&
            (text[i + 1] == '"' || text[i + 1] == '\\')) {
            out[o++] = text[i + 1];
            i++;
            continue;
        }
        out[o++] = text[i];
    }
    out[o] = '\0';
}

static void copy_list(char *out, unsigned int size,
                      const char *text, size_t start, size_t end) {
    unsigned int written = 0;
    int first = 1;
    for (size_t i = start; i < end && written + 1 < size;) {
        if (text[i] != '"') {
            i++;
            continue;
        }
        i++;
        if (!first && written + 1 < size) {
            out[written++] = ';';
        }
        first = 0;
        while (i < end && text[i] != '"' && written + 1 < size) {
            if (text[i] == '\\' && i + 1 < end &&
                (text[i + 1] == '"' || text[i + 1] == '\\')) {
                i++;
            }
            out[written++] = text[i++];
        }
        if (i < end && text[i] == '"') {
            i++;
        }
    }
    out[written] = '\0';
}

/* Write `value` between quotes, escaping only a quote and a backslash so the
   result reads back as the same value. */
static void append_quoted(char *out, unsigned int size, const char *value) {
    unsigned int o = (unsigned int)strlen(out);
    if (o + 1 < size) {
        out[o++] = '"';
    }
    for (const char *p = value; *p && o + 2 < size; p++) {
        if (*p == '"' || *p == '\\') {
            out[o++] = '\\';
        }
        out[o++] = *p;
    }
    if (o + 1 < size) {
        out[o++] = '"';
    }
    out[o] = '\0';
}

/* The value as it should stand in the file: a string for a colour or a text, a
   bare number for an int, a bare fraction for a real, True/False (or
   true/false) for a switch. */
static void render_value(char *out, unsigned int size, const SettingDef *def,
                         const SettingValue *value, int capital_bools) {
    out[0] = '\0';
    if (def->type == SETTING_INT) {
        snprintf(out, size, "%d", setting_value_int(value));
        return;
    }
    if (def->type == SETTING_REAL) {
        /* A fraction is written bare, the way the file writes it — 1.0 and not
           "1.0" — so a value a person typed lands in the file unchanged. */
        snprintf(out, size, "%s", value->text);
        return;
    }
    if (def->type == SETTING_BOOL) {
        int flag = setting_value_bool(value);
        const char *word;
        if (capital_bools) {
            word = flag ? "True" : "False";
        } else {
            word = flag ? "true" : "false";
        }
        snprintf(out, size, "%s", word);
        return;
    }
    if (def->type == SETTING_LIST) {
        snprintf(out, size, "[");
        const char *item = value->text;
        while (*item) {
            const char *end = strchr(item, ';');
            size_t length = end ? (size_t)(end - item) : strlen(item);
            while (length > 0 && isspace((unsigned char)item[length - 1])) {
                length--;
            }
            while (length > 0 && isspace((unsigned char)*item)) {
                item++;
                length--;
            }
            if (length > 0) {
                char entry[SETTINGS_TEXT_LENGTH];
                if (strlen(out) > 1) {
                    strncat(out, ", ", size - strlen(out) - 1);
                }
                size_t copy = length < sizeof(entry) - 1
                            ? length : sizeof(entry) - 1;
                memcpy(entry, item, copy);
                entry[copy] = '\0';
                append_quoted(out, size, entry);
            }
            if (!end) {
                break;
            }
            item = end + 1;
        }
        strncat(out, "]", size - strlen(out) - 1);
        return;
    }
    append_quoted(out, size, value->text);
}

/* --- public: values -------------------------------------------------------- */

int setting_value_int(const SettingValue *value) {
    const char *p = value->text;
    while (*p && isspace((unsigned char)*p)) {
        p++;
    }
    if (!*p) {
        return 0;
    }
    return (int)strtol(p, NULL, 10);
}

int setting_value_bool(const SettingValue *value) {
    if (strcmp(value->text, "True") == 0 || strcmp(value->text, "true") == 0 ||
        strcmp(value->text, "1") == 0 || strcmp(value->text, "yes") == 0 ||
        strcmp(value->text, "on") == 0) {
        return 1;
    }
    return 0;
}

void setting_value_set_int(SettingValue *value, int number) {
    snprintf(value->text, sizeof(value->text), "%d", number);
}

void setting_value_set_bool(SettingValue *value, int flag, int capital) {
    const char *word;
    if (capital) {
        word = flag ? "True" : "False";
    } else {
        word = flag ? "true" : "false";
    }
    snprintf(value->text, sizeof(value->text), "%s", word);
}

/* --- ensure the directory chain to a path exists -------------------------- */

/* Create every directory in `path`'s parent chain, like mkdir -p. Returns 0
   on success, -1 on an error worth reporting. A path whose parent already
   exists is not an error. */
static int mkdir_parents(char *path) {
    char *p = path;
    /* Skip a leading slash so a rooted path still walks its components. */
    if (*p == '/') {
        p++;
    }
    while (*p) {
        /* Find the next separator. */
        char *slash = strchr(p, '/');
        if (slash) {
            *slash = '\0';
        }
        if (path[0]) {
            if (mkdir(path, 0755) != 0 && errno != EEXIST) {
                if (slash) {
                    *slash = '/';
                }
                return -1;
            }
        }
        if (!slash) {
            break;
        }
        *slash = '/';
        p = slash + 1;
    }
    return 0;
}

/* --- reading ---------------------------------------------------------------- */

char *settings_path(char *out, unsigned int size, const AppDef *app) {
    const char *xdg = getenv("XDG_CONFIG_HOME");
    if (xdg && xdg[0]) {
        snprintf(out, size, "%s/%s/%s", xdg, app->dir, app->file);
        return out;
    }
    const char *home = getenv("HOME");
    if (home && home[0]) {
        snprintf(out, size, "%s/.config/%s/%s", home, app->dir, app->file);
        return out;
    }
    out[0] = '\0';
    return out;
}

/* --- reading --------------------------------------------------------------- */

static char *read_file(const char *path, size_t *out_length) {
    FILE *file = fopen(path, "rb");
    if (!file) {
        return NULL;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return NULL;
    }
    long size = ftell(file);
    if (size < 0) {
        fclose(file);
        return NULL;
    }
    if ((unsigned long)size > SETTINGS_FILE_MAX_SIZE) {
        fclose(file);
        errno = EFBIG;
        return NULL;
    }
    if (fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return NULL;
    }
    char *buffer = malloc((size_t)size + 1);
    if (!buffer) {
        fclose(file);
        return NULL;
    }
    size_t got = fread(buffer, 1, (size_t)size, file);
    int read_failed = ferror(file) || got != (size_t)size;
    fclose(file);
    if (read_failed) {
        free(buffer);
        errno = EIO;
        return NULL;
    }
    buffer[got] = '\0';
    *out_length = got;
    return buffer;
}

int settings_load(const AppDef *app, SettingValue *values) {
    for (int i = 0; i < app->setting_count; i++) {
        snprintf(values[i].text, sizeof(values[i].text), "%s",
                 app->settings[i].fallback);
        values[i].present = 0;
    }

    char path[SETTINGS_TEXT_LENGTH * 2];
    settings_path(path, sizeof(path), app);
    if (!path[0]) {
        return -1;
    }

    size_t length = 0;
    errno = 0;
    char *text = read_file(path, &length);
    if (!text) {
        if (errno != ENOENT) {
            return -1;
        }

        /* Read old static call-style files without evaluating them. A save
           still writes the new flat .conf file at the normal path. */
        char legacy_path[SETTINGS_TEXT_LENGTH * 2];
        snprintf(legacy_path, sizeof(legacy_path), "%s", path);
        char *suffix = strrchr(legacy_path, '.');
        if (suffix && strcmp(suffix, ".conf") == 0) {
            strcpy(suffix, ".py");
            text = read_file(legacy_path, &length);
        }
        if (!text) {
            return errno == ENOENT ? 0 : -1;
        }

        AppDef legacy_app = *app;
        legacy_app.style = SETTING_CALL;
        app = &legacy_app;
    }

    for (int i = 0; i < app->setting_count; i++) {
        size_t vs = 0;
        size_t ve = 0;
        if (locate(text, length, app, &app->settings[i], &vs, &ve)) {
            if (app->settings[i].type == SETTING_LIST) {
                copy_list(values[i].text, sizeof(values[i].text), text, vs, ve);
            } else {
                copy_unquoted(values[i].text, sizeof(values[i].text),
                              text, vs, ve);
            }
            values[i].present = 1;
        }
    }
    free(text);
    return 1;
}

typedef struct LegacyValueSpan {
    char key[SETTINGS_TEXT_LENGTH];
    size_t start;
    size_t end;
} LegacyValueSpan;

#define LEGACY_MAX_VALUES 256

static void collect_legacy_call_values(const char *text, size_t start,
                                       size_t end, LegacyValueSpan *values,
                                       int *count) {
    size_t i = start;
    while (i < end) {
        while (i < end &&
               (isspace((unsigned char)text[i]) || text[i] == ',')) {
            i++;
        }
        if (i >= end) {
            break;
        }
        if (text[i] == '#') {
            i = skip_comment(text, i, end);
            continue;
        }

        size_t key_start = i;
        while (i < end && is_name_char((unsigned char)text[i])) {
            i++;
        }
        size_t key_length = i - key_start;
        while (i < end && isspace((unsigned char)text[i])) {
            i++;
        }
        if (!key_length || i >= end || text[i] != '=') {
            i = scan_value_end(text, i, end);
            if (i < end && text[i] != ',') {
                i++;
            }
            continue;
        }

        i++;
        while (i < end && isspace((unsigned char)text[i])) {
            i++;
        }
        size_t value_start = i;
        size_t value_end = scan_value_end(text, i, end);
        trim_span(text, &value_start, &value_end);

        if (key_length < SETTINGS_TEXT_LENGTH && value_start < value_end) {
            int slot = -1;
            for (int j = 0; j < *count; j++) {
                if (strlen(values[j].key) == key_length &&
                    memcmp(values[j].key, text + key_start, key_length) == 0) {
                    slot = j;
                    break;
                }
            }
            if (slot < 0 && *count < LEGACY_MAX_VALUES) {
                slot = (*count)++;
                memcpy(values[slot].key, text + key_start, key_length);
                values[slot].key[key_length] = '\0';
            }
            if (slot >= 0) {
                values[slot].start = value_start;
                values[slot].end = value_end;
            }
        }

        i = value_end;
        if (i < end && text[i] == ',') {
            i++;
        }
    }
}

/* Convert only named values in the old static call blocks. Raw value spans are
   copied unchanged, so lists and quoted strings keep their exact contents. */
static char *migrate_legacy_file(const AppDef *app, const char *text,
                                size_t length, size_t *out_length) {
    LegacyValueSpan values[LEGACY_MAX_VALUES];
    int count = 0;
    size_t scan = 0;
    size_t args_start = 0;
    size_t args_end = 0;
    while (find_call_from(text, length, app->call, scan,
                          &args_start, &args_end)) {
        collect_legacy_call_values(text, args_start, args_end, values, &count);
        scan = args_end + 1;
    }

    size_t capacity = length + (size_t)count *
                      (SETTINGS_TEXT_LENGTH + 8) + 128;
    char *converted = malloc(capacity);
    if (!converted) {
        return NULL;
    }
    size_t used = (size_t)snprintf(
        converted, capacity,
        "# Migrated from the legacy static settings format.\n");
    for (int i = 0; i < count; i++) {
        int written = snprintf(converted + used, capacity - used, "%s = ",
                               values[i].key);
        if (written < 0 || (size_t)written >= capacity - used) {
            free(converted);
            return NULL;
        }
        used += (size_t)written;
        size_t value_length = values[i].end - values[i].start;
        if (value_length >= capacity - used) {
            free(converted);
            return NULL;
        }
        memcpy(converted + used, text + values[i].start, value_length);
        used += value_length;
        converted[used++] = '\n';
        converted[used] = '\0';
    }
    *out_length = used;
    return converted;
}

/* --- writing --------------------------------------------------------------- */

/* One line a value is written as, when it is added to a file that did not name
   it. The form follows the row's style. */
static void render_added(char *out, unsigned int size, const AppDef *app,
                         const SettingDef *def, const SettingValue *value) {
    char rendered[SETTINGS_TEXT_LENGTH * 4 + 32];
    render_value(rendered, sizeof(rendered), def, value, app->capital_bools);
    SettingStyle style = row_style(app, def);
    const char *call = row_call(app, def);

    if (style == SETTING_CALL) {
        snprintf(out, size, "    %s=%s,\n", def->key, rendered);
    } else if (style == SETTING_FUNC) {
        snprintf(out, size, "\n%s.%s(%s)", call, def->key, rendered);
    } else if (style == SETTING_ASSIGN) {
        snprintf(out, size, "%s.%s = %s\n", call, def->key, rendered);
    } else {
        snprintf(out, size, "%s = %s\n", def->key, rendered);
    }
}

/* The whole file for a program that had none: the program's call around every
   value, so the result is a file the program can read. */
static char *build_new_file(const AppDef *app, const SettingValue *values,
                            size_t *out_length) {
    size_t capacity = 256;
    for (int i = 0; i < app->setting_count; i++) {
        capacity += SETTINGS_TEXT_LENGTH * 4 + 80;
    }
    char *buffer = malloc(capacity);
    if (!buffer) {
        return NULL;
    }
    buffer[0] = '\0';

    const char *header =
        "# Created by GnuChanSettings.\n"
        "# This is a settings file; it may also be edited by hand.\n";
    strncat(buffer, header, capacity - strlen(buffer) - 1);

    SettingStyle style = app->style;
    char line[SETTINGS_TEXT_LENGTH * 5];

    if (style == SETTING_CALL) {
        snprintf(line, sizeof(line), "%s(\n", app->call);
        strncat(buffer, line, capacity - strlen(buffer) - 1);
    } else if (style == SETTING_INI) {
        snprintf(line, sizeof(line), "[%s]\n", app->call);
        strncat(buffer, line, capacity - strlen(buffer) - 1);
    }
    for (int i = 0; i < app->setting_count; i++) {
        render_added(line, sizeof(line), app, &app->settings[i], &values[i]);
        strncat(buffer, line, capacity - strlen(buffer) - 1);
    }
    if (style == SETTING_CALL) {
        strncat(buffer, ")\n", capacity - strlen(buffer) - 1);
    }

    *out_length = strlen(buffer);
    return buffer;
}

/* Where a missing argument is inserted into a call block: just before the
   closing bracket of the last block. Returns the insertion point, or
   `(size_t)-1` when there is no such block. */
static size_t call_insert_point(const char *text, size_t length,
                                const char *call) {
    size_t scan = 0;
    size_t as = 0;
    size_t ae = 0;
    size_t last = (size_t)-1;
    while (find_call_from(text, length, call, scan, &as, &ae)) {
        last = ae;
        scan = ae + 1;
    }
    return last;
}

static size_t ini_insert_point(const char *text, size_t length,
                               const char *section) {
    size_t section_start = 0;
    size_t section_end = 0;
    return find_ini_section(text, length, section, &section_start, &section_end)
         ? section_end : length;
}

int settings_save(const AppDef *app, const SettingValue *values, char *reason) {
    reason[0] = '\0';
    char path[SETTINGS_TEXT_LENGTH * 2];
    settings_path(path, sizeof(path), app);
    if (!path[0]) {
        snprintf(reason, SETTINGS_TEXT_LENGTH,
                 "no home directory to write into");
        return -1;
    }

    size_t length = 0;
    char *text = read_file(path, &length);
    char *created = NULL;
    int migrated_legacy = 0;
    if (!text) {
        if (errno != ENOENT) {
            snprintf(reason, SETTINGS_TEXT_LENGTH,
                     "cannot read settings: %.180s", path);
            return -1;
        }

        char legacy_path[SETTINGS_TEXT_LENGTH * 2];
        snprintf(legacy_path, sizeof(legacy_path), "%s", path);
        char *suffix = strrchr(legacy_path, '.');
        char *legacy_text = NULL;
        if (suffix && strcmp(suffix, ".conf") == 0) {
            strcpy(suffix, ".py");
            size_t legacy_length = 0;
            legacy_text = read_file(legacy_path, &legacy_length);
            if (legacy_text) {
                created = migrate_legacy_file(app, legacy_text,
                                              legacy_length, &length);
                free(legacy_text);
                if (!created) {
                    snprintf(reason, SETTINGS_TEXT_LENGTH,
                             "cannot safely migrate legacy settings");
                    return -1;
                }
                text = created;
                migrated_legacy = 1;
            } else if (errno != ENOENT) {
                snprintf(reason, SETTINGS_TEXT_LENGTH,
                         "cannot read legacy settings: %.160s", legacy_path);
                return -1;
            }
        }

        if (!text) {
            /* No file yet: build one whole. */
            /* Make sure the directory chain exists so the file can be created. */
            {
                char dir[SETTINGS_TEXT_LENGTH * 2];
                snprintf(dir, sizeof(dir), "%s", path);
                char *last = strrchr(dir, '/');
                if (last) {
                    *last = '\0';
                    if (dir[0] && mkdir_parents(dir) != 0) {
                        snprintf(reason, SETTINGS_TEXT_LENGTH,
                                 "cannot create directory %.200s", dir);
                        return -1;
                    }
                }
            }
            created = build_new_file(app, values, &length);
            if (!created) {
                snprintf(reason, SETTINGS_TEXT_LENGTH, "out of memory");
                return -1;
            }
            text = created;
        }
    }

    if (row_style(app, app->settings) == SETTING_INI) {
        size_t section_start = 0;
        size_t section_end = 0;
        if (!find_ini_section(text, length, app->call,
                              &section_start, &section_end)) {
            size_t section_len = strlen(app->call);
            int needs_newline = length > 0 && text[length - 1] != '\n';
            size_t addition = (needs_newline ? 1 : 0) + section_len + 4;
            char *with_section = malloc(length + addition + 1);
            if (!with_section) {
                free(created);
                snprintf(reason, SETTINGS_TEXT_LENGTH, "out of memory");
                return -1;
            }
            memcpy(with_section, text, length);
            size_t offset = length;
            if (needs_newline) {
                with_section[offset++] = '\n';
            }
            with_section[offset++] = '\n';
            with_section[offset++] = '[';
            memcpy(with_section + offset, app->call, section_len);
            offset += section_len;
            with_section[offset++] = ']';
            with_section[offset++] = '\n';
            with_section[offset] = '\0';
            free(created);
            created = with_section;
            text = created;
            length = offset;
        }
    }

    Edit edits[SETTINGS_MAX_EDITS];
    int edit_count = 0;

    for (int i = 0; i < app->setting_count && edit_count < SETTINGS_MAX_EDITS;
         i++) {
        const SettingDef *def = &app->settings[i];
        size_t vs = 0;
        size_t ve = 0;
        if (locate(text, length, app, def, &vs, &ve)) {
            edits[edit_count].start = vs;
            edits[edit_count].end = ve;
            render_value(edits[edit_count].text,
                         sizeof(edits[edit_count].text), def, &values[i],
                         app->capital_bools);
            edit_count++;
        } else if (!created || migrated_legacy) {
            /* The file does not name this setting. It is added so a person who
               changed it in the panel gets it saved, not silently dropped. */
            size_t at = length;
            if (row_style(app, def) == SETTING_CALL) {
                size_t point = call_insert_point(text, length, row_call(app, def));
                if (point != (size_t)-1) {
                    at = point;
                }
            } else if (row_style(app, def) == SETTING_INI) {
                at = ini_insert_point(text, length, row_call(app, def));
            }
            edits[edit_count].start = at;
            edits[edit_count].end = at;
            char added[SETTINGS_TEXT_LENGTH * 4 + 32];
            render_added(added, sizeof(added), app, def, &values[i]);
            if (row_style(app, def) == SETTING_INI && at > 0 &&
                text[at - 1] != '\n') {
                snprintf(edits[edit_count].text,
                         sizeof(edits[edit_count].text), "\n%s", added);
            } else {
                snprintf(edits[edit_count].text,
                         sizeof(edits[edit_count].text), "%s", added);
            }
            edit_count++;
        }
    }

    /* Apply the edits in file order. An insertion and a replacement at the same
       point keep the order they were added, which is the order the rows are
       written in. */
    for (int a = 0; a < edit_count; a++) {
        for (int b = a + 1; b < edit_count; b++) {
            if (edits[b].start < edits[a].start) {
                Edit swap = edits[a];
                edits[a] = edits[b];
                edits[b] = swap;
            }
        }
    }

    size_t capacity = length + 64;
    for (int i = 0; i < edit_count; i++) {
        capacity += strlen(edits[i].text) + 4;
    }
    char *out = malloc(capacity);
    if (!out) {
        free(created);
        snprintf(reason, SETTINGS_TEXT_LENGTH, "out of memory");
        return -1;
    }

    size_t o = 0;
    size_t cursor = 0;
    for (int i = 0; i < edit_count; i++) {
        if (edits[i].start < cursor) {
            continue;
        }
        size_t gap = edits[i].start - cursor;
        memcpy(out + o, text + cursor, gap);
        o += gap;
        size_t t = strlen(edits[i].text);
        memcpy(out + o, edits[i].text, t);
        o += t;
        cursor = edits[i].end;
    }
    size_t rest = length - cursor;
    memcpy(out + o, text + cursor, rest);
    o += rest;
    out[o] = '\0';

    /* Write beside the target, then rename: a crash mid-write leaves the old
       file, never half of a new one. */
    char temporary[SETTINGS_TEXT_LENGTH * 2 + 8];
    snprintf(temporary, sizeof(temporary), "%s.new", path);
    FILE *file = fopen(temporary, "wb");
    if (!file) {
        free(out);
        free(created);
        snprintf(reason, SETTINGS_TEXT_LENGTH, "cannot write %.200s",
                 temporary);
        return -1;
    }
    size_t wrote = fwrite(out, 1, o, file);
    int close_ok = (fclose(file) == 0);
    free(out);
    free(created);
    if (wrote != o || !close_ok) {
        remove(temporary);
        snprintf(reason, SETTINGS_TEXT_LENGTH, "cannot finish writing %.200s",
                 path);
        return -1;
    }
    if (rename(temporary, path) != 0) {
        remove(temporary);
        snprintf(reason, SETTINGS_TEXT_LENGTH, "cannot replace %.200s", path);
        return -1;
    }
    return 0;
}
