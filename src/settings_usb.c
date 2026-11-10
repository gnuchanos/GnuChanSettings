#include "settings_usb.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static int valid_name(const char *name, int allow_dot) {
    if (!name || !name[0] || strlen(name) >= SETTINGS_USB_NAME_LENGTH) {
        return 0;
    }
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
        return 0;
    }
    for (const unsigned char *p = (const unsigned char *)name; *p; p++) {
        if (!isalnum(*p) && *p != '-' && *p != '_' &&
            !(allow_dot && *p == '.')) {
            return 0;
        }
    }
    return 1;
}

int settings_usb_valid_id(const char *id) {
    return valid_name(id, 1);
}

int settings_usb_valid_block(const char *block) {
    return valid_name(block, 1);
}

static int read_line(const char *path, char *value, size_t size) {
    FILE *file = fopen(path, "r");
    if (!file) {
        return -1;
    }
    if (!fgets(value, (int)size, file)) {
        fclose(file);
        return -1;
    }
    fclose(file);
    value[strcspn(value, "\r\n")] = '\0';
    return 0;
}

static void decode_mount_field(char *field) {
    char *read = field;
    char *write = field;
    while (*read) {
        if (read[0] == '\\' && read[1] == '0' && read[2] == '4' &&
            read[3] == '0') {
            *write++ = ' ';
            read += 4;
        } else if (read[0] == '\\' && read[1] == '0' && read[2] == '1' &&
                   read[3] == '1') {
            *write++ = '\t';
            read += 4;
        } else if (read[0] == '\\' && read[1] == '1' && read[2] == '3' &&
                   read[3] == '4') {
            *write++ = '\\';
            read += 4;
        } else {
            *write++ = *read++;
        }
    }
    *write = '\0';
}

static void find_mountpoint(const char *block, char *mountpoint,
                            size_t mountpoint_size) {
    char device_path[SETTINGS_USB_NAME_LENGTH + 8];
    snprintf(device_path, sizeof(device_path), "/dev/%s", block);
    FILE *mounts = fopen("/proc/self/mounts", "r");
    if (!mounts) {
        return;
    }
    char line[1024];
    while (fgets(line, sizeof(line), mounts)) {
        char source[SETTINGS_USB_MOUNT_LENGTH];
        char target[SETTINGS_USB_MOUNT_LENGTH];
        if (sscanf(line, "%255s %255s", source, target) != 2) {
            continue;
        }
        decode_mount_field(source);
        decode_mount_field(target);
        if (strcmp(source, device_path) == 0) {
            snprintf(mountpoint, mountpoint_size, "%s", target);
            break;
        }
    }
    fclose(mounts);
}

static int find_usb_parent(const char *block_path, char *id, size_t id_size,
                           char *product, size_t product_size,
                           char *control, size_t control_size) {
    char current[PATH_MAX];
    if (snprintf(current, sizeof(current), "%s", block_path) >=
        (int)sizeof(current)) {
        return 0;
    }
    while (strcmp(current, "/") != 0) {
        char vendor_path[PATH_MAX];
        char product_id_path[PATH_MAX];
        char product_path[PATH_MAX];
        char control_path[PATH_MAX];
        char vendor[32];
        char product_id[32];
        if (snprintf(vendor_path, sizeof(vendor_path), "%s/idVendor",
                     current) < (int)sizeof(vendor_path) &&
            snprintf(product_id_path, sizeof(product_id_path),
                     "%s/idProduct", current) <
                (int)sizeof(product_id_path) &&
            read_line(vendor_path, vendor, sizeof(vendor)) == 0 &&
            read_line(product_id_path, product_id, sizeof(product_id)) == 0) {
            const char *base = strrchr(current, '/');
            base = base ? base + 1 : current;
            size_t base_length = strlen(base);
            if (!settings_usb_valid_id(base) || base_length >= id_size) {
                return 0;
            }
            memcpy(id, base, base_length + 1);
            if (snprintf(product_path, sizeof(product_path), "%s/product",
                         current) < (int)sizeof(product_path) &&
                read_line(product_path, product, product_size) == 0) {
                for (char *p = product; *p; p++) {
                    if (!isprint((unsigned char)*p)) {
                        *p = ' ';
                    }
                }
            }
            if (!product[0]) {
                snprintf(product, product_size, "USB device %s:%s",
                         vendor, product_id);
            }
            if (snprintf(control_path, sizeof(control_path),
                         "%s/power/control", current) >=
                    (int)sizeof(control_path) ||
                read_line(control_path, control, control_size) != 0) {
                snprintf(control, control_size, "unsupported");
            }
            return 1;
        }
        char *slash = strrchr(current, '/');
        if (!slash || slash == current) {
            break;
        }
        *slash = '\0';
    }
    return 0;
}

