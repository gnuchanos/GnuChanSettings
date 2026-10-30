/*
 * settings_draw.c — the whole panel, painted from the state.
 *
 * One function, because the painting is one act: the sidebar, the header, the
 * rows of the open category and the footer all go down in one pass from the
 * state settings_ui.c owns. The geometry it walks is the same geometry the
 * clicks are tested against (settings_ui.h), which is what makes the field
 * under the pointer the field that answers it.
 *
 * The text is drawn through Xft, the same as the dock's labels and the window
 * manager's title bars, so the panel matches the rest of the session. A pixel
 * the style resolved is turned back into the XftColor the text needs through
 * XQueryColor.
 */
#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include <X11/Xft/Xft.h>

#include "settings_draw.h"
#include "gnuchan_lang.h"

/* --- small drawing helpers ------------------------------------------------- */

static void fill_rect(SettingsUi *ui, unsigned long colour,
                      int x, int y, int width, int height) {
    if (width <= 0 || height <= 0) {
        return;
    }
    XSetForeground(ui->display, ui->gc, colour);
    XFillRectangle(ui->display, ui->target, ui->gc, x, y,
                   (unsigned int)width, (unsigned int)height);
}

static void outline_rect(SettingsUi *ui, unsigned long colour,
                         int x, int y, int width, int height) {
    if (width <= 1 || height <= 1) {
        return;
    }
    XSetForeground(ui->display, ui->gc, colour);
    XDrawRectangle(ui->display, ui->target, ui->gc, x, y,
                   (unsigned int)(width - 1), (unsigned int)(height - 1));
}

/* A rectangle with rounded corners, for the fields and the buttons. Core X has
   no rounded rectangle, so it is the two crossing rectangles plus four quarter
   discs. */
static void fill_rounded(SettingsUi *ui, unsigned long colour,
                         int x, int y, int width, int height, int radius) {
    if (radius <= 0 || radius * 2 >= width || radius * 2 >= height) {
        fill_rect(ui, colour, x, y, width, height);
        return;
    }
    int span = radius * 2;
    int angle = 90 * 64;
    XSetForeground(ui->display, ui->gc, colour);
    XFillRectangle(ui->display, ui->target, ui->gc, x + radius, y,
                   (unsigned int)(width - span), (unsigned int)height);
    XFillRectangle(ui->display, ui->target, ui->gc, x, y + radius,
                   (unsigned int)radius, (unsigned int)(height - span));
    XFillRectangle(ui->display, ui->target, ui->gc, x + width - radius,
                   y + radius, (unsigned int)radius,
                   (unsigned int)(height - span));
    XFillArc(ui->display, ui->target, ui->gc, x, y, (unsigned int)span,
             (unsigned int)span, angle, angle);
    XFillArc(ui->display, ui->target, ui->gc, x + width - span, y,
             (unsigned int)span, (unsigned int)span, 0, angle);
    XFillArc(ui->display, ui->target, ui->gc, x, y + height - span,
             (unsigned int)span, (unsigned int)span, 180 * 64, angle);
    XFillArc(ui->display, ui->target, ui->gc, x + width - span,
             y + height - span, (unsigned int)span, (unsigned int)span,
             270 * 64, angle);
}

/* An XftColor from a pixel the style resolved: Xft wants 16-bit channels, so
   XQueryColor is asked for the channels the server gave us. The caller frees
   it. */
static XftColor xft_from_pixel(SettingsUi *ui, unsigned long pixel) {
    XftColor result;
    XColor colour;
    colour.pixel = pixel;
    XQueryColor(ui->display, ui->colormap, &colour);
    XRenderColor render;
    render.red = colour.red;
    render.green = colour.green;
    render.blue = colour.blue;
    render.alpha = 0xffff;
    XftColorAllocValue(ui->display, ui->visual, ui->colormap, &render, &result);
    return result;
}

static void draw_text(SettingsUi *ui, XftColor *colour, XftFont *font,
                      int x, int baseline, const char *text) {
    if (!colour || !font || !text || !text[0]) {
        return;
    }
    XftDrawStringUtf8(ui->draw, colour, font, x, baseline,
                      (const FcChar8 *)text, (int)strlen(text));
}

