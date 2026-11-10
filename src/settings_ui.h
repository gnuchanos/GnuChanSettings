/*
 * settings_ui.h — the panel's window, its state, and its layout.
 *
 * This header is the agreement between the two halves of GnuChanSettings: the
 * code that handles what a person does (settings_ui.c) and the code that draws
 * the result (settings_draw.c). Both walk the SAME layout numbers — the
 * sidebar's width, a row's height, where the fields sit — so a click lands on
 * the field that was drawn under it. The numbers live here, once.
 *
 * The state is one struct. It holds the window and its drawing surfaces, the
 * style, the current program's values as the person has them on screen (which
 * are NOT yet written to disk — that is what Save does), which field has the
 * keyboard, and what the last save had to say.
 */
#ifndef GNUCHANSETTINGS_UI_H
#define GNUCHANSETTINGS_UI_H

#include <X11/Xlib.h>
#include <X11/Xft/Xft.h>

#include "settings_store.h"
#include "settings_style.h"
#include "settings_types.h"
#include "settings_usb.h"

/* --- the layout -----------------------------------------------------------
 *
 * The window can be resized; both the settings rows and the sidebar categories
 * scroll inside it rather than changing the window size. The numbers are here
 * because drawing and hit-testing must agree to the pixel. */
#define UI_WINDOW_WIDTH   840
#define UI_WINDOW_HEIGHT  580
#define UI_SIDEBAR_WIDTH  220
#define UI_HEADER_HEIGHT  86
#define UI_ROW_HEIGHT     58
#define UI_FOOTER_HEIGHT  64
#define UI_MARGIN         18
#define UI_SCROLLBAR_WIDTH 6
#define UI_SCROLLBAR_GUTTER 12
#define UI_SCROLLBAR_MIN_THUMB 22
/* A category's row in the sidebar. The sidebar can scroll when all categories
   do not fit in the available height. */
#define UI_CATEGORY_HEIGHT 28

/* The default width of the field column on the right of a row's label: where
   the text box, the switch or the swatch is drawn. Wider windows grow it. */
#define UI_FIELD_WIDTH   320
#define UI_SWITCH_WIDTH  64

/* The height of one name in an open dropdown. Shorter than a row, because a
   list of names is read as a list and not as a column of fields. */
#define UI_CHOICE_HEIGHT 30
#define UI_LIST_EDITOR_ROW_HEIGHT 30
#define UI_LIST_EDITOR_HEADER 44
#define UI_LIST_EDITOR_FOOTER 48
#define UI_USB_DEVICE_ROW_HEIGHT 36

/* Which row holds the keyboard. -1 means none, which is the state the panel
   opens in — nothing is focused until a person clicks a field. */
#define UI_FOCUS_NONE (-1)

/* One program's settings as the window holds them: the values on screen, which
   are written to the file only when Save is pressed. */
typedef struct UiPage {
    const AppDef     *app;
    SettingValue      values[SETTINGS_MAX_ROWS];
    int               loaded;      /* the file was read (0 = defaults shown) */
} UiPage;

typedef struct UiScrollbar {
    int x;
    int track_y;
    int width;
    int track_height;
    int thumb_y;
    int thumb_height;
    int maximum;
} UiScrollbar;

typedef struct SettingsUi {
    Display *display;
    int      screen;
    Window   root;
    Window   window;
    GC       gc;
    Visual  *visual;
    Colormap colormap;
    int      depth;
    XftDraw *draw;                 /* the BACK BUFFER as an Xft surface */
    Pixmap   buffer;               /* the whole panel is painted here    */
    Drawable target;               /* where a paint goes: the back buffer */
    int      buffer_width;         /* dimensions of the back buffer       */
    int      buffer_height;

    SettingsStyle style;

    const AppDef *apps;
    int           app_count;
    int           app_index;       /* which category is open            */
    UiPage        page;
    int           category_scroll; /* first sidebar category shown      */
    int           scrollbar_drag;  /* 0 none, 1 categories, 2 settings  */
    int           scrollbar_drag_y;
    int           scrollbar_drag_scroll;

    int           focus_row;       /* the row holding the keyboard, -1 = none */
    int           caret;           /* the caret's byte offset in that field   */
    int           scroll;          /* the first row shown, in rows            */

    int           hover_row;       /* the row under the pointer, -1 = none    */
    int           hover_category;  /* a category under the pointer, -1 = none */
    int           dropdown_row;    /* the choice row whose list is open, -1   */
    int           list_selected_row;
    int           list_selected_item;
    int           list_editing;       /* selected item accepts keyboard input */
    int           list_editor_open;   /* the expanded, row-based list editor */
    int           list_scroll;        /* first visible item in that editor */

    SettingsUsbDevice usb_devices[SETTINGS_USB_MAX_DEVICES];
    int           usb_device_count;
    int           usb_scroll;
    int           usb_selected;
    int           usb_menu_open;
    int           usb_menu_x;
    int           usb_menu_y;

    /* The window's actual size, which changes when the window manager resizes
       or maximizes it. The panel relays out itself against these numbers. */
    int           window_width;    /* current width in pixels                 */
    int           window_height;   /* current height in pixels                */

    /* What the last save said, shown in the footer. `status_failed` colours it
       — a save that failed must not read like one that worked. */
    char status[SETTINGS_TEXT_LENGTH];
    int  status_failed;

    char saved_theme_color[SETTINGS_TEXT_LENGTH];

    int running;
} SettingsUi;

