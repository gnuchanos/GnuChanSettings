#include <errno.h>
#include <stdio.h>

#include "settings_usb.h"

int main(void) {
    if (!settings_usb_valid_id("1-2.3") ||
        !settings_usb_valid_id("usb-device_1") ||
        settings_usb_valid_id("") ||
        settings_usb_valid_id(".") ||
        settings_usb_valid_id("..") ||
        settings_usb_valid_id("../1-2") ||
        settings_usb_valid_id("1-2:1.0") ||
        settings_usb_valid_id("1/2")) {
        fprintf(stderr, "USB device identity validation failed\n");
        return 1;
    }
    if (!settings_usb_valid_block("sda1") ||
        !settings_usb_valid_block("nvme0n1p2") ||
        settings_usb_valid_block("../sda") ||
        settings_usb_valid_block("sda;mount")) {
        fprintf(stderr, "USB block device name validation failed\n");
        return 1;
    }
    SettingsUsbDevice devices[SETTINGS_USB_MAX_DEVICES];
    int count = settings_usb_list(devices, SETTINGS_USB_MAX_DEVICES);
    if (count < 0 && errno != ENOENT) {
        perror("USB device enumeration failed");
        return 1;
    }
    for (int i = 0; i < count; i++) {
        if (!settings_usb_valid_id(devices[i].id) ||
            (devices[i].block[0] &&
             !settings_usb_valid_block(devices[i].block))) {
            fprintf(stderr, "USB enumeration returned an unsafe identity\n");
            return 1;
        }
    }
    puts("USB identity checks passed.");
    return 0;
}
