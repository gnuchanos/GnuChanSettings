#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "settings_store.h"
#include "settings_ui.h"

static int test_category_layout(void) {
    SettingsUi ui = {0};
    ui.apps = settings_apps(&ui.app_count);
    ui.window_height = UI_WINDOW_HEIGHT;

    int system_category = -1;
    for (int i = 0; i < ui.app_count; i++) {
        if (strcmp(ui.apps[i].name, "System") == 0) {
            system_category = i;
            break;
        }
    }
    if (system_category < 0) {
        fprintf(stderr, "System category should be registered\n");
        return 0;
    }
    int system_y = UI_HEADER_HEIGHT +
                   system_category * UI_CATEGORY_HEIGHT + 1;
    if (settings_ui_category_at(&ui, system_y) >= 0) {
        fprintf(stderr, "System category should require sidebar scrolling\n");
        return 0;
    }
    int clipped_row_y = UI_HEADER_HEIGHT +
                        settings_ui_visible_categories(&ui) *
                            UI_CATEGORY_HEIGHT + 1;
    if (clipped_row_y < ui.window_height &&
        settings_ui_category_at(&ui, clipped_row_y) >= 0) {
        fprintf(stderr, "partially clipped categories should not be clickable\n");
        return 0;
    }

    const int heights[] = { UI_WINDOW_HEIGHT, 320, 220 };
    for (size_t i = 0; i < sizeof(heights) / sizeof(heights[0]); i++) {
        ui.window_height = heights[i];
        int maximum = ui.app_count - settings_ui_visible_categories(&ui);
        if (maximum < 0) {
            maximum = 0;
        }
        for (int category = 0; category < ui.app_count; category++) {
            ui.category_scroll = category < maximum ? category : maximum;
            int y = UI_HEADER_HEIGHT +
                    (category - ui.category_scroll) * UI_CATEGORY_HEIGHT + 1;
            if (y >= ui.window_height ||
                settings_ui_category_at(&ui, y) != category) {
                fprintf(stderr, "%s should be reachable after scrolling\n",
                        ui.apps[category].name);
                return 0;
            }
        }
    }

    ui.window_height = 220;
    ui.category_scroll = 0;
    UiScrollbar categories;
    if (!settings_ui_scrollbar_geometry(&ui, 1, &categories) ||
        categories.maximum <= 0 ||
        categories.thumb_height >= categories.track_height) {
        fprintf(stderr, "overflowing categories should show a scrollbar\n");
        return 0;
    }
    ui.category_scroll = categories.maximum;
    settings_ui_scrollbar_geometry(&ui, 1, &categories);
    if (categories.thumb_y + categories.thumb_height !=
        categories.track_y + categories.track_height) {
        fprintf(stderr, "category scrollbar should reach the bottom\n");
        return 0;
    }

    ui.page.app = &ui.apps[system_category];
    UiScrollbar rows;
    if (!settings_ui_scrollbar_geometry(&ui, 0, &rows) ||
        rows.maximum <= 0) {
        fprintf(stderr, "overflowing settings rows should show a scrollbar\n");
        return 0;
    }
    ui.scroll = rows.maximum;
    settings_ui_scrollbar_geometry(&ui, 0, &rows);
    if (rows.thumb_y + rows.thumb_height !=
        rows.track_y + rows.track_height) {
        fprintf(stderr, "settings scrollbar should reach the bottom\n");
        return 0;
    }

    ui.window_width = UI_SIDEBAR_WIDTH + UI_FIELD_WIDTH + 2 * UI_MARGIN +
                      UI_SCROLLBAR_GUTTER + 180;
    int status_x = UI_SIDEBAR_WIDTH + UI_MARGIN;
    int revert_x = ui.window_width - UI_MARGIN - 130 - 130 - 12;
    if (status_x + settings_ui_footer_status_width(&ui) > revert_x - UI_MARGIN) {
        fprintf(stderr, "footer status should not overlap button controls\n");
        return 0;
    }
    return 1;
}

