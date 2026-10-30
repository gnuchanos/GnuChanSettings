 /*
 * settings_ui.c — the window's behaviour: categories, fields, and Save.
 *
 * This is the half of GnuChanSettings that answers what a person does. It owns
 * the window, reads the events, and keeps the values a person has typed in
 * memory until Save writes them. Nothing here draws: every pixel is put down
 * by settings_draw.c, which reads the state this file keeps. The two walk the
 * SAME geometry (settings_ui.h), so a click lands on the field drawn under it.
 *
 * The whole promise of the program is in two keys: typing changes a value on
 * screen and nothing else, and Save is what writes the file. A person can
 * therefore open the panel, change six things, dislike all six, close it, and
 * have changed nothing on disk.
 */
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>

#include "settings_draw.h"
#include "settings_ui.h"
#include "gnuchan_lang.h"

/* --- the geometry, computed in one place ----------------------------------- */

int settings_ui_category_y(int index) {
    return UI_HEADER_HEIGHT + index * UI_CATEGORY_HEIGHT;
}

int settings_ui_category_at(const SettingsUi *ui, int y) {
    if (y < UI_HEADER_HEIGHT) {
        return -1;
    }
    return (y - UI_HEADER_HEIGHT) / UI_CATEGORY_HEIGHT;
}

int settings_ui_visible_rows(const SettingsUi *ui) {
    int body = ui->window_height - UI_HEADER_HEIGHT - UI_FOOTER_HEIGHT;
    int rows = body / UI_ROW_HEIGHT;
    return rows > 0 ? rows : 1;
}

void settings_ui_row_field_rect(const SettingsUi *ui, int row,
                                int *x, int *y, int *width, int *height) {
    /* `row` is the absolute row in the list; the scroll decides which line of
       the body it is drawn on. This is the same subtraction draw_row() makes,
       and it has to be the same one or a click would land a row or two away
       from the field it was aimed at once the list has been scrolled. */
    int shown = row - ui->scroll;
    int row_y = UI_HEADER_HEIGHT + shown * UI_ROW_HEIGHT;
    int content_width = ui->window_width - UI_SIDEBAR_WIDTH -
                        2 * UI_MARGIN;
    int field_width = content_width / 2;
    if (field_width < UI_FIELD_WIDTH) {
        field_width = UI_FIELD_WIDTH;
    }
    int max_field_width = content_width - 180;
    if (field_width > max_field_width) {
        field_width = max_field_width;
    }
    if (field_width < 1) {
        field_width = 1;
    }
    *x = ui->window_width - UI_MARGIN - field_width;
    *y = row_y + 8;
    *width = field_width;
    *height = UI_ROW_HEIGHT - 16;
}

int settings_ui_row_at(const SettingsUi *ui, int x, int y) {
    if (x < UI_SIDEBAR_WIDTH) {
        return -1;                        /* the sidebar, not the body */
    }
    int body_top = UI_HEADER_HEIGHT;
    int body_bottom = ui->window_height - UI_FOOTER_HEIGHT;
    if (y < body_top || y >= body_bottom) {
        return -1;
    }
    int row = (y - body_top) / UI_ROW_HEIGHT;
    return ui->scroll + row;
}

static int list_item_count(const char *text) {
    int count = 0;
    const char *entry = text;
    while (*entry) {
        const char *end = strchr(entry, ';');
        size_t length = end ? (size_t)(end - entry) : strlen(entry);
        while (length > 0 && isspace((unsigned char)entry[length - 1])) {
            length--;
        }
        while (length > 0 && isspace((unsigned char)*entry)) {
            entry++;
            length--;
        }
        if (length > 0) {
            count++;
        }
        if (!end) {
            break;
        }
        entry = end + 1;
    }
    return count;
}

static int list_item_bounds(const char *text, int wanted,
                            size_t *item_start, size_t *item_end) {
    size_t start = 0;
    int item = 0;
    size_t text_length = strlen(text);
    while (start < text_length) {
        size_t end = start;
        while (end < text_length && text[end] != ';') {
            end++;
        }
        size_t visible_start = start;
        size_t visible_end = end;
        while (visible_start < visible_end &&
               isspace((unsigned char)text[visible_start])) {
            visible_start++;
        }
        while (visible_end > visible_start &&
               isspace((unsigned char)text[visible_end - 1])) {
            visible_end--;
        }
        if (visible_start < visible_end) {
            if (item == wanted) {
                *item_start = start;
                *item_end = end;
                return 1;
            }
            item++;
        }
        if (end == text_length) {
            break;
        }
        start = end + 1;
    }
    return 0;
}

static size_t utf8_previous_offset(const char *text, size_t offset) {
    if (offset > 0) {
        offset--;
        while (offset > 0 &&
               ((unsigned char)text[offset] & 0xc0) == 0x80) {
            offset--;
        }
    }
    return offset;
}

