/*
 * GnuChanSettings.c — the entry point.
 *
 * One command: open the control panel. Everything it does is done by the two
 * halves behind it — settings_ui.c for what a person does, settings_draw.c for
 * what they see — and this file is only the door.
 *
 * The settings of the panel ITSELF are not configurable and have no file: the
 * panel is the thing that edits the OTHER programs' files, and a settings
 * program whose own colours lived in a file it was editing would be a knot
 * worth not tying. Its palette is in settings_style.c, in one place.
 */
#include <stdio.h>
#include <string.h>

#include "settings_ui.h"
#include "gnuchan_lang.h"

#define PROGRAM_NAME    "GnuChanSettings"
#define PROGRAM_VERSION "1.0"

static void print_help(void) {
    printf("%s %s\n", PROGRAM_NAME, PROGRAM_VERSION);
    printf("\n");
    printf("%s\n", gnuchan_lang_t("settings_help"));
    printf("\n");
    printf("Usage:\n");
    printf("  %s              %s\n", PROGRAM_NAME,
           gnuchan_lang_t("settings_usage_open"));
    printf("  %s --help       %s\n", PROGRAM_NAME,
           gnuchan_lang_t("settings_usage_help"));
    printf("  %s --version    %s\n", PROGRAM_NAME,
           gnuchan_lang_t("settings_usage_version"));
    printf("\n");
    printf("%s\n", gnuchan_lang_t("settings_help_note"));
}

int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_help();
            return 0;
        }
        if (strcmp(argv[i], "--version") == 0 || strcmp(argv[i], "-v") == 0) {
            printf("%s %s\n", PROGRAM_NAME, PROGRAM_VERSION);
            return 0;
        }
        char errmsg[256];
        snprintf(errmsg, sizeof(errmsg),
                 gnuchan_lang_t("error_unknown_argument"),
                 PROGRAM_NAME, argv[i]);
        fprintf(stderr, "%s\n", errmsg);
        fprintf(stderr, "run '%s --help' for the usage\n", PROGRAM_NAME);
        return 2;
    }

    SettingsUi ui;
    gnuchan_lang_init();
    if (settings_ui_open(&ui) != 0) {
        return 1;
    }

    settings_ui_run(&ui);
    settings_ui_close(&ui);
    return 0;
}