static const AppDef *find_app(const char *name) {
    int count = 0;
    const AppDef *apps = settings_apps(&count);
    for (int i = 0; i < count; i++) {
        if (strcmp(apps[i].name, name) == 0) {
            return &apps[i];
        }
    }
    return NULL;
}

static int has_setting(const AppDef *app, const char *key) {
    if (!app) {
        return 0;
    }
    for (int i = 0; i < app->setting_count; i++) {
        if (strcmp(app->settings[i].key, key) == 0) {
            return 1;
        }
    }
    return 0;
}

static int read_text(const char *path, char *out, size_t size) {
    FILE *file = fopen(path, "rb");
    if (!file) {
        return 0;
    }
    size_t read = fread(out, 1, size - 1, file);
    out[read] = '\0';
    int okay = !ferror(file);
    fclose(file);
    return okay;
}

static int write_text(const char *path, const char *text) {
    FILE *file = fopen(path, "wb");
    if (!file) {
        return 0;
    }
    size_t length = strlen(text);
    int okay = fwrite(text, 1, length, file) == length;
    okay = fclose(file) == 0 && okay;
    return okay;
}

static int cleanup_config(const char *root) {
    char path[1024];
    const char *files[] = {
        "gnuchan-desktop/panel.conf",
        "GnuChanFileManager/config.conf",
        "GnuChanSettings/system.conf",
        "GnuChanWelcome/welcome.conf",
    };
    const char *dirs[] = {
        "gnuchan-desktop",
        "GnuChanFileManager",
        "GnuChanSettings",
        "GnuChanWelcome",
    };
    for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
        snprintf(path, sizeof(path), "%s/%s", root, files[i]);
        remove(path);
    }
    for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
        snprintf(path, sizeof(path), "%s/%s", root, dirs[i]);
        rmdir(path);
    }
    return rmdir(root) == 0;
}