static size_t utf8_next_offset(const char *text, size_t offset) {
    size_t length = strlen(text);
    if (offset < length) {
        offset++;
        while (offset < length &&
               ((unsigned char)text[offset] & 0xc0) == 0x80) {
            offset++;
        }
    }
    return offset;
}

void settings_ui_save_rect(const SettingsUi *ui, int *x, int *y,
                           int *width, int *height) {
    (void)ui;
    *width = 130;
    *height = UI_FOOTER_HEIGHT - 24;
    *x = ui->window_width - UI_MARGIN - *width;
    *y = ui->window_height - UI_FOOTER_HEIGHT + 12;
}

void settings_ui_revert_rect(const SettingsUi *ui, int *x, int *y,
                             int *width, int *height) {
    settings_ui_save_rect(ui, x, y, width, height);
    *x -= (*width + 12);
}

/* The dropdown hangs directly under the field it belongs to, one name per
   UI_CHOICE_HEIGHT. Both the drawing and the hit-testing come through here, so
   the name a click lands on is the name that was drawn. */
void settings_ui_choice_rect(const SettingsUi *ui, int row, int index,
                             int *x, int *y, int *width, int *height) {
    int fx, fy, fw, fh;
    settings_ui_row_field_rect(ui, row, &fx, &fy, &fw, &fh);
    *x = fx;
    *y = fy + fh + index * UI_CHOICE_HEIGHT;
    *width = fw;
    *height = UI_CHOICE_HEIGHT;
}

int settings_ui_choice_at(const SettingsUi *ui, int x, int y) {
    if (ui->dropdown_row < 0 || !ui->page.app ||
        ui->dropdown_row >= ui->page.app->setting_count) {
        return -1;
    }
    int count =
        settings_choice_count(&ui->page.app->settings[ui->dropdown_row]);
    if (count <= 0) {
        return -1;
    }
    int fx, fy, fw, fh;
    settings_ui_row_field_rect(ui, ui->dropdown_row, &fx, &fy, &fw, &fh);
    int list_top = fy + fh;
    if (x < fx || x >= fx + fw || y < list_top) {
        return -1;
    }
    int index = (y - list_top) / UI_CHOICE_HEIGHT;
    if (index < 0 || index >= count) {
        return -1;
    }
    return index;
}

/* --- loading a category ---------------------------------------------------- */

void settings_ui_load_page(SettingsUi *ui) {
    ui->page.app = &ui->apps[ui->app_index];
    ui->page.loaded = settings_load(ui->page.app, ui->page.values);
    ui->focus_row = UI_FOCUS_NONE;
    ui->hover_row = -1;
    ui->dropdown_row = -1;
    ui->list_selected_row = -1;
    ui->list_selected_item = -1;
    ui->list_editing = 0;
    ui->list_editor_open = 0;
    ui->list_scroll = 0;
    ui->scroll = 0;
    ui->status[0] = '\0';
    ui->status_failed = 0;

    if (ui->page.loaded == 1) {
        snprintf(ui->status, sizeof(ui->status),
                 gnuchan_lang_t("status_loaded"), ui->page.app->name);
    } else {
        snprintf(ui->status, sizeof(ui->status),
                 gnuchan_lang_t("status_no_file"), ui->page.app->name);
    }
    ui->status_failed = 0;
}

/* --- acting on the page ---------------------------------------------------- */

static void switch_category(SettingsUi *ui, int index) {
    if (index < 0 || index >= ui->app_count) {
        return;
    }
    ui->app_index = index;
    settings_ui_load_page(ui);
}

static int apply_system_settings(SettingsUi *ui) {
    if (strcmp(ui->page.app->dir, "GnuChanSettings") != 0 ||
        strcmp(ui->page.app->file, "system.conf") != 0) {
        return 0;
    }

    const SettingValue *values = ui->page.values;
    pid_t child = fork();
    if (child < 0) {
        return -1;
    }
    if (child == 0) {
        execlp("pkexec", "pkexec", "/usr/local/libexec/gnuchan-settings-helper",
               "apply", values[1].text, values[2].text, values[3].text,
               (char *)NULL);
        _exit(127);
    }

    int status;
    while (waitpid(child, &status, 0) < 0) {
        if (errno != EINTR) {
            return -1;
        }
    }
    return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : -1;
}

