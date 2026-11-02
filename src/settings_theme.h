/*
 * settings_theme.h — coordinated palettes for the desktop applications.
 */
#ifndef GNUCHANSETTINGS_THEME_H
#define GNUCHANSETTINGS_THEME_H

#include "settings_types.h"

typedef struct SettingsPalette {
    char background[8];
    char sidebar[8];
    char panel[8];
    char field[8];
    char edge[8];
    char text[8];
    char muted[8];
    char accent[8];
    char accent_dim[8];
    char selection[8];
} SettingsPalette;

extern const char *const settings_theme_presets[];

/* Resolve a preset name to a coordinated set of role colours. */
int settings_theme_palette(const char *name, SettingsPalette *palette);

/* Return the palette colour for a known themed setting, or NULL for semantic
   status colours which should retain their meaning (danger, connected, etc.). */
const char *settings_theme_color_for(const SettingDef *setting,
                                     const SettingsPalette *palette);

/* Apply the palette to every Settings-managed application config. */
int settings_theme_apply(const char *name, char *reason);

#endif /* GNUCHANSETTINGS_THEME_H */