static int text_width(SettingsUi *ui, XftFont *font, const char *text) {
    if (!font || !text || !text[0]) {
        return 0;
    }
    XGlyphInfo extent;
    XftTextExtentsUtf8(ui->display, font, (const FcChar8 *)text,
                       (int)strlen(text), &extent);
    return (int)extent.xOff;
}

/* Copy `text` into `out`, cut so it fits `max_width`. Bytes are kept whole, so
   a cut never splits a UTF-8 character. */
static void fit_text(SettingsUi *ui, XftFont *font, const char *text,
                     int max_width, char *out, unsigned int size) {
    snprintf(out, size, "%s", text);
    if (max_width <= 0 || text_width(ui, font, out) <= max_width) {
        return;
    }
    int length = (int)strlen(out);
    for (int cut = length; cut > 0; cut--) {
        if (((unsigned char)out[cut] & 0xc0) == 0x80) {
            continue;                     /* never split a UTF-8 character */
        }
        out[cut] = '\0';
        snprintf(out + cut, size - (unsigned int)cut, "...");
        if (text_width(ui, font, out) <= max_width) {
            return;
        }
        out[cut] = '\0';
    }
    out[0] = '\0';
}

/* --- the three kinds of field ---------------------------------------------- */

static void draw_colour_field(SettingsUi *ui, int x, int y, int width,
                              int height, const SettingValue *value) {
    int swatch = height;
    unsigned long pixel = settings_style_colour(ui->display, ui->screen,
                                                value->text, 0x000000);
    fill_rounded(ui, pixel, x, y, swatch, height, 6);
    outline_rect(ui, ui->style.panel_edge, x, y, swatch, height);

    XftColor text = xft_from_pixel(ui, ui->style.text);
    char shown[SETTINGS_TEXT_LENGTH];
    fit_text(ui, ui->style.font, value->text, width - swatch - 12, shown,
             sizeof(shown));
    draw_text(ui, &text, ui->style.font, x + swatch + 10,
              y + height / 2 + 5, shown);
    XftColorFree(ui->display, ui->visual, ui->colormap, &text);
}

static void draw_text_field(SettingsUi *ui, int row, int x, int y, int width,
                            int height, const SettingValue *value) {
    fill_rounded(ui, ui->style.panel, x, y, width, height, 6);
    outline_rect(ui, row == ui->focus_row ? ui->style.accent
                                          : ui->style.panel_edge,
                 x, y, width, height);

    int inner = width - 20;
    char shown[SETTINGS_TEXT_LENGTH];

    if (row == ui->focus_row) {
        /* The field has the keyboard: it is being typed into, and the part of
           the text that matters is the END — the characters just entered and
           the caret sitting after them. Fitting the head and putting "..." on
           the tail, which is what the resting view does, hid exactly what the
           person was watching: a long colour or path showed its beginning and
           swallowed every keystroke after it. So the head is dropped — a whole
           UTF-8 character at a time, never mid-character — until the tail fits
           the field, and the tail is what is drawn. */
        const char *full = value->text;
        int length = (int)strlen(full);
        int start = 0;
        while (start < length &&
               text_width(ui, ui->style.font, full + start) > inner) {
            start++;
            while (start < length &&
                   ((unsigned char)full[start] & 0xc0) == 0x80) {
                start++;
            }
        }
        snprintf(shown, sizeof(shown), "%s", full + start);
    } else {
        fit_text(ui, ui->style.font, value->text, inner, shown, sizeof(shown));
    }

    XftColor text = xft_from_pixel(ui, ui->style.text);
    draw_text(ui, &text, ui->style.font, x + 10, y + height / 2 + 5, shown);
    XftColorFree(ui->display, ui->visual, ui->colormap, &text);

    if (row == ui->focus_row) {
        int caret_x = x + 10 + text_width(ui, ui->style.font, shown);
        if (caret_x > x + width - 6) {
            caret_x = x + width - 6;
        }
        fill_rect(ui, ui->style.accent, caret_x, y + 6, 2, height - 12);
    }
}