static void save_page(SettingsUi *ui) {
    if (strcmp(ui->page.app->dir, "GnuChanSettings") == 0 &&
        strcmp(ui->page.app->file, "system.conf") == 0) {
        char *end = NULL;
        errno = 0;
        long threshold = strtol(ui->page.values[0].text, &end, 10);
        if (errno != 0 || end == ui->page.values[0].text || *end != '\0' ||
            threshold < 1 || threshold > 100) {
            snprintf(ui->status, sizeof(ui->status),
                     "Battery warning must be between 1 and 100.");
            ui->status_failed = 1;
            return;
        }
    }

    char reason[SETTINGS_TEXT_LENGTH];
    if (settings_save(ui->page.app, ui->page.values, reason) == 0) {
        if (apply_system_settings(ui) != 0) {
            snprintf(ui->status, sizeof(ui->status),
                     "Settings saved, but system changes were not applied.");
            ui->status_failed = 1;
            return;
        }
        snprintf(ui->status, sizeof(ui->status), gnuchan_lang_t("status_saved"),
                 ui->page.app->name);
        ui->status_failed = 0;
    } else {
        snprintf(ui->status, sizeof(ui->status),
                 gnuchan_lang_t("status_save_failed"), reason);
        ui->status_failed = 1;
    }
}

/* --- the keyboard ---------------------------------------------------------- */

/* Put a character into the focused field, if that field takes text. A number
   field takes only digits and a minus or a dot; a switch takes no typing at
   all — it is toggled, not typed. */
static int field_accepts_char(const SettingDef *def, char c) {
    if (def->type == SETTING_INT || def->type == SETTING_REAL) {
        if (isdigit((unsigned char)c)) {
            return 1;
        }
        return c == '-' || (def->type == SETTING_REAL && c == '.');
    }
    if (def->type == SETTING_LIST && c == ';') {
        return 0;
    }
    return 1;      /* a colour and a text take anything the person types */
}

static void field_insert(SettingsUi *ui, const char *text) {
    if (ui->focus_row < 0 || ui->focus_row >= ui->page.app->setting_count) {
        return;
    }
    const SettingDef *def = &ui->page.app->settings[ui->focus_row];
    /* A switch is toggled and a choice is chosen from its list; neither is
       typed into, so a keystroke must not land in either. */
    if (def->type == SETTING_BOOL || def->type == SETTING_CHOICE) {
        return;
    }
    if (def->type == SETTING_LIST && !ui->list_editing) {
        return;
    }
    SettingValue *value = &ui->page.values[ui->focus_row];

    if (def->type == SETTING_LIST) {
        int entries = list_item_count(value->text);
        size_t start = 0;
        size_t end = 0;
        int adding = ui->list_selected_item == entries;
        if (!adding &&
            !list_item_bounds(value->text, ui->list_selected_item,
                              &start, &end)) {
            return;
        }
        size_t length = strlen(value->text);
        char accepted[8];
        size_t accepted_length = 0;
        for (const char *p = text; *p; p++) {
            if (field_accepts_char(def, *p) &&
                accepted_length < sizeof(accepted)) {
                accepted[accepted_length++] = *p;
            }
        }
        if (accepted_length == 0) {
            return;
        }
        int separator = adding && length > 0 &&
                        value->text[length - 1] != ';';
        if (length + accepted_length + (size_t)separator >=
            sizeof(value->text)) {
            return;
        }
        if (adding) {
            if (separator) {
                value->text[length++] = ';';
                value->text[length] = '\0';
            }
            start = end = length;
        }
        size_t item_length = end - start;
        if (ui->caret < 0 || (size_t)ui->caret > item_length) {
            ui->caret = (int)item_length;
        }
        size_t insert_at = start + (size_t)ui->caret;
        memmove(value->text + insert_at + accepted_length,
                value->text + insert_at,
                length - insert_at + 1);
        memcpy(value->text + insert_at, accepted, accepted_length);
        ui->caret += (int)accepted_length;
        return;
    }

    size_t length = strlen(value->text);
    for (const char *p = text; *p; p++) {
        if (!field_accepts_char(def, *p)) {
            continue;
        }
        if (length + 1 >= sizeof(value->text)) {
            break;
        }
        value->text[length++] = *p;
        value->text[length] = '\0';
    }
    ui->caret = (int)length;
}

static void field_backspace(SettingsUi *ui) {
    if (ui->focus_row < 0) {
        return;
    }
    if (ui->page.app->settings[ui->focus_row].type == SETTING_LIST &&
        !ui->list_editing) {
        return;
    }
    SettingValue *value = &ui->page.values[ui->focus_row];
    if (ui->page.app->settings[ui->focus_row].type == SETTING_LIST) {
        int entries = list_item_count(value->text);
        if (ui->list_selected_item < 0 ||
            ui->list_selected_item >= entries || ui->caret <= 0) {
            return;
        }
        size_t start = 0;
        size_t end = 0;
        if (!list_item_bounds(value->text, ui->list_selected_item,
                              &start, &end)) {
            return;
        }
        size_t item_length = end - start;
        if ((size_t)ui->caret > item_length) {
            ui->caret = (int)item_length;
        }
        size_t previous = utf8_previous_offset(value->text + start,
                                               (size_t)ui->caret);
        size_t remove_at = start + previous;
        size_t remove_end = start + (size_t)ui->caret;
        memmove(value->text + remove_at, value->text + remove_end,
                strlen(value->text + remove_end) + 1);
        ui->caret = (int)previous;
        return;
    }
    size_t length = strlen(value->text);
    if (length == 0) {
        return;
    }
    value->text[length - 1] = '\0';
    ui->caret = (int)(length - 1);
}