static int run_tests(const char *config_home) {
    if (!test_category_layout()) {
        return 0;
    }

    int count = 0;
    settings_apps(&count);
    if (count > SETTINGS_MAX_APPS) {
        fprintf(stderr, "settings app count %d exceeds limit %d\n",
                count, SETTINGS_MAX_APPS);
        return 0;
    }

    const AppDef *desktop = find_app("GnuChanDesktop");
    const AppDef *file_manager = find_app("GnuChanFileManager");
    const AppDef *sound = find_app("GnuChanSound");
    const AppDef *welcome = find_app("GnuChanWelcome");
    if (!desktop || !has_setting(desktop, "wallpaper") ||
        !has_setting(desktop, "background_color") ||
        !has_setting(desktop, "wallpaper_mode") ||
        !file_manager || !has_setting(file_manager, "show_hidden") ||
        !has_setting(file_manager, "start_location") ||
        !sound || !has_setting(sound, "ApplicationLanguage") ||
        !welcome || !has_setting(welcome, "FirstRunComplete") ||
        strcmp(welcome->file, "welcome.conf") != 0) {
        fprintf(stderr, "one or more new application settings are not registered\n");
        return 0;
    }

    char expected_root[1024];
    snprintf(expected_root, sizeof(expected_root), "%s", config_home);
    if (setenv("XDG_CONFIG_HOME", expected_root, 1) != 0) {
        fprintf(stderr, "could not set test XDG_CONFIG_HOME\n");
        return 0;
    }

    SettingValue values[SETTINGS_MAX_ROWS];
    char reason[SETTINGS_TEXT_LENGTH];

    if (settings_load(desktop, values) != 0) {
        fprintf(stderr, "desktop settings unexpectedly exist\n");
        return 0;
    }
    snprintf(values[0].text, sizeof(values[0].text), "%s", "/home/test/wall.png");
    snprintf(values[1].text, sizeof(values[1].text), "%s", "#123ABC");
    snprintf(values[2].text, sizeof(values[2].text), "%s", "fit");
    if (settings_save(desktop, values, reason) != 0 ||
        settings_load(desktop, values) != 1 ||
        strcmp(values[0].text, "/home/test/wall.png") != 0 ||
        strcmp(values[1].text, "#123ABC") != 0 ||
        strcmp(values[2].text, "fit") != 0) {
        fprintf(stderr, "desktop settings round trip failed: %s\n", reason);
        return 0;
    }

    if (settings_load(file_manager, values) != 0) {
        fprintf(stderr, "file manager settings unexpectedly exist\n");
        return 0;
    }
    setting_value_set_bool(&values[0], 1, 0);
    snprintf(values[1].text, sizeof(values[1].text), "%s", "/home/test");
    if (settings_save(file_manager, values, reason) != 0 ||
        settings_load(file_manager, values) != 1 ||
        strcmp(values[0].text, "true") != 0 ||
        strcmp(values[1].text, "/home/test") != 0) {
        fprintf(stderr, "file manager settings round trip failed: %s\n", reason);
        return 0;
    }

    char config_path[2048];
    snprintf(config_path, sizeof(config_path),
             "%s/GnuChanFileManager/config.conf", expected_root);
    char content[4096];
    if (!read_text(config_path, content, sizeof(content)) ||
        strstr(content, "[General]\n") == NULL ||
        strstr(content, "show_hidden = true\n") == NULL ||
        strstr(content, "start_location = \"/home/test\"\n") == NULL) {
        fprintf(stderr, "file manager file was not written as a General section\n");
        return 0;
    }

    if (!write_text(config_path,
                    "[General]\nshow_hidden=false\n"
                    "[Other]\nshow_hidden=true\n")) {
        fprintf(stderr, "could not prepare section-isolation fixture\n");
        return 0;
    }
    if (settings_load(file_manager, values) != 1 ||
        strcmp(values[0].text, "false") != 0) {
        fprintf(stderr, "file manager read a value outside General\n");
        return 0;
    }
    snprintf(values[1].text, sizeof(values[1].text), "%s", "/home/test-next");
    if (settings_save(file_manager, values, reason) != 0 ||
        !read_text(config_path, content, sizeof(content)) ||
        strstr(content, "[Other]\nshow_hidden=true\n") == NULL ||
        strstr(content, "start_location = \"/home/test-next\"\n") == NULL) {
        fprintf(stderr, "file manager save changed another section: %s\n", reason);
        return 0;
    }

    if (settings_load(sound, values) != 0) {
        fprintf(stderr, "sound language setting unexpectedly exists\n");
        return 0;
    }
    snprintf(values[0].text, sizeof(values[0].text), "%s", "Turkish");
    if (settings_save(sound, values, reason) != 0 ||
        settings_load(sound, values) != 1 ||
        strcmp(values[0].text, "Turkish") != 0) {
        fprintf(stderr, "sound language setting round trip failed: %s\n", reason);
        return 0;
    }

    if (settings_load(welcome, values) != 0) {
        fprintf(stderr, "Welcome state unexpectedly exists\n");
        return 0;
    }
    setting_value_set_bool(&values[0], 1, 0);
    if (settings_save(welcome, values, reason) != 0 ||
        settings_load(welcome, values) != 1 ||
        strcmp(values[0].text, "true") != 0) {
        fprintf(stderr, "Welcome settings round trip failed: %s\n", reason);
        return 0;
    }
    return 1;
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s CONFIG_HOME\n", argv[0]);
        return 2;
    }
    int okay = run_tests(argv[1]);
    if (!cleanup_config(argv[1])) {
        fprintf(stderr, "could not clean test configuration directory\n");
        okay = 0;
    }
    if (okay) {
        puts("GnuChanSettings integration tests passed");
    }
    return okay ? 0 : 1;
}