static void draw_list_field(SettingsUi *ui, int row, int x, int y, int width,
                            int height, const SettingValue *value) {
    const int button_width = 32;
    const int gap = 6;
    int input_width = width - button_width * 2 - gap * 2;
    fill_rounded(ui, ui->style.panel, x, y, input_width, height, 6);
    outline_rect(ui, ui->list_editor_open &&
                             ui->list_selected_row == row
                         ? ui->style.accent
                                          : ui->style.panel_edge,
                 x, y, input_width, height);

    int item_count = 0;
    const char *entry = value->text;
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
            item_count++;
        }
        if (!end) {
            break;
        }
        entry = end + 1;
    }

    char summary[SETTINGS_TEXT_LENGTH];
    snprintf(summary, sizeof(summary), "%d item%s — click to edit",
             item_count, item_count == 1 ? "" : "s");
    char shown[SETTINGS_TEXT_LENGTH];
    fit_text(ui, ui->style.font, summary, input_width - 16, shown,
             sizeof(shown));
    XftColor summary_colour = xft_from_pixel(ui, ui->style.text_muted);
    draw_text(ui, &summary_colour, ui->style.font, x + 8,
              y + height / 2 + 5, shown);
    XftColorFree(ui->display, ui->visual, ui->colormap, &summary_colour);

    const char *labels[] = { "+", "-" };
    for (int i = 0; i < 2; i++) {
        int bx = x + input_width + gap + i * (button_width + gap);
        fill_rounded(ui, ui->style.panel, bx, y, button_width, height, 6);
        outline_rect(ui, ui->style.panel_edge, bx, y, button_width, height);
        XftColor text = xft_from_pixel(ui, ui->style.text);
        int label_width = text_width(ui, ui->style.font_bold, labels[i]);
        draw_text(ui, &text, ui->style.font_bold,
                  bx + (button_width - label_width) / 2,
                  y + height / 2 + 5, labels[i]);
        XftColorFree(ui->display, ui->visual, ui->colormap, &text);
    }
}

static int draw_list_item_bounds(const char *text, int wanted,
                                 size_t *item_start, size_t *item_end);

