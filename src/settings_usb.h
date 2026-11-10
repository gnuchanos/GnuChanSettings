#ifndef GNUCHANSETTINGS_USB_H
#define GNUCHANSETTINGS_USB_H

#include <stddef.h>

#define SETTINGS_USB_MAX_DEVICES 64
#define SETTINGS_USB_NAME_LENGTH 128
#define SETTINGS_USB_MOUNT_LENGTH 256

typedef struct SettingsUsbDevice {
    char id[SETTINGS_USB_NAME_LENGTH];
    char product[SETTINGS_USB_NAME_LENGTH];
    char block[SETTINGS_USB_NAME_LENGTH];
    char mountpoint[SETTINGS_USB_MOUNT_LENGTH];
    char power_control[16];
} SettingsUsbDevice;

typedef enum SettingsUsbAction {
    SETTINGS_USB_MOUNT,
    SETTINGS_USB_UNMOUNT,
    SETTINGS_USB_DISABLE_AUTOSUSPEND,
    SETTINGS_USB_ENABLE_AUTOSUSPEND
} SettingsUsbAction;

int settings_usb_valid_id(const char *id);
int settings_usb_valid_block(const char *block);
int settings_usb_list(SettingsUsbDevice *devices, size_t capacity);
int settings_usb_action(const SettingsUsbDevice *device,
                        SettingsUsbAction action,
                        char *message, size_t message_size);

#endif