static void toggle_bool(SettingsUi *ui, int row) {
    if (row < 0 || row >= ui->page.app->setting_count) {
        return;
    }
    if (ui->page.app->settings[row].type != SETTING_BOOL) {
        return;
    }
    SettingValue *value = &ui->page.values[row];
    int flag = setting_value_bool(value) ? 0 : 1;
    setting_value_set_bool(value, flag, ui->page.app->capital_bools);
}

static void add_list_item(SettingsUi *ui, int row);
static void remove_selected_list_item(SettingsUi *ui, int row);

static void edit_list_button(SettingsUi *ui, int row, int x) {
    int fx, fy, fw, fh;
    settings_ui_row_field_rect(ui, row, &fx, &fy, &fw, &fh);
    (void)fy;
    (void)fh;
    const int button_width = 32;
    const int gap = 6;
    int input_width = fw - button_width * 2 - gap * 2;
    int plus_x = fx + input_width + gap;
    int minus_x = plus_x + button_width + gap;
    int selected = ui->list_selected_row == row
                       ? ui->list_selected_item : -1;
    ui->list_editor_open = 1;
    ui->list_scroll = 0;
    ui->list_selected_row = row;
    if (x >= plus_x && x < plus_x + button_width) {
        ui->list_selected_item = -1;
        ui->list_editing = 0;
        ui->focus_row = UI_FOCUS_NONE;
        add_list_item(ui, row);
    } else if (x >= minus_x && x < minus_x + button_width) {
        ui->list_selected_item = selected;
        ui->list_editing = selected >= 0;
        ui->focus_row = UI_FOCUS_NONE;
        remove_selected_list_item(ui, row);
    } else {
        ui->list_selected_item = -1;
        ui->list_editing = 0;
        ui->focus_row = UI_FOCUS_NONE;
    }
}

static int list_editor_geometry(const SettingsUi *ui, int *x, int *y,
                                int *width, int *height, int *visible) {
    *x = UI_SIDEBAR_WIDTH + UI_MARGIN;
    *y = UI_HEADER_HEIGHT + UI_MARGIN;
    *width = ui->window_width - *x - UI_MARGIN;
    *height = ui->window_height - UI_FOOTER_HEIGHT - *y - UI_MARGIN;
    *visible = (*height - UI_LIST_EDITOR_HEADER - UI_LIST_EDITOR_FOOTER) /
               UI_LIST_EDITOR_ROW_HEIGHT;
    return *width > 0 && *height > UI_LIST_EDITOR_HEADER +
                                      UI_LIST_EDITOR_FOOTER &&
           *visible > 0;
}

static void add_list_item(SettingsUi *ui, int row) {
    SettingValue *value = &ui->page.values[row];
    int entries = list_item_count(value->text);
    if (ui->list_editing && ui->list_selected_item == entries) {
        return;
    }
    if (entries >= SETTINGS_MAX_LIST_ITEMS) {
        snprintf(ui->status, sizeof(ui->status),
                 gnuchan_lang_t("status_list_limit"), SETTINGS_MAX_LIST_ITEMS);
        ui->status_failed = 1;
        return;
    }
    size_t length = strlen(value->text);
    size_t extra = length > 0 && value->text[length - 1] != ';' ? 3 : 2;
    if (length + extra > sizeof(value->text)) {
        snprintf(ui->status, sizeof(ui->status),
                 gnuchan_lang_t("status_list_full"));
        ui->status_failed = 1;
        return;
    }
    ui->list_selected_row = row;
    ui->list_selected_item = entries;
    ui->list_editing = 1;
    ui->focus_row = row;
    ui->caret = 0;
    int left, top, width, height, visible;
    if (list_editor_geometry(ui, &left, &top, &width, &height, &visible)) {
        int maximum = entries + 1 - visible;
        ui->list_scroll = maximum > 0 ? maximum : 0;
    }
    (void)left;
    (void)top;
    (void)width;
    (void)height;
}