/* Open the window: the display, the style, the first category's values. Returns
 * 0 on success, -1 when there is no display or the window cannot be made. */
int settings_ui_open(SettingsUi *ui);

/* The event loop, until the window is closed. */
void settings_ui_run(SettingsUi *ui);

/* Close the display and free everything settings_ui_open() made. */
void settings_ui_close(SettingsUi *ui);

/* Load the open category's values from its file into ui->page. Called when the
 * panel opens and when a person switches category. */
void settings_ui_load_page(SettingsUi *ui);

/* --- the geometry, shared by the drawing and the hit-testing ---------------
 *
 * Every one of these answers a question both halves ask, so they are computed
 * in one place. They are pure: no X, no state changed. */

/* The y of a visible category row in the sidebar. */
int settings_ui_category_y(int index);
static inline int settings_ui_visible_categories(const SettingsUi *ui) {
    int height = ui->window_height - UI_HEADER_HEIGHT;
    int categories = height / UI_CATEGORY_HEIGHT;
    return categories > 0 ? categories : 1;
}

static inline int settings_ui_category_at(const SettingsUi *ui, int y) {
    if (y < UI_HEADER_HEIGHT || y >= ui->window_height) {
        return -1;
    }
    int visible_row = (y - UI_HEADER_HEIGHT) / UI_CATEGORY_HEIGHT;
    if (visible_row >= settings_ui_visible_categories(ui)) {
        return -1;
    }
    int category = ui->category_scroll + visible_row;
    return category < ui->app_count ? category : -1;
}

static inline int settings_ui_scrollbar_geometry(const SettingsUi *ui,
                                                 int categories,
                                                 UiScrollbar *bar) {
    int total = categories
                    ? ui->app_count
                    : ui->page.app ? ui->page.app->setting_count : 0;
    int visible = categories
                      ? settings_ui_visible_categories(ui)
                      : (ui->window_height - UI_HEADER_HEIGHT -
                         UI_FOOTER_HEIGHT) / UI_ROW_HEIGHT;
    if (visible < 1) {
        visible = 1;
    }
    bar->x = categories ? UI_SIDEBAR_WIDTH - UI_SCROLLBAR_WIDTH - 4
                        : ui->window_width - UI_SCROLLBAR_WIDTH - 4;
    bar->track_y = UI_HEADER_HEIGHT + 4;
    bar->track_height = categories
                            ? ui->window_height - bar->track_y - 4
                            : ui->window_height - UI_FOOTER_HEIGHT -
                                  bar->track_y - 4;
    bar->width = UI_SCROLLBAR_WIDTH;
    if (bar->track_height < 1) {
        bar->track_height = 1;
    }
    bar->maximum = total - visible;
    if (bar->maximum < 0) {
        bar->maximum = 0;
    }
    int scroll = categories ? ui->category_scroll : ui->scroll;
    if (scroll < 0) {
        scroll = 0;
    } else if (scroll > bar->maximum) {
        scroll = bar->maximum;
    }
    int thumb = total > 0 ? bar->track_height * visible / total
                          : bar->track_height;
    if (thumb < UI_SCROLLBAR_MIN_THUMB) {
        thumb = UI_SCROLLBAR_MIN_THUMB;
    }
    if (thumb > bar->track_height) {
        thumb = bar->track_height;
    }
    bar->thumb_height = thumb;
    int travel = bar->track_height - thumb;
    bar->thumb_y = bar->track_y +
                   (bar->maximum > 0
                        ? travel * scroll / bar->maximum
                        : 0);
    return bar->maximum > 0;
}

static inline int settings_ui_footer_status_width(const SettingsUi *ui) {
    int save_x = ui->window_width - UI_MARGIN - 130;
    int revert_x = save_x - 130 - 12;
    int status_x = UI_SIDEBAR_WIDTH + UI_MARGIN;
    int width = revert_x - UI_MARGIN - status_x;
    return width > 0 ? width : 0;
}

/* The rectangle of a setting row's field on screen, counting the scroll. */
void settings_ui_row_field_rect(const SettingsUi *ui, int row,
                                int *x, int *y, int *width, int *height);

/* The rectangle of one name in an open dropdown — the list hangs under the
   field of `row` — and which name a point falls in, or -1. */
void settings_ui_choice_rect(const SettingsUi *ui, int row, int index,
                             int *x, int *y, int *width, int *height);
int settings_ui_choice_at(const SettingsUi *ui, int x, int y);

/* The row a point falls in, counting the scroll; -1 when it is above the first
   shown row or in the footer. */
int settings_ui_row_at(const SettingsUi *ui, int x, int y);

/* How many rows fit in the panel's body at the current size. */
int settings_ui_visible_rows(const SettingsUi *ui);

/* USB storage controls share the System page below its ordinary settings. */
int settings_ui_usb_panel(const SettingsUi *ui, int *top, int *list_top,
                          int *visible_rows);

/* The rectangle of the Save button. */
void settings_ui_save_rect(const SettingsUi *ui, int *x, int *y,
                           int *width, int *height);

/* The rectangle of the Revert button. */
void settings_ui_revert_rect(const SettingsUi *ui, int *x, int *y,
                             int *width, int *height);

#endif /* GNUCHANSETTINGS_UI_H */
