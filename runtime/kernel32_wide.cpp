// UTF-16 kernel32 entry points. File operations share the ANSI shim's path seam.
#include "kernel32_internal.h"
#include "loader.h"
#include "memory.h"
#include "win32.h"
#include "../platform/os.h"
#include <algorithm>
#include <cstring>
#include <string>

namespace {
// gm_wstr produces valid UTF-8. Count UTF-16 units, including surrogate pairs.
uint32_t wide_units(const std::string &s) {
    uint32_t n = 0;
    for (unsigned char ch : s) {
        if ((ch & 0xc0) != 0x80)
            n += ch >= 0xf0 ? 2 : 1;
    }
    return n;
}

void k_CreateFileW(X86 *c) {
    create_file_named(c, gm_wstr(arg(c, 0)));
}
void k_GetFileAttributesW(X86 *c) {
    get_file_attributes_named(c, gm_wstr(arg(c, 0)));
}
void k_GetFileAttributesExW(X86 *c) {
    get_file_attributes_ex_named(c, gm_wstr(arg(c, 0)));
}
void k_SetFileAttributesW(X86 *c) {
    set_file_attributes_named(c, gm_wstr(arg(c, 0)));
}
void k_DeleteFileW(X86 *c) {
    delete_file_named(c, gm_wstr(arg(c, 0)));
}
void k_CopyFileW(X86 *c) {
    copy_file_named(c, gm_wstr(arg(c, 0)), gm_wstr(arg(c, 1)));
}
void k_CreateDirectoryW(X86 *c) {
    create_directory_named(c, gm_wstr(arg(c, 0)));
}
void k_RemoveDirectoryW(X86 *c) {
    remove_directory_named(c, gm_wstr(arg(c, 0)));
}
void k_FindFirstFileW(X86 *c) {
    if (!arg(c, 1) || !gm_valid(arg(c, 1), 592)) {
        set_last_error(87);
        set_eax(c, 0xffffffffu);
        return;
    }
    find_first_named(c, gm_wstr(arg(c, 0)), true);
}
void k_FindNextFileW(X86 *c) {
    if (!arg(c, 1) || !gm_valid(arg(c, 1), 592)) {
        set_last_error(87);
        set_eax(c, 0);
        return;
    }
    find_next(c, true);
}
void k_GetFullPathNameW(X86 *c) {
    std::string full = full_path_named(gm_wstr(arg(c, 0)));
    std::replace(full.begin(), full.end(), '/', '\\');
    uint32_t len = arg(c, 1), buf = arg(c, 2), pfile = arg(c, 3);
    uint32_t need = wide_units(full) + 1;
    if (!buf || len < need) {
        set_eax(c, need);
        return;
    }
    uint32_t n = gm_put_wstr(buf, full, len);
    if (pfile) {
        size_t slash = full.find_last_of('\\');
        wr32(pfile,
             buf + (slash == std::string::npos ? 0 : 2 * wide_units(full.substr(0, slash + 1))));
    }
    set_eax(c, n);
}
void k_GetSystemDirectoryW(X86 *c) {
    const std::string dir = "C:\\WINDOWS\\SYSTEM";
    uint32_t cap = arg(c, 1);
    set_eax(c, !arg(c, 0) || cap <= dir.size() ? (uint32_t)dir.size() + 1
                                               : gm_put_wstr(arg(c, 0), dir, cap));
}
void k_GetVolumeInformationW(X86 *c) {
    volume_information_named(c, gm_wstr(arg(c, 0)), true);
}
void k_GetDriveTypeW(X86 *c) {
    drive_type_named(c, gm_wstr(arg(c, 0)));
}
void k_GetLogicalDriveStringsW(X86 *c) {
    logical_drive_strings(c, true);
}
void k_GetDiskFreeSpaceW(X86 *c) {
    // Stable virtual-disk geometry; the arena does not expose host disk details.
    const uint32_t values[] = {8, 512, 262144, 524288};
    for (int i = 0; i < 4; ++i)
        if (uint32_t p = arg(c, i + 1))
            wr32(p, values[i]);
    set_eax(c, 1);
}
void k_QueryDosDeviceW(X86 *c) {
    uint32_t name = arg(c, 0), out = arg(c, 1), cap = arg(c, 2);
    std::string dev = gm_wstr(name);
    if (name && os_strcasecmp(dev.c_str(), "C:") != 0) {
        set_last_error(2);
        set_eax(c, 0);
        return;
    }
    std::string result = name ? "\\Device\\HarddiskVolume1" : "C:";
    uint32_t need = wide_units(result) + 2; // MULTI_SZ includes its extra NUL.
    if (!out || cap < need) {
        set_last_error(122);
        set_eax(c, 0);
        return;
    }
    gm_put_wstr(out, result, cap);
    wr16(out + (need - 1) * 2, 0);
    set_eax(c, need);
}

static const ImportShim g_kernel32_wide[] = {
    {"KERNEL32.dll", "CreateFileW", 7, k_CreateFileW},
    {"KERNEL32.dll", "FindFirstFileW", 2, k_FindFirstFileW},
    {"KERNEL32.dll", "FindNextFileW", 2, k_FindNextFileW},
    {"KERNEL32.dll", "GetFullPathNameW", 4, k_GetFullPathNameW},
    {"KERNEL32.dll", "GetFileAttributesW", 1, k_GetFileAttributesW},
    {"KERNEL32.dll", "GetFileAttributesExW", 3, k_GetFileAttributesExW},
    {"KERNEL32.dll", "SetFileAttributesW", 2, k_SetFileAttributesW},
    {"KERNEL32.dll", "DeleteFileW", 1, k_DeleteFileW},
    {"KERNEL32.dll", "CopyFileW", 3, k_CopyFileW},
    {"KERNEL32.dll", "CreateDirectoryW", 2, k_CreateDirectoryW},
    {"KERNEL32.dll", "RemoveDirectoryW", 1, k_RemoveDirectoryW},
    {"KERNEL32.dll", "GetSystemDirectoryW", 2, k_GetSystemDirectoryW},
    {"KERNEL32.dll", "GetVolumeInformationW", 8, k_GetVolumeInformationW},
    {"KERNEL32.dll", "GetDriveTypeW", 1, k_GetDriveTypeW},
    {"KERNEL32.dll", "GetLogicalDriveStringsW", 2, k_GetLogicalDriveStringsW},
    {"KERNEL32.dll", "GetDiskFreeSpaceW", 5, k_GetDiskFreeSpaceW},
    {"KERNEL32.dll", "QueryDosDeviceW", 3, k_QueryDosDeviceW},
};
} // namespace

void kernel32_wide_register() {
    imports_register(g_kernel32_wide, sizeof g_kernel32_wide / sizeof g_kernel32_wide[0]);
}
