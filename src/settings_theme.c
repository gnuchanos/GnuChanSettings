/*
 * settings_theme.c — palette presets and their config-file application.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "settings_theme.h"
#include "settings_store.h"

const char *const settings_theme_presets[] = {
    "Purple", "Black", "White", "Gray", "Blue",
    "Green", "Orange", "Yellow", "Red", "Pink", NULL
};

typedef struct Rgb {
    unsigned char red;
    unsigned char green;
    unsigned char blue;
} Rgb;

typedef struct ThemeSpec {
    const char *name;
    const char *accent;
    int light;
    int neutral;
} ThemeSpec;

static const ThemeSpec theme_specs[] = {
    { "Purple", "#c77dff", 0, 0 },
    { "Black",  "#c4c4c4", 0, 1 },
    { "White",  "#414141", 1, 1 },
    { "Gray",   "#a0a0a0", 0, 1 },
    { "Blue",   "#55a5ff", 0, 0 },
    { "Green",  "#5bd27a", 0, 0 },
    { "Orange", "#ff9c52", 0, 0 },
    { "Yellow", "#e7c43c", 0, 0 },
    { "Red",    "#ff6678", 0, 0 },
    { "Pink",   "#ff78b7", 0, 0 },
};

static int hex_digit(char value) {
    if (value >= '0' && value <= '9') {
        return value - '0';
    }
    if (value >= 'a' && value <= 'f') {
        return value - 'a' + 10;
    }
    if (value >= 'A' && value <= 'F') {
        return value - 'A' + 10;
    }
    return -1;
}

static int parse_rgb(const char *text, Rgb *colour) {
    if (!text || text[0] != '#' || strlen(text) != 7) {
        return 0;
    }
    unsigned char *channels[] = {
        &colour->red, &colour->green, &colour->blue
    };
    for (int i = 0; i < 3; i++) {
        int high = hex_digit(text[1 + i * 2]);
        int low = hex_digit(text[2 + i * 2]);
        if (high < 0 || low < 0) {
            return 0;
        }
        *channels[i] = (unsigned char)(high * 16 + low);
    }
    return 1;
}

static unsigned char mix_channel(unsigned char first, unsigned char second,
                                 int second_percent) {
    return (unsigned char)(((int)first * (100 - second_percent) +
                            (int)second * second_percent + 50) / 100);
}

static Rgb mix(Rgb first, Rgb second, int second_percent) {
    Rgb result = {
        mix_channel(first.red, second.red, second_percent),
        mix_channel(first.green, second.green, second_percent),
        mix_channel(first.blue, second.blue, second_percent),
    };
    return result;
}

static void format_rgb(char out[8], Rgb colour) {
    snprintf(out, 8, "#%02x%02x%02x",
             colour.red, colour.green, colour.blue);
}

static void set_palette_colour(char out[8], const char *base,
                               const char *neutral, int weight) {
    Rgb base_rgb;
    Rgb neutral_rgb;
    if (!parse_rgb(base, &base_rgb) || !parse_rgb(neutral, &neutral_rgb)) {
        snprintf(out, 8, "%s", neutral);
        return;
    }
    format_rgb(out, mix(neutral_rgb, base_rgb, weight));
}

int settings_theme_palette(const char *name, SettingsPalette *palette) {
    if (!name || !palette) {
        return -1;
    }
    const ThemeSpec *spec = NULL;
    for (size_t i = 0; i < sizeof(theme_specs) / sizeof(theme_specs[0]); i++) {
        if (strcmp(theme_specs[i].name, name) == 0) {
            spec = &theme_specs[i];
            break;
        }
    }
    if (!spec) {
        return -1;
    }

    if (spec->light) {
        set_palette_colour(palette->background, spec->accent, "#ffffff", 3);
        set_palette_colour(palette->sidebar, spec->accent, "#f0f0f0", 6);
        set_palette_colour(palette->panel, spec->accent, "#e8e8e8", 8);
        set_palette_colour(palette->field, spec->accent, "#ffffff", 1);
        set_palette_colour(palette->edge, spec->accent, "#666666", 12);
        set_palette_colour(palette->text, spec->accent, "#171717", 4);
        set_palette_colour(palette->muted, spec->accent, "#5a5a5a", 8);
        set_palette_colour(palette->accent, spec->accent, "#333333", 20);
        set_palette_colour(palette->accent_dim, spec->accent, "#777777", 20);
        set_palette_colour(palette->selection, spec->accent, "#d4d4d4", 10);
        return 0;
    }

    const char *background = spec->neutral ? "#101010" : "#100b14";
    const char *sidebar = spec->neutral ? "#151515" : "#140e1b";
    const char *panel = spec->neutral ? "#202020" : "#211827";
    const char *field = spec->neutral ? "#292929" : "#2b2132";
    const char *edge = spec->neutral ? "#666666" : "#72647b";
    const char *text = spec->neutral ? "#f5f5f5" : "#f3f0f5";
    const char *muted = spec->neutral ? "#b8b8b8" : "#b9b3bf";
    const char *accent_dim = spec->neutral ? "#777777" : "#77717e";
    const char *selection = spec->neutral ? "#383838" : "#34303a";

    set_palette_colour(palette->background, spec->accent, background, 14);
    set_palette_colour(palette->sidebar, spec->accent, sidebar, 18);
    set_palette_colour(palette->panel, spec->accent, panel, 22);
    set_palette_colour(palette->field, spec->accent, field, 26);
    set_palette_colour(palette->edge, spec->accent, edge, 48);
    set_palette_colour(palette->text, spec->accent, text, 10);
    set_palette_colour(palette->muted, spec->accent, muted, 28);
    snprintf(palette->accent, sizeof(palette->accent), "%s", spec->accent);
    set_palette_colour(palette->accent_dim, spec->accent, accent_dim, 70);
    set_palette_colour(palette->selection, spec->accent, selection, 68);

    /* Keep the established purple look for existing installations. */
    if (strcmp(spec->name, "Purple") == 0) {
        snprintf(palette->background, sizeof(palette->background), "#1a0b2e");
        snprintf(palette->sidebar, sizeof(palette->sidebar), "#140826");
        snprintf(palette->panel, sizeof(palette->panel), "#241033");
        snprintf(palette->field, sizeof(palette->field), "#241033");
        snprintf(palette->edge, sizeof(palette->edge), "#7b2cbf");
        snprintf(palette->text, sizeof(palette->text), "#e0c3fc");
        snprintf(palette->muted, sizeof(palette->muted), "#9d7bba");
        snprintf(palette->accent, sizeof(palette->accent), "#c77dff");
        snprintf(palette->accent_dim, sizeof(palette->accent_dim), "#7b2cbf");
        snprintf(palette->selection, sizeof(palette->selection), "#32143f");
    }
    return 0;
}