static void remove_selected_list_item(SettingsUi *ui, int row) {
    SettingValue *value = &ui->page.values[row];
    int entries = list_item_count(value->text);
    int selected = ui->list_selected_row == row
                       ? ui->list_selected_item : -1;
    if (selected == entries && ui->list_editing) {
        ui->list_selected_item = -1;
        ui->list_editing = 0;
        ui->focus_row = UI_FOCUS_NONE;
        ui->list_scroll = 0;
        return;
    }
    if (selected < 0 || selected >= entries) {
        return;
    }
    size_t start = 0;
    size_t end = 0;
    size_t length = strlen(value->text);
    if (!list_item_bounds(value->text, selected, &start, &end)) {
        return;
    }
    if (end < length && value->text[end] == ';') {
        end++;
    } else if (start > 0) {
        start--;
    }
    memmove(value->text + start, value->text + end, length - end + 1);
    ui->list_selected_item = -1;
    ui->list_editing = 0;
    ui->focus_row = UI_FOCUS_NONE;
    ui->list_scroll = 0;
}

static void handle_list_editor_click(SettingsUi *ui, int x, int y) {
    int left, top, width, height, visible;
    if (!list_editor_geometry(ui, &left, &top, &width, &height, &visible)) {
        ui->list_editor_open = 0;
        return;
    }
    int row = ui->list_selected_row;
    int entries = list_item_count(ui->page.values[row].text);
    int list_top = top + UI_LIST_EDITOR_HEADER;
    int list_bottom = top + height - UI_LIST_EDITOR_FOOTER;
    int button_y = top + height - UI_LIST_EDITOR_FOOTER + 6;
    int button_width = 72;
    int gap = 8;
    int done_x = left + width - 12 - button_width;
    int remove_x = done_x - gap - button_width;
    int add_x = remove_x - gap - button_width;

    if (y >= button_y && y < button_y + 34) {
        if (x >= add_x && x < add_x + button_width) {
            add_list_item(ui, row);
        } else if (x >= remove_x && x < remove_x + button_width) {
            remove_selected_list_item(ui, row);
        } else if (x >= done_x && x < done_x + button_width) {
            ui->list_editor_open = 0;
            ui->list_editing = 0;
            ui->focus_row = UI_FOCUS_NONE;
        }
        return;
    }

    if (x >= left && x < left + width && y >= list_top &&
        y < list_bottom) {
        int index = ui->list_scroll +
                    (y - list_top) / UI_LIST_EDITOR_ROW_HEIGHT;
        if (index >= entries && index == entries &&
            ui->list_editing && ui->list_selected_item == entries) {
            ui->caret = 0;
        } else if (index >= 0 && index < entries) {
            ui->list_selected_item = index;
            ui->list_editing = 1;
            ui->focus_row = row;
            size_t start = 0;
            size_t end = 0;
            if (list_item_bounds(ui->page.values[row].text, index,
                                 &start, &end)) {
                ui->caret = (int)(end - start);
            }
        }
        return;
    }
    (void)visible;
    ui->list_editor_open = 0;
    ui->list_editing = 0;
    ui->focus_row = UI_FOCUS_NONE;
}

static void handle_key(SettingsUi *ui, XKeyEvent *key) {
    KeySym symbol = XLookupKeysym(key, 0);
    if (symbol == NoSymbol) {
        return;
    }

    switch (symbol) {
    case XK_Escape:
        if (ui->list_editor_open) {
            ui->list_editor_open = 0;
            ui->list_editing = 0;
            ui->focus_row = UI_FOCUS_NONE;
            return;
        }
        /* Escape puts an open list away first, and only closes the panel when
           there is no list to close. */
        if (ui->dropdown_row >= 0) {
            ui->dropdown_row = -1;
            return;
        }
        ui->running = 0;
        return;
    case XK_BackSpace:
        field_backspace(ui);
        return;
    case XK_Left:
        if (ui->list_editor_open && ui->list_editing && ui->caret > 0) {
            size_t start = 0;
            size_t end = 0;
            if (ui->list_selected_item <
                    list_item_count(ui->page.values[ui->focus_row].text) &&
                list_item_bounds(ui->page.values[ui->focus_row].text,
                                 ui->list_selected_item, &start, &end)) {
                ui->caret = (int)utf8_previous_offset(
                    ui->page.values[ui->focus_row].text + start,
                    (size_t)ui->caret);
            }
        }
        return;
    case XK_Right:
        if (ui->list_editor_open && ui->list_editing &&
            ui->focus_row >= 0) {
            int row = ui->focus_row;
            size_t start = 0;
            size_t end = 0;
            if (ui->list_selected_item <
                    list_item_count(ui->page.values[row].text) &&
                list_item_bounds(ui->page.values[row].text,
                                 ui->list_selected_item, &start, &end) &&
                (size_t)ui->caret < end - start) {
                ui->caret = (int)utf8_next_offset(
                    ui->page.values[row].text + start, (size_t)ui->caret);
            }
        }
        return;
    case XK_Home:
        if (ui->list_editor_open && ui->list_editing) {
            ui->caret = 0;
        }
        return;
    case XK_End:
        if (ui->list_editor_open && ui->list_editing &&
            ui->focus_row >= 0) {
            size_t start = 0;
            size_t end = 0;
            if (ui->list_selected_item <
                    list_item_count(ui->page.values[ui->focus_row].text) &&
                list_item_bounds(ui->page.values[ui->focus_row].text,
                                 ui->list_selected_item, &start, &end)) {
                ui->caret = (int)(end - start);
            }
        }
        return;
    case XK_Tab:
        if (ui->list_editor_open) {
            return;
        }
        if (ui->page.app->setting_count > 0) {
            ui->focus_row = (ui->focus_row + 1) % ui->page.app->setting_count;
            ui->caret = (int)strlen(ui->page.values[ui->focus_row].text);
        }
        return;
    default:
        break;
    }

    char text[8];
    int length = XLookupString(key, text, sizeof(text) - 1, NULL, NULL);
    if (length <= 0) {
        return;
    }
    text[length] = '\0';
    for (int i = 0; i < length; i++) {
        if ((unsigned char)text[i] < 0x20) {
            return;
        }
    }
    field_insert(ui, text);
}