static int same_device(const SettingsUsbDevice *left,
                       const SettingsUsbDevice *right) {
    return strcmp(left->id, right->id) == 0 &&
           strcmp(left->block, right->block) == 0;
}

int settings_usb_list(SettingsUsbDevice *devices, size_t capacity) {
    if (!devices || capacity == 0) {
        errno = EINVAL;
        return -1;
    }
    DIR *blocks = opendir("/sys/class/block");
    if (!blocks) {
        return -1;
    }

    size_t count = 0;
    struct dirent *entry;
    while ((entry = readdir(blocks)) != NULL && count < capacity) {
        if (!settings_usb_valid_block(entry->d_name)) {
            continue;
        }
        char sys_path[PATH_MAX];
        char resolved[PATH_MAX];
        char partition_path[PATH_MAX];
        if (snprintf(sys_path, sizeof(sys_path), "/sys/class/block/%s",
                     entry->d_name) >= (int)sizeof(sys_path) ||
            !realpath(sys_path, resolved)) {
            continue;
        }
        if (snprintf(partition_path, sizeof(partition_path),
                     "%s/partition", resolved) >=
                (int)sizeof(partition_path)) {
            continue;
        }
        struct stat st;
        int is_partition = stat(partition_path, &st) == 0;
        char removable_path[PATH_MAX];
        char removable[16] = "";
        if (snprintf(removable_path, sizeof(removable_path),
                     "%s/removable", resolved) < (int)sizeof(removable_path)) {
            (void)read_line(removable_path, removable, sizeof(removable));
        }
        if (!is_partition && strcmp(removable, "1") != 0) {
            continue;
        }

        SettingsUsbDevice candidate;
        memset(&candidate, 0, sizeof(candidate));
        if (!find_usb_parent(resolved, candidate.id, sizeof(candidate.id),
                             candidate.product, sizeof(candidate.product),
                             candidate.power_control,
                             sizeof(candidate.power_control))) {
            continue;
        }
        size_t block_length = strlen(entry->d_name);
        memcpy(candidate.block, entry->d_name, block_length + 1);
        find_mountpoint(candidate.block, candidate.mountpoint,
                        sizeof(candidate.mountpoint));
        int duplicate = 0;
        for (size_t i = 0; i < count; i++) {
            if (same_device(&devices[i], &candidate)) {
                duplicate = 1;
                break;
            }
        }
        if (!duplicate) {
            devices[count++] = candidate;
        }
    }
    closedir(blocks);

    DIR *usb_devices = opendir("/sys/bus/usb/devices");
    if (!usb_devices) {
        return count > 0 ? (int)count : -1;
    }
    while ((entry = readdir(usb_devices)) != NULL && count < capacity) {
        if (!settings_usb_valid_id(entry->d_name)) {
            continue;
        }
        char device_path[PATH_MAX];
        char resolved[PATH_MAX];
        if (snprintf(device_path, sizeof(device_path),
                     "/sys/bus/usb/devices/%s", entry->d_name) >=
                (int)sizeof(device_path) ||
            !realpath(device_path, resolved)) {
            continue;
        }
        SettingsUsbDevice candidate;
        memset(&candidate, 0, sizeof(candidate));
        if (!find_usb_parent(resolved, candidate.id, sizeof(candidate.id),
                             candidate.product, sizeof(candidate.product),
                             candidate.power_control,
                             sizeof(candidate.power_control)) ||
            strcmp(candidate.id, entry->d_name) != 0) {
            continue;
        }
        int present = 0;
        for (size_t i = 0; i < count; i++) {
            if (strcmp(devices[i].id, candidate.id) == 0) {
                present = 1;
                break;
            }
        }
        if (!present) {
            devices[count++] = candidate;
        }
    }
    closedir(usb_devices);
    return (int)count;
}