static void draw_list_editor(SettingsUi *ui) {
    if (!ui->list_editor_open || ui->list_selected_row < 0 ||
        ui->list_selected_row >= ui->page.app->setting_count) {
        return;
    }

    int x = UI_SIDEBAR_WIDTH + UI_MARGIN;
    int y = UI_HEADER_HEIGHT + UI_MARGIN;
    int width = ui->window_width - x - UI_MARGIN;
    int height = ui->window_height - UI_FOOTER_HEIGHT - y - UI_MARGIN;
    int visible = (height - UI_LIST_EDITOR_HEADER - UI_LIST_EDITOR_FOOTER) /
                  UI_LIST_EDITOR_ROW_HEIGHT;
    if (width <= 0 || visible <= 0) {
        return;
    }

    fill_rounded(ui, ui->style.sidebar, x, y, width, height, 8);
    outline_rect(ui, ui->style.accent, x, y, width, height);
    XftColor text = xft_from_pixel(ui, ui->style.text);
    XftColor muted = xft_from_pixel(ui, ui->style.text_muted);
    XftColor accent = xft_from_pixel(ui, ui->style.accent);
    const SettingValue *value = &ui->page.values[ui->list_selected_row];

    draw_text(ui, &text, ui->style.font_bold, x + 12, y + 27,
              ui->page.app->settings[ui->list_selected_row].label);
    draw_text(ui, &muted, ui->style.font, x + 12, y + 43,
              "Select a command; use Home/End and arrows to inspect long text.");

    int list_top = y + UI_LIST_EDITOR_HEADER;
    int item_count = 0;
    const char *scan = value->text;
    while (*scan) {
        const char *end = strchr(scan, ';');
        size_t length = end ? (size_t)(end - scan) : strlen(scan);
        while (length > 0 && isspace((unsigned char)scan[length - 1])) {
            length--;
        }
        while (length > 0 && isspace((unsigned char)*scan)) {
            scan++;
            length--;
        }
        if (length > 0) {
            item_count++;
        }
        if (!end) {
            break;
        }
        scan = end + 1;
    }
    int total = item_count + (ui->list_editing &&
                               ui->list_selected_item == item_count ? 1 : 0);
    for (int visible_index = 0; visible_index < visible; visible_index++) {
        int index = ui->list_scroll + visible_index;
        if (index >= total) {
            break;
        }
        int item_y = list_top + visible_index * UI_LIST_EDITOR_ROW_HEIGHT;
        int selected = ui->list_selected_item == index &&
                       ui->list_selected_row == ui->focus_row;
        fill_rect(ui, selected ? ui->style.sidebar_active : ui->style.panel,
                  x + 8, item_y, width - 16, UI_LIST_EDITOR_ROW_HEIGHT - 2);
        outline_rect(ui, selected ? ui->style.accent : ui->style.panel_edge,
                     x + 8, item_y, width - 16, UI_LIST_EDITOR_ROW_HEIGHT - 2);

        char item[SETTINGS_TEXT_LENGTH] = "";
        size_t start = 0;
        size_t end = 0;
        if (index < item_count &&
            draw_list_item_bounds(value->text, index, &start, &end)) {
            size_t length = end - start;
            if (length >= sizeof(item)) {
                length = sizeof(item) - 1;
            }
            memcpy(item, value->text + start, length);
            item[length] = '\0';
        } else if (index == item_count) {
            snprintf(item, sizeof(item), "New item — type command here");
        }

        char shown[SETTINGS_TEXT_LENGTH];
        int caret_x = -1;
        if (selected && ui->list_editing && index < item_count) {
            size_t item_length = strlen(item);
            size_t caret = (size_t)(ui->caret < 0 ? 0 : ui->caret);
            if (caret > item_length) {
                caret = item_length;
            }
            size_t start_at = 0;
            while (start_at < caret &&
                   text_width(ui, ui->style.font,
                              item + start_at) > width - 42) {
                start_at++;
                while (start_at < caret &&
                       ((unsigned char)item[start_at] & 0xc0) == 0x80) {
                    start_at++;
                }
            }
            fit_text(ui, ui->style.font, item + start_at, width - 40,
                     shown, sizeof(shown));
            char caret_part[SETTINGS_TEXT_LENGTH];
            size_t caret_length = caret - start_at;
            if (caret_length >= sizeof(caret_part)) {
                caret_length = sizeof(caret_part) - 1;
            }
            memcpy(caret_part, item + start_at, caret_length);
            caret_part[caret_length] = '\0';
            caret_x = x + 16 + text_width(ui, ui->style.font, caret_part);
        } else if (selected && ui->list_editing && index == item_count) {
            caret_x = x + 16;
        } else {
            fit_text(ui, ui->style.font, item, width - 40, shown,
                     sizeof(shown));
        }
        draw_text(ui, selected ? &accent : &text, ui->style.font,
                  x + 16, item_y + 20, shown);
        if (caret_x >= 0) {
            fill_rect(ui, ui->style.accent, caret_x, item_y + 5, 2,
                      UI_LIST_EDITOR_ROW_HEIGHT - 12);
        }
    }

    int button_width = 72;
    int gap = 8;
    int button_y = y + height - UI_LIST_EDITOR_FOOTER + 6;
    int done_x = x + width - 12 - button_width;
    int remove_x = done_x - gap - button_width;
    int add_x = remove_x - gap - button_width;
    const char *labels[] = { "+", "-", "Done" };
    int positions[] = { add_x, remove_x, done_x };
    for (int i = 0; i < 3; i++) {
        fill_rounded(ui, ui->style.panel, positions[i], button_y,
                     button_width, 34, 6);
        outline_rect(ui, ui->style.panel_edge, positions[i], button_y,
                     button_width, 34);
        int label_width = text_width(ui, ui->style.font_bold, labels[i]);
        draw_text(ui, &text, ui->style.font_bold,
                  positions[i] + (button_width - label_width) / 2,
                  button_y + 22, labels[i]);
    }

    XftColorFree(ui->display, ui->visual, ui->colormap, &text);
    XftColorFree(ui->display, ui->visual, ui->colormap, &muted);
    XftColorFree(ui->display, ui->visual, ui->colormap, &accent);
}