/* --- the pointer ----------------------------------------------------------- */

static void handle_click(SettingsUi *ui, XButtonEvent *press) {
    if (press->button != Button1) {
        return;
    }
    int x = press->x;
    int y = press->y;

    if (ui->list_editor_open) {
        handle_list_editor_click(ui, x, y);
        return;
    }

    int bx, by, bw, bh;
    settings_ui_save_rect(ui, &bx, &by, &bw, &bh);
    if (x >= bx && x < bx + bw && y >= by && y < by + bh) {
        save_page(ui);
        return;
    }
    settings_ui_revert_rect(ui, &bx, &by, &bw, &bh);
    if (x >= bx && x < bx + bw && y >= by && y < by + bh) {
        settings_ui_load_page(ui);
        return;
    }

    if (x < UI_SIDEBAR_WIDTH) {
        int category = settings_ui_category_at(ui, y);
        if (category >= 0 && category < ui->app_count) {
            switch_category(ui, category);
        }
        return;
    }

    /* An open dropdown answers the click first: a name in it is chosen, and a
       click anywhere else puts the list away before the click is read again. */
    if (ui->dropdown_row >= 0) {
        int choice = settings_ui_choice_at(ui, x, y);
        if (choice >= 0) {
            const SettingDef *def = &ui->page.app->settings[ui->dropdown_row];
            const char *const *list = settings_choices(def);
            if (list && list[choice]) {
                snprintf(ui->page.values[ui->dropdown_row].text,
                         sizeof(ui->page.values[ui->dropdown_row].text),
                         "%s", list[choice]);
            }
            ui->dropdown_row = -1;
            return;
        }
        ui->dropdown_row = -1;
        /* Fall through: the click may still be on a field. */
    }

    /* The body: a row. A click on a switch toggles it, a click on a choice
       opens its list, and a click on any other field puts the keyboard in it. */
    int row = settings_ui_row_at(ui, x, y);
    if (row < 0 || row >= ui->page.app->setting_count) {
        ui->focus_row = UI_FOCUS_NONE;
        ui->list_selected_row = -1;
        ui->list_selected_item = -1;
        ui->list_editing = 0;
        return;
    }
    const SettingDef *def = &ui->page.app->settings[row];
    ui->focus_row = row;
    ui->caret = (int)strlen(ui->page.values[row].text);
    if (def->type == SETTING_BOOL) {
        toggle_bool(ui, row);
    } else if (def->type == SETTING_CHOICE) {
        ui->dropdown_row = row;
    } else if (def->type == SETTING_LIST) {
        edit_list_button(ui, row, x);
        return;
    } else {
        ui->list_selected_row = -1;
        ui->list_selected_item = -1;
        ui->list_editing = 0;
    }
}

static void handle_motion(SettingsUi *ui, XMotionEvent *motion) {
    int row = settings_ui_row_at(ui, motion->x, motion->y);
    int category = motion->x < UI_SIDEBAR_WIDTH
                       ? settings_ui_category_at(ui, motion->y)
                       : -1;
    if (row != ui->hover_row || category != ui->hover_category) {
        ui->hover_row = row;
        ui->hover_category = category;
        settings_draw(ui);
    }
}

/* The wheel over the body scrolls the rows, kept inside the list so a person
   cannot scroll past the last setting into empty space. */