static int run_command(char *const arguments[], char *message,
                       size_t message_size) {
    message[0] = '\0';
    int output[2];
    if (pipe(output) != 0) {
        snprintf(message, message_size, "Could not start operation: %s",
                 strerror(errno));
        return -1;
    }
    pid_t child = fork();
    if (child < 0) {
        int error = errno;
        close(output[0]);
        close(output[1]);
        snprintf(message, message_size, "Could not start operation: %s",
                 strerror(error));
        return -1;
    }
    if (child == 0) {
        close(output[0]);
        dup2(output[1], STDOUT_FILENO);
        dup2(output[1], STDERR_FILENO);
        close(output[1]);
        execvp(arguments[0], arguments);
        dprintf(STDERR_FILENO, "%s: %s\n", arguments[0], strerror(errno));
        _exit(127);
    }
    close(output[1]);
    size_t used = 0;
    char chunk[256];
    ssize_t got;
    while ((got = read(output[0], chunk, sizeof(chunk))) > 0) {
        size_t available = message_size > used ? message_size - used - 1 : 0;
        size_t copy = (size_t)got < available ? (size_t)got : available;
        if (copy > 0) {
            memcpy(message + used, chunk, copy);
            used += copy;
            message[used] = '\0';
        }
    }
    close(output[0]);
    int status;
    while (waitpid(child, &status, 0) < 0) {
        if (errno != EINTR) {
            snprintf(message, message_size, "Could not wait for operation: %s",
                     strerror(errno));
            return -1;
        }
    }
    while (used > 0 && isspace((unsigned char)message[used - 1])) {
        message[--used] = '\0';
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        if (!message[0]) {
            snprintf(message, message_size, "Operation failed.");
        }
        return -1;
    }
    if (!message[0]) {
        snprintf(message, message_size, "Operation completed.");
    }
    return 0;
}

int settings_usb_action(const SettingsUsbDevice *device,
                        SettingsUsbAction action,
                        char *message, size_t message_size) {
    if (!device || !message || message_size == 0 ||
        !settings_usb_valid_id(device->id)) {
        if (message && message_size > 0) {
            snprintf(message, message_size, "Invalid USB device identity.");
        }
        return -1;
    }

    if (action == SETTINGS_USB_MOUNT) {
        if (!settings_usb_valid_block(device->block)) {
            snprintf(message, message_size,
                     "This USB device has no mountable storage volume.");
            return -1;
        }
        char device_path[SETTINGS_USB_NAME_LENGTH + 8];
        snprintf(device_path, sizeof(device_path), "/dev/%s", device->block);
        char *arguments[] = {"udisksctl", "mount", "-b", device_path, NULL};
        return run_command(arguments, message, message_size);
    }
    if (action == SETTINGS_USB_UNMOUNT) {
        if (!settings_usb_valid_block(device->block)) {
            snprintf(message, message_size,
                     "This USB device has no mountable storage volume.");
            return -1;
        }
        char device_path[SETTINGS_USB_NAME_LENGTH + 8];
        snprintf(device_path, sizeof(device_path), "/dev/%s", device->block);
        char *arguments[] = {"udisksctl", "unmount", "-b", device_path, NULL};
        return run_command(arguments, message, message_size);
    }
    if (action == SETTINGS_USB_DISABLE_AUTOSUSPEND ||
        action == SETTINGS_USB_ENABLE_AUTOSUSPEND) {
        char mode[5];
        char device_id[SETTINGS_USB_NAME_LENGTH];
        memcpy(device_id, device->id, strlen(device->id) + 1);
        snprintf(mode, sizeof(mode), "%s",
                 action == SETTINGS_USB_DISABLE_AUTOSUSPEND ? "on" : "auto");
        char *arguments[] = {"pkexec",
                             "/usr/local/libexec/gnuchan-settings-helper",
                             "usb-power", device_id, mode, NULL};
        return run_command(arguments, message, message_size);
    }
    snprintf(message, message_size, "Unsupported USB action.");
    return -1;
}