/* A choice row: the current name, and a caret saying it opens a list. */
static void draw_choice_field(SettingsUi *ui, int row, int x, int y,
                              int width, int height,
                              const SettingValue *value) {
    fill_rounded(ui, ui->style.panel, x, y, width, height, 6);
    outline_rect(ui, row == ui->dropdown_row ? ui->style.accent
                                             : ui->style.panel_edge,
                 x, y, width, height);

    char shown[SETTINGS_TEXT_LENGTH];
    fit_text(ui, ui->style.font, value->text, width - 34, shown,
             sizeof(shown));

    XftColor text = xft_from_pixel(ui, ui->style.text);
    draw_text(ui, &text, ui->style.font, x + 10, y + height / 2 + 5, shown);
    XftColorFree(ui->display, ui->visual, ui->colormap, &text);

    XftColor accent = xft_from_pixel(ui, ui->style.accent);
    draw_text(ui, &accent, ui->style.font, x + width - 22,
              y + height / 2 + 5, ui->dropdown_row == row ? "^" : "v");
    XftColorFree(ui->display, ui->visual, ui->colormap, &accent);
}

/* The open list: every name, under the field, the one the value holds lit. */
static void draw_dropdown(SettingsUi *ui) {
    int row = ui->dropdown_row;
    if (row < 0 || !ui->page.app || row >= ui->page.app->setting_count) {
        return;
    }
    const SettingDef *def = &ui->page.app->settings[row];
    const char *const *list = settings_choices(def);
    int count = settings_choice_count(def);
    if (!list || count <= 0) {
        return;
    }

    const char *current = ui->page.values[row].text;
    XftColor text = xft_from_pixel(ui, ui->style.text);
    XftColor accent = xft_from_pixel(ui, ui->style.accent);

    for (int i = 0; i < count; i++) {
        int cx, cy, cw, ch;
        settings_ui_choice_rect(ui, row, i, &cx, &cy, &cw, &ch);
        int chosen = list[i] && strcmp(list[i], current) == 0;

        fill_rect(ui, chosen ? ui->style.sidebar_active : ui->style.panel,
                  cx, cy, cw, ch);
        outline_rect(ui, ui->style.panel_edge, cx, cy, cw, ch);
        draw_text(ui, chosen ? &accent : &text, ui->style.font, cx + 12,
                  cy + ch / 2 + 5, list[i]);
    }

    XftColorFree(ui->display, ui->visual, ui->colormap, &text);
    XftColorFree(ui->display, ui->visual, ui->colormap, &accent);
}

static int draw_list_item_bounds(const char *text, int wanted,
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
        if (visible_start < visible_end && item++ == wanted) {
            *item_start = start;
            *item_end = end;
            return 1;
        }
        if (end == text_length) {
            break;
        }
        start = end + 1;
    }
    return 0;
}

static void draw_switch(SettingsUi *ui, int x, int y, int height,
                        const SettingValue *value) {
    int flag = setting_value_bool(value);
    int track_w = UI_SWITCH_WIDTH;
    int track_h = height - 10;

    fill_rounded(ui, flag ? ui->style.accent : ui->style.panel, x,
                 y + 5, track_w, track_h, track_h / 2);
    outline_rect(ui, ui->style.panel_edge, x, y + 5, track_w, track_h);

    int knob = track_h - 8;
    int knob_x = flag ? x + track_w - knob - 4 : x + 4;
    fill_rounded(ui, ui->style.text, knob_x, y + 9, knob, knob, knob / 2);

    XftColor text = xft_from_pixel(ui, ui->style.text);
    draw_text(ui, &text, ui->style.font, x + track_w + 10,
              y + height / 2 + 5,
              gnuchan_lang_t(flag ? "switch_on" : "switch_off"));
    XftColorFree(ui->display, ui->visual, ui->colormap, &text);
}

/* --- the panel's parts ----------------------------------------------------- */