static void handle_wheel(SettingsUi *ui, XButtonEvent *press) {
    if (ui->list_editor_open) {
        int left, top, width, height, visible;
        if (!list_editor_geometry(ui, &left, &top, &width, &height,
                                  &visible)) {
            return;
        }
        int entries =
            list_item_count(ui->page.values[ui->list_selected_row].text);
        int total = entries +
                    (ui->list_editing &&
                             ui->list_selected_item == entries
                         ? 1 : 0);
        int maximum = total - visible;
        if (maximum < 0) {
            maximum = 0;
        }
        if (press->button == Button4) {
            ui->list_scroll--;
        } else if (press->button == Button5) {
            ui->list_scroll++;
        } else {
            return;
        }
        if (ui->list_scroll < 0) {
            ui->list_scroll = 0;
        } else if (ui->list_scroll > maximum) {
            ui->list_scroll = maximum;
        }
        (void)left;
        (void)top;
        (void)width;
        (void)height;
        return;
    }
    int rows = ui->page.app->setting_count;
    int visible = settings_ui_visible_rows(ui);
    int maximum = rows - visible;
    if (maximum < 0) {
        maximum = 0;
    }
    if (press->button == Button4) {
        ui->scroll--;
    } else if (press->button == Button5) {
        ui->scroll++;
    } else {
        return;
    }
    if (ui->scroll < 0) {
        ui->scroll = 0;
    }
    if (ui->scroll > maximum) {
        ui->scroll = maximum;
    }
    settings_draw(ui);
}

/* --- opening and closing --------------------------------------------------- */

int settings_ui_open(SettingsUi *ui) {
    memset(ui, 0, sizeof(*ui));
    ui->focus_row = UI_FOCUS_NONE;
    ui->hover_row = -1;
    ui->hover_category = -1;
    ui->app_index = 0;

    ui->display = XOpenDisplay(NULL);
    if (!ui->display) {
        fprintf(stderr, "gnuchansettings: cannot open the X display\n");
        return -1;
    }
    ui->screen = DefaultScreen(ui->display);
    ui->root = RootWindow(ui->display, ui->screen);
    ui->visual = DefaultVisual(ui->display, ui->screen);
    ui->colormap = DefaultColormap(ui->display, ui->screen);
    ui->depth = DefaultDepth(ui->display, ui->screen);

    if (settings_style_load(&ui->style, ui->display, ui->screen) != 0) {
        fprintf(stderr, "gnuchansettings: cannot load the fonts\n");
        XCloseDisplay(ui->display);
        ui->display = NULL;
        return -1;
    }

    ui->apps = settings_apps(&ui->app_count);
    ui->gc = XCreateGC(ui->display, ui->root, 0, NULL);
    if (!ui->gc) {
        fprintf(stderr, "gnuchansettings: cannot create the graphics context\n");
        settings_ui_close(ui);
        return -1;
    }

    XSetWindowAttributes attributes;
    memset(&attributes, 0, sizeof(attributes));
    attributes.background_pixel = ui->style.background;
    attributes.border_pixel = ui->style.panel_edge;
    attributes.event_mask = ExposureMask | KeyPressMask | ButtonPressMask |
                            PointerMotionMask | StructureNotifyMask;
    ui->window = XCreateWindow(ui->display, ui->root, 0, 0,
                               UI_WINDOW_WIDTH, UI_WINDOW_HEIGHT, 0,
                               ui->depth, InputOutput, ui->visual,
                               CWBackPixel | CWBorderPixel | CWEventMask,
                               &attributes);
    if (ui->window == None) {
        fprintf(stderr, "gnuchansettings: cannot make the window\n");
        settings_ui_close(ui);
        return -1;
    }
    ui->window_width = UI_WINDOW_WIDTH;
    ui->window_height = UI_WINDOW_HEIGHT;
    XSizeHints size_hints;
    memset(&size_hints, 0, sizeof(size_hints));
    size_hints.flags = PMinSize;
    size_hints.min_width = UI_SIDEBAR_WIDTH + UI_FIELD_WIDTH +
                           2 * UI_MARGIN + 180;
    size_hints.min_height = UI_HEADER_HEIGHT + UI_FOOTER_HEIGHT +
                            UI_ROW_HEIGHT;
    int category_height = UI_HEADER_HEIGHT +
                          ui->app_count * UI_CATEGORY_HEIGHT;
    if (size_hints.min_height < category_height) {
        size_hints.min_height = category_height;
    }
    XSetWMNormalHints(ui->display, ui->window, &size_hints);
    if (!XStoreName(ui->display, ui->window, "GnuChanSettings")) {
        fprintf(stderr, "gnuchansettings: cannot set the window title\n");
        settings_ui_close(ui);
        return -1;
    }

    /* The window's class, which is how the dock knows what this program is.
       It reads WM_CLASS to tell one program's windows from another's and to
       gather them — see dock_items.c — and a window that set none is a window
       with no name to gather by. The instance is the name a single window is
       started under and the class the program's own name, exactly as the
       terminal sets them, so the dock can collect every GnuChanSettings window
       into its one settings slot instead of drawing each as another icon. */
    {
        XClassHint hint;
        hint.res_name = (char *)"gcl_settings";
        hint.res_class = (char *)"GnuChanSettings";
        if (!XSetClassHint(ui->display, ui->window, &hint)) {
            fprintf(stderr, "gnuchansettings: cannot set the window class\n");
            settings_ui_close(ui);
            return -1;
        }
    }

    /* The whole panel is painted into this pixmap and copied to the window in
       ONE XCopyArea, rather than drawn straight onto the window a shape at a
       time. That is what keeps the redraw from being seen: a window painted
       piece by piece shows each piece as it lands — the flicker — while a
       window shown a finished picture never does. */
    ui->buffer = XCreatePixmap(ui->display, ui->window, UI_WINDOW_WIDTH,
                               UI_WINDOW_HEIGHT, (unsigned int)ui->depth);
    if (ui->buffer == None) {
        fprintf(stderr, "gnuchansettings: cannot make the back buffer\n");
        settings_ui_close(ui);
        return -1;
    }
    ui->target = ui->buffer;
    ui->draw = XftDrawCreate(ui->display, ui->buffer, ui->visual,
                             ui->colormap);
    if (!ui->draw) {
        fprintf(stderr, "gnuchansettings: cannot create the text drawable\n");
        settings_ui_close(ui);
        return -1;
    }

    settings_ui_load_page(ui);

    if (!XMapWindow(ui->display, ui->window)) {
        fprintf(stderr, "gnuchansettings: cannot map the window\n");
        settings_ui_close(ui);
        return -1;
    }
    XFlush(ui->display);
    return 0;
}