static void lowercase_key(char out[128], const char *key) {
    size_t i = 0;
    if (!key) {
        out[0] = '\0';
        return;
    }
    while (key[i] && i + 1 < 128) {
        char ch = key[i];
        out[i] = ch >= 'A' && ch <= 'Z' ? (char)(ch - 'A' + 'a') : ch;
        i++;
    }
    out[i] = '\0';
}

const char *settings_theme_color_for(const SettingDef *setting,
                                     const SettingsPalette *palette) {
    if (!setting || !palette || setting->type != SETTING_COLOR) {
        return NULL;
    }
    char key[128];
    lowercase_key(key, setting->key);

    if (strstr(key, "muted")) {
        return palette->muted;
    }
    if (strcmp(key, "text") == 0 || strcmp(key, "title") == 0 ||
        strcmp(key, "body") == 0 || strcmp(key, "barforeground") == 0 ||
        strcmp(key, "select_text") == 0) {
        return palette->text;
    }
    if (strstr(key, "field")) {
        return palette->field;
    }
    if (strstr(key, "backgroundalt") || strstr(key, "barbackground") ||
        strstr(key, "iconbackground") || strcmp(key, "panel") == 0 ||
        strcmp(key, "badge") == 0 ||
        strcmp(key, "set_inactive_window_panel_color") == 0) {
        return palette->panel;
    }
    if (strcmp(key, "background") == 0 ||
        strcmp(key, "backgroundcolor") == 0 ||
        strcmp(key, "background_color") == 0 ||
        strcmp(key, "imagebackground") == 0) {
        return palette->background;
    }
    if (strstr(key, "moved_window_border") || strcmp(key, "selection") == 0 ||
        strcmp(key, "select_border") == 0) {
        return palette->selection;
    }
    if (strstr(key, "accentdim")) {
        return palette->accent_dim;
    }
    if (strstr(key, "accent") || strstr(key, "primarycolor") ||
        strstr(key, "cursor") ||
        strcmp(key, "set_active_window_border_color") == 0) {
        return palette->accent;
    }
    if (strstr(key, "edge") || strstr(key, "frame") ||
        strstr(key, "border") || strcmp(key, "cell_border") == 0) {
        return palette->edge;
    }
    return NULL;
}

int settings_theme_apply(const char *name, char *reason) {
    SettingsPalette palette;
    if (settings_theme_palette(name, &palette) != 0) {
        if (reason) {
            snprintf(reason, SETTINGS_TEXT_LENGTH, "Unknown theme colour: %s",
                     name ? name : "");
        }
        return -1;
    }

    int app_count = 0;
    const AppDef *apps = settings_apps(&app_count);
    for (int app_index = 0; app_index < app_count; app_index++) {
        const AppDef *app = &apps[app_index];
        if (strcmp(app->dir, "GnuChanSettings") == 0 &&
            strcmp(app->file, "system.conf") == 0) {
            continue;
        }

        SettingValue values[SETTINGS_MAX_ROWS];
        int loaded = settings_load(app, values);
        if (loaded < 0) {
            if (reason) {
                snprintf(reason, SETTINGS_TEXT_LENGTH,
                         "Could not read %s settings.", app->name);
            }
            return -1;
        }

        int themed = 0;
        for (int row = 0; row < app->setting_count; row++) {
            const char *colour =
                settings_theme_color_for(&app->settings[row], &palette);
            if (!colour) {
                continue;
            }
            themed = 1;
            snprintf(values[row].text, sizeof(values[row].text), "%s", colour);
        }
        if (!themed) {
            continue;
        }
        if (settings_save(app, values, reason) != 0) {
            if (reason) {
                char detail[SETTINGS_TEXT_LENGTH];
                snprintf(detail, sizeof(detail), "%s", reason);
                snprintf(reason, SETTINGS_TEXT_LENGTH,
                         "Could not apply the theme to %s: %.190s",
                         app->name, detail);
            }
            return -1;
        }
    }
    if (reason) {
        reason[0] = '\0';
    }
    return 0;
}