static void draw_sidebar(SettingsUi *ui) {
    fill_rect(ui, ui->style.sidebar, 0, 0, UI_SIDEBAR_WIDTH,
              ui->window_height);

    XftColor title = xft_from_pixel(ui, ui->style.accent);
    XftColor text = xft_from_pixel(ui, ui->style.text);
    XftColor muted = xft_from_pixel(ui, ui->style.text_muted);

    draw_text(ui, &title, ui->style.font_bold, UI_MARGIN, 46,
              gnuchan_lang_t("settings_title"));
    draw_text(ui, &muted, ui->style.font, UI_MARGIN, 66,
              gnuchan_lang_t("settings_subtitle"));

    for (int i = 0; i < ui->app_count; i++) {
        int y = settings_ui_category_y(i);
        if (y + UI_CATEGORY_HEIGHT > ui->window_height) {
            break;
        }
        int active = i == ui->app_index;
        int hovered = i == ui->hover_category;

        if (active) {
            fill_rect(ui, ui->style.sidebar_active, 0, y, UI_SIDEBAR_WIDTH,
                      UI_CATEGORY_HEIGHT);
            fill_rect(ui, ui->style.accent, 0, y, 4, UI_CATEGORY_HEIGHT);
        } else if (hovered) {
            fill_rect(ui, ui->style.panel, 0, y, UI_SIDEBAR_WIDTH,
                      UI_CATEGORY_HEIGHT);
        }

        XftColor *colour = active ? &title : &text;
        draw_text(ui, colour, ui->style.font, UI_MARGIN,
                  y + UI_CATEGORY_HEIGHT / 2 + 5, ui->apps[i].name);
    }

    XftColorFree(ui->display, ui->visual, ui->colormap, &title);
    XftColorFree(ui->display, ui->visual, ui->colormap, &text);
    XftColorFree(ui->display, ui->visual, ui->colormap, &muted);
}

static void draw_header(SettingsUi *ui) {
    if (!ui->page.app) {
        return;
    }
    XftColor title = xft_from_pixel(ui, ui->style.text);
    XftColor muted = xft_from_pixel(ui, ui->style.text_muted);

    draw_text(ui, &title, ui->style.font_bold, UI_SIDEBAR_WIDTH + UI_MARGIN,
              46, ui->page.app->name);
    draw_text(ui, &muted, ui->style.font, UI_SIDEBAR_WIDTH + UI_MARGIN, 68,
              ui->page.app->subtitle);

    XftColorFree(ui->display, ui->visual, ui->colormap, &title);
    XftColorFree(ui->display, ui->visual, ui->colormap, &muted);
}

static void draw_row(SettingsUi *ui, int row) {
    const SettingDef *def = &ui->page.app->settings[row];
    const SettingValue *value = &ui->page.values[row];
    int shown = row - ui->scroll;
    int y = UI_HEADER_HEIGHT + shown * UI_ROW_HEIGHT;

    if (row == ui->hover_row) {
        fill_rect(ui, ui->style.panel, UI_SIDEBAR_WIDTH, y,
                  ui->window_width - UI_SIDEBAR_WIDTH, UI_ROW_HEIGHT);
    }

    XftColor label = xft_from_pixel(ui, ui->style.text);
    XftColor muted = xft_from_pixel(ui, ui->style.text_muted);

    /* The label, and the group it belongs to underneath in a quieter colour.
       The group is drawn under every row rather than as a heading band, so the
       rows and the clicks stay on one grid. */
    int label_x = UI_SIDEBAR_WIDTH + UI_MARGIN + 4;
    int fx, fy, fw, fh;
    settings_ui_row_field_rect(ui, row, &fx, &fy, &fw, &fh);
    char shown_label[SETTINGS_TEXT_LENGTH];
    char shown_group[SETTINGS_TEXT_LENGTH];
    int label_width = fx - label_x - UI_MARGIN;
    fit_text(ui, ui->style.font, def->label, label_width,
             shown_label, sizeof(shown_label));
    draw_text(ui, &label, ui->style.font, label_x, y + 24, shown_label);
    if (def->group && def->group[0]) {
        fit_text(ui, ui->style.font, def->group, label_width,
                 shown_group, sizeof(shown_group));
        draw_text(ui, &muted, ui->style.font, label_x, y + 44, shown_group);
    }

    XftColorFree(ui->display, ui->visual, ui->colormap, &label);
    XftColorFree(ui->display, ui->visual, ui->colormap, &muted);

    switch (def->type) {
    case SETTING_COLOR:
        draw_colour_field(ui, fx, fy, fw, fh, value);
        break;
    case SETTING_BOOL:
        draw_switch(ui, fx, fy, fh, value);
        break;
    case SETTING_CHOICE:
        draw_choice_field(ui, row, fx, fy, fw, fh, value);
        break;
    case SETTING_LIST:
        draw_list_field(ui, row, fx, fy, fw, fh, value);
        break;
    default:
        draw_text_field(ui, row, fx, fy, fw, fh, value);
        break;
    }

    fill_rect(ui, ui->style.panel_edge, UI_SIDEBAR_WIDTH + UI_MARGIN,
              y + UI_ROW_HEIGHT - 1,
              ui->window_width - UI_SIDEBAR_WIDTH - UI_MARGIN * 2, 1);
}