void settings_ui_run(SettingsUi *ui) {
    ui->running = 1;
    while (ui->running) {
        XEvent event;
        XNextEvent(ui->display, &event);
        int redraw = 0;

        switch (event.type) {
        case Expose:
            if (event.xexpose.count == 0) {
                redraw = 1;
            }
            break;
        case KeyPress:
            handle_key(ui, &event.xkey);
            redraw = 1;
            break;
        case ButtonPress:
            handle_wheel(ui, &event.xbutton);
            if (event.xbutton.button == Button1) {
                handle_click(ui, &event.xbutton);
            }
            redraw = 1;
            break;
        case MotionNotify:
            handle_motion(ui, &event.xmotion);
            break;
        case ConfigureNotify: {
            int w = event.xconfigure.width;
            int h = event.xconfigure.height;
            if (w > 0 && h > 0 &&
                (w != ui->window_width || h != ui->window_height)) {
                ui->window_width = w;
                ui->window_height = h;
                int visible = settings_ui_visible_rows(ui);
                int maximum = ui->page.app
                                  ? ui->page.app->setting_count - visible : 0;
                if (maximum < 0) {
                    maximum = 0;
                }
                if (ui->scroll > maximum) {
                    ui->scroll = maximum;
                }
                /* Recreate the back buffer at the new size so the panel
                   redraws to fill the window instead of leaving empty space. */
                if (ui->buffer != None) {
                    XFreePixmap(ui->display, ui->buffer);
                    ui->buffer = None;
                }
                ui->buffer = XCreatePixmap(ui->display, ui->window,
                                           (unsigned int)w, (unsigned int)h,
                                           (unsigned int)ui->depth);
                if (ui->buffer != None) {
                    if (ui->draw) {
                        XftDrawDestroy(ui->draw);
                    }
                    ui->draw = XftDrawCreate(ui->display, ui->buffer,
                                             ui->visual, ui->colormap);
                    ui->target = ui->buffer;
                } else {
                    ui->draw = NULL;
                    ui->target = ui->window;
                }
            }
            redraw = 1;
            break;
        }
        case DestroyNotify:
            ui->running = 0;
            break;
        default:
            break;
        }

        if (redraw) {
            settings_draw(ui);
        }
    }
}

void settings_ui_close(SettingsUi *ui) {
    if (!ui->display) {
        return;
    }
    if (ui->draw) {
        XftDrawDestroy(ui->draw);
        ui->draw = NULL;
    }
    if (ui->buffer != None) {
        XFreePixmap(ui->display, ui->buffer);
        ui->buffer = None;
    }
    if (ui->window != None) {
        XDestroyWindow(ui->display, ui->window);
        ui->window = None;
    }
    if (ui->gc) {
        XFreeGC(ui->display, ui->gc);
        ui->gc = NULL;
    }
    settings_style_free(&ui->style, ui->display);
    XCloseDisplay(ui->display);
    ui->display = NULL;
}
