#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "settings_theme.h"
#include "settings_store.h"

#define CHECK(condition, message) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL: %s\n", message); \
        return 1; \
    } \
} while (0)

static int has_text(const char *path, const char *text) {
    FILE *file = fopen(path, "r");
    if (!file) {
        return 0;
    }
    char line[512];
    int found = 0;
    while (fgets(line, sizeof(line), file)) {
        if (strstr(line, text)) {
            found = 1;
            break;
        }
    }
    fclose(file);
    return found;
}

int main(void) {
    SettingsPalette purple;
    SettingsPalette blue;
    CHECK(settings_theme_palette("Purple", &purple) == 0,
          "Purple preset should be available");
    CHECK(strcmp(purple.background, "#1a0b2e") == 0,
          "Purple should preserve the current Settings background");
    CHECK(settings_theme_palette("Blue", &blue) == 0,
          "Blue preset should be available");
    CHECK(strcmp(blue.accent, purple.accent) != 0,
          "Different presets should have different accents");
    CHECK(settings_theme_palette("not-a-preset", &blue) != 0,
          "Unknown presets should be rejected");
    SettingDef connected = { .key = "Connected", .type = SETTING_COLOR };
    SettingDef background = { .key = "Background", .type = SETTING_COLOR };
    CHECK(settings_theme_color_for(&connected, &blue) == NULL,
          "semantic connection indicators should retain their own colour");
    CHECK(strcmp(settings_theme_color_for(&background, &blue),
                 blue.background) == 0,
          "background roles should use the selected palette");

    int app_count = 0;
    const AppDef *apps = settings_apps(&app_count);
    int theme_setting_found = 0;
    for (int app_index = 0; app_index < app_count; app_index++) {
        if (strcmp(apps[app_index].dir, "GnuChanSettings") != 0 ||
            strcmp(apps[app_index].file, "system.conf") != 0) {
            continue;
        }
        for (int row = 0; row < apps[app_index].setting_count; row++) {
            const SettingDef *setting = &apps[app_index].settings[row];
            if (strcmp(setting->key, "ThemeColor") == 0) {
                theme_setting_found = setting->type == SETTING_CHOICE &&
                                      strcmp(setting->fallback, "Purple") == 0 &&
                                      settings_choice_count(setting) == 10;
            }
        }
    }
    CHECK(theme_setting_found,
          "System should expose and persist the palette preset choice");

    const char *config_root = getenv("GNUCHAN_THEME_TEST_CONFIG");
    CHECK(config_root && config_root[0], "test config root must be set");
    CHECK(setenv("XDG_CONFIG_HOME", config_root, 1) == 0,
          "test should isolate XDG_CONFIG_HOME");

    char app_dir[512];
    char config_path[640];
    snprintf(app_dir, sizeof(app_dir), "%s/GnuChanDock", config_root);
    CHECK(mkdir(app_dir, 0700) == 0, "create a fixture config directory");
    snprintf(config_path, sizeof(config_path), "%s/GnuChanDock.conf", app_dir);
    FILE *fixture = fopen(config_path, "w");
    CHECK(fixture != NULL, "create a fixture config");
    fputs("# hand-written setting\n"
          "Background = \"#112233\"\n"
          "Accent = \"#445566\"\n"
          "Gap = 77\n"
          "Keep = preserved\n", fixture);
    fclose(fixture);

    char reason[SETTINGS_TEXT_LENGTH];
    CHECK(settings_theme_apply("Blue", reason) == 0, reason);
    CHECK(has_text(config_path, blue.background),
          "the preset should update a background color");
    CHECK(has_text(config_path, blue.accent),
          "the preset should update an accent color");
    CHECK(has_text(config_path, "Gap = 77"),
          "non-colour settings should remain unchanged");
    CHECK(has_text(config_path, "Keep = preserved"),
          "unknown config lines should remain unchanged");
    CHECK(has_text(config_path, "# hand-written setting"),
          "comments should remain unchanged");

    puts("theme presets and config application passed");
    return 0;
}