static void draw_footer(SettingsUi *ui) {
    int top = ui->window_height - UI_FOOTER_HEIGHT;
    fill_rect(ui, ui->style.sidebar, UI_SIDEBAR_WIDTH, top,
              ui->window_width - UI_SIDEBAR_WIDTH, UI_FOOTER_HEIGHT);
    fill_rect(ui, ui->style.panel_edge, UI_SIDEBAR_WIDTH, top,
              ui->window_width - UI_SIDEBAR_WIDTH, 1);

    XftColor text = xft_from_pixel(ui,
        ui->status_failed ? ui->style.danger : ui->style.text_muted);
    char shown[SETTINGS_TEXT_LENGTH];
    fit_text(ui, ui->style.font, ui->status,
             ui->window_width - UI_SIDEBAR_WIDTH - UI_MARGIN * 2 - 300,
             shown, sizeof(shown));
    draw_text(ui, &text, ui->style.font, UI_SIDEBAR_WIDTH + UI_MARGIN,
              top + UI_FOOTER_HEIGHT / 2 + 5, shown);
    XftColorFree(ui->display, ui->visual, ui->colormap, &text);

    /* Revert, then Save: the one that writes is rightmost, which is the corner
       a person's hand goes to last and so reads as the final act. */
    int bx, by, bw, bh;
    settings_ui_revert_rect(ui, &bx, &by, &bw, &bh);
    fill_rounded(ui, ui->style.panel, bx, by, bw, bh, 8);
    outline_rect(ui, ui->style.panel_edge, bx, by, bw, bh);
    XftColor button = xft_from_pixel(ui, ui->style.text);
    const char *rev = gnuchan_lang_t("button_revert");
    int rw = text_width(ui, ui->style.font, rev);
    draw_text(ui, &button, ui->style.font, bx + (bw - rw) / 2,
              by + bh / 2 + 5, rev);
    XftColorFree(ui->display, ui->visual, ui->colormap, &button);

    settings_ui_save_rect(ui, &bx, &by, &bw, &bh);
    fill_rounded(ui, ui->style.accent, bx, by, bw, bh, 8);
    XftColor save_text = xft_from_pixel(ui, ui->style.background);
    const char *sv = gnuchan_lang_t("button_save");
    int sw = text_width(ui, ui->style.font_bold, sv);
    draw_text(ui, &save_text, ui->style.font_bold, bx + (bw - sw) / 2,
              by + bh / 2 + 5, sv);
    XftColorFree(ui->display, ui->visual, ui->colormap, &save_text);
}

void settings_draw(SettingsUi *ui) {
    if (!ui || !ui->display || ui->window == None || !ui->draw) {
        return;
    }

    fill_rect(ui, ui->style.background, 0, 0,
              ui->window_width, ui->window_height);
    fill_rect(ui, ui->style.background, UI_SIDEBAR_WIDTH, UI_HEADER_HEIGHT,
              ui->window_width - UI_SIDEBAR_WIDTH,
              ui->window_height - UI_HEADER_HEIGHT - UI_FOOTER_HEIGHT);

    draw_sidebar(ui);
    draw_header(ui);

    int count = ui->page.app ? ui->page.app->setting_count : 0;
    int visible = settings_ui_visible_rows(ui);
    for (int i = 0; i < visible; i++) {
        int row = ui->scroll + i;
        if (row >= count) {
            break;
        }
        draw_row(ui, row);
    }

    draw_footer(ui);

    /* The list is drawn last, so it sits over the rows it covers. */
    draw_dropdown(ui);
    draw_list_editor(ui);

    /* The finished picture goes to the window in ONE copy: nothing is shown
       half-drawn, which is what removes the flicker the panel had when every
       shape went to the window on its own. */
    XCopyArea(ui->display, ui->target, ui->window, ui->gc, 0, 0,
              (unsigned int)ui->window_width, (unsigned int)ui->window_height,
              0, 0);
    XFlush(ui->display);
}
