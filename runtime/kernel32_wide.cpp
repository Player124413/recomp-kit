// UTF-16 kernel32 entry points. File operations share the ANSI shim's path seam.
#include "kernel32_internal.h"
#include "loader.h"
#include "memory.h"
#include "win32.h"
#include "../platform/os.h"
#include <algorithm>
#include <cstring>
#include <string>
#include <vector>
#include <ctime>

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

// INI files are read through the overlay and rewritten through its write tier.
// Preserve unrelated lines, comments and existing UTF-16LE encoding.
struct ProfileFile {
    std::vector<std::string> lines;
    bool utf16 = false;
};
std::string trim(std::string s) {
    size_t first = s.find_first_not_of(" \t\r\n"), last = s.find_last_not_of(" \t\r\n");
    return first == std::string::npos ? "" : s.substr(first, last - first + 1);
}
bool equal_name(const std::string &a, const std::string &b) {
    return os_strcasecmp(a.c_str(), b.c_str()) == 0;
}
bool profile_read(const std::string &name, ProfileFile &ini) {
    std::string path = win32_host_path_op(name, WIN32_FILE_READ);
    if (path.empty())
        return true; // A missing INI starts empty.
    FILE *f = fopen(path.c_str(), "rb");
    if (!f)
        return false;
    std::string text;
    char chunk[4096];
    size_t n;
    while ((n = fread(chunk, 1, sizeof chunk, f)))
        text.append(chunk, n);
    bool ok = !ferror(f);
    fclose(f);
    if (!ok)
        return false;
    if (text.size() >= 2 && (uint8_t)text[0] == 0xff && (uint8_t)text[1] == 0xfe) {
        ini.utf16 = true;
        if (text.size() > GUEST_SIZE)
            return false;
        uint32_t tmp = heap_alloc((uint32_t)text.size() + 2, true);
        if (!tmp)
            return false;
        memcpy(g_mem + tmp, text.data() + 2, text.size() - 2);
        text = gm_wstr(tmp, text.size() / 2);
        heap_free(tmp);
    } else if (text.compare(0, 3, "\xef\xbb\xbf") == 0)
        text.erase(0, 3);
    size_t start = 0;
    while (start < text.size()) {
        size_t end = text.find('\n', start);
        std::string line = text.substr(start, end == std::string::npos ? end : end - start);
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        ini.lines.push_back(line);
        if (end == std::string::npos)
            break;
        start = end + 1;
    }
    return true;
}
// Return a section heading or a key/value pair without interpreting comments.
bool profile_section(const std::string &line, std::string &section) {
    std::string s = trim(line);
    if (s.size() < 2 || s.front() != '[' || s.back() != ']')
        return false;
    section = trim(s.substr(1, s.size() - 2));
    return true;
}
bool profile_key(const std::string &line, std::string &key, std::string &value) {
    std::string s = trim(line);
    size_t eq = s.find('=');
    if (s.empty() || s.front() == ';' || s.front() == '#' || eq == std::string::npos)
        return false;
    key = trim(s.substr(0, eq));
    value = trim(s.substr(eq + 1));
    if (value.size() >= 2 && (value.front() == '"' || value.front() == '\'') &&
        value.back() == value.front())
        value = value.substr(1, value.size() - 2);
    return !key.empty();
}
void k_GetPrivateProfileStringW(X86 *c) {
    std::string section = gm_wstr(arg(c, 0)), key = gm_wstr(arg(c, 1));
    uint32_t out = arg(c, 3), cap = arg(c, 4);
    ProfileFile ini;
    if (!profile_read(gm_wstr(arg(c, 5)), ini)) {
        set_last_error(5);
        set_eax(c, 0);
        return;
    }
    bool multi = !arg(c, 0) || !arg(c, 1);
    std::string result = trim(gm_wstr(arg(c, 2))), current, k, value;
    std::vector<std::string> names;
    for (const std::string &line : ini.lines) {
        if (profile_section(line, current)) {
            if (!arg(c, 0))
                names.push_back(current);
        } else if (equal_name(current, section) && profile_key(line, k, value)) {
            if (!arg(c, 1))
                names.push_back(k);
            else if (equal_name(k, key)) {
                result = value;
                break;
            }
        }
    }
    if (!out || cap == 0) {
        set_eax(c, 0);
        return;
    }
    if (!multi) {
        set_eax(c, gm_put_wstr(out, result, cap));
        return;
    }
    // MULTI_SZ sizes include each name's terminator, excluding the final one.
    uint32_t used = 0;
    wr16(out, 0);
    if (cap == 1) {
        set_eax(c, 0);
        return;
    }
    for (const auto &name : names) {
        uint32_t need = wide_units(name) + 1;
        if (need >= cap - used) {
            gm_put_wstr(out + used * 2, name, cap - used - 1);
            wr16(out + (cap - 2) * 2, 0);
            wr16(out + (cap - 1) * 2, 0);
            set_eax(c, cap - 2);
            return;
        }
        used += gm_put_wstr(out + used * 2, name, cap - used) + 1;
    }
    wr16(out + used * 2, 0);
    if (used == 0)
        wr16(out + 2, 0);
    set_eax(c, used);
}
void k_WritePrivateProfileStringW(X86 *c) {
    if (!arg(c, 0)) {
        set_eax(c, !arg(c, 1) && !arg(c, 2));
        return;
    } // cache flush
    ProfileFile ini;
    std::string name = gm_wstr(arg(c, 3));
    if (name.empty() || !profile_read(name, ini)) {
        set_last_error(5);
        set_eax(c, 0);
        return;
    }
    std::string section = gm_wstr(arg(c, 0)), key = gm_wstr(arg(c, 1)), current, k, value;
    std::string replacement = key + "=" + gm_wstr(arg(c, 2));
    std::vector<std::string> lines;
    bool inside = false, found_section = false, found_key = false;
    for (const auto &line : ini.lines) {
        if (profile_section(line, current)) {
            if (inside && !found_key && arg(c, 1) && arg(c, 2)) {
                lines.push_back(replacement);
                found_key = true;
            }
            inside = equal_name(current, section);
            if (inside)
                found_section = true;
        } else if (inside && profile_key(line, k, value) && equal_name(k, key)) {
            if (!found_key && arg(c, 2))
                lines.push_back(replacement);
            found_key = true;
            continue;
        }
        if (!(inside && !arg(c, 1)))
            lines.push_back(line);
    }
    if (!found_key && arg(c, 1) && arg(c, 2)) {
        if (!found_section)
            lines.push_back("[" + section + "]");
        lines.push_back(replacement);
    }
    std::string text;
    for (const auto &line : lines)
        text += line + "\r\n";
    if (ini.utf16) {
        if (text.size() > GUEST_SIZE / 2 - 1) {
            set_eax(c, 0);
            return;
        }
        uint32_t cap = (uint32_t)text.size() + 1, tmp = heap_alloc(cap * 2, true);
        if (!tmp) {
            set_eax(c, 0);
            return;
        }
        uint32_t n = gm_put_wstr(tmp, text, cap);
        text = std::string("\xff\xfe", 2) + std::string((char *)g_mem + tmp, n * 2);
        heap_free(tmp);
    }
    std::string path = win32_host_path_op(name, WIN32_FILE_WRITE);
    FILE *f = path.empty() ? nullptr : fopen(path.c_str(), "wb");
    if (!f) {
        set_last_error(5);
        set_eax(c, 0);
        return;
    }
    bool ok = fwrite(text.data(), 1, text.size(), f) == text.size();
    if (fclose(f) != 0)
        ok = false;
    win32_invalidate_dir_cache();
    set_eax(c, ok ? 1 : 0);
}

void k_CreateEventW(X86 *c) {
    create_event_named(c, gm_wstr(arg(c, 3)));
}
void k_CreateMutexW(X86 *c) {
    create_mutex_named(c, gm_wstr(arg(c, 2)));
}
void k_OpenMutexW(X86 *c) {
    open_mutex_named(c, gm_wstr(arg(c, 2)));
}
void k_CreateFileMappingW(X86 *c) {
    create_mapping_named(c, gm_wstr(arg(c, 5)));
}

uint32_t wstrlen(uint32_t p) {
    uint32_t n = 0;
    if (p)
        while (n < (GUEST_SIZE - std::min(p, GUEST_SIZE)) / 2 && rd16(p + n * 2))
            ++n;
    return n;
}
void k_lstrlenW(X86 *c) {
    set_eax(c, wstrlen(arg(c, 0)));
}
void k_lstrcatW(X86 *c) {
    uint32_t dest = arg(c, 0), src = arg(c, 1), n = wstrlen(dest), extra = wstrlen(src);
    if (dest && gm_valid(dest, n * 2) && extra < (GUEST_SIZE - dest) / 2 - n) {
        // memmove also preserves raw UTF-16 units when source and destination overlap.
        if (src)
            memmove(g_mem + dest + n * 2, g_mem + src, extra * 2);
        wr16(dest + (n + extra) * 2, 0);
    }
    set_eax(c, dest);
}
void k_FormatMessageW(X86 *c) {
    uint32_t flags = arg(c, 0), out = arg(c, 4), cap = arg(c, 5);
    if (!(flags & 0x1000) || (flags & (0x400 | 0x800)) || !out) {
        set_last_error(87);
        set_eax(c, 0);
        return;
    }
    std::string text = "Error " + std::to_string(arg(c, 2));
    uint32_t need = wide_units(text) + 1;
    if (flags & 0x100) { // FORMAT_MESSAGE_ALLOCATE_BUFFER
        cap = std::max(cap, need);
        if (cap > GUEST_SIZE / 2 || !gm_valid(out, 4)) {
            set_eax(c, 0);
            return;
        }
        uint32_t buffer = heap_alloc(cap * 2, true);
        if (!buffer) {
            set_last_error(8);
            set_eax(c, 0);
            return;
        }
        wr32(out, buffer);
        out = buffer;
    } else if (cap < need || !gm_valid(out, need * 2)) {
        set_last_error(122);
        set_eax(c, 0);
        return;
    }
    set_eax(c, gm_put_wstr(out, text, cap));
}
void k_OutputDebugStringW(X86 *c) {
    LOGV("OutputDebugStringW: %s", gm_wstr(arg(c, 0)).c_str());
    set_eax(c, 0);
}
// FILETIME is unsigned 100 ns ticks since 1601, SYSTEMTIME is eight WORDs.
// Keep conversion in UTC and use the platform time seam on every host.
bool filetime_fields(uint32_t p, struct tm &t, uint16_t &ms) {
    if (!p || !gm_valid(p, 8))
        return false;
    uint64_t ticks = (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32);
    if (ticks >> 63)
        return false;
    int64_t seconds = (int64_t)(ticks / 10000000ull) - 11644473600ll;
    ms = (uint16_t)((ticks % 10000000ull) / 10000);
    return os_gmtime(seconds, &t) == 0;
}
void k_FileTimeToLocalFileTime(X86 *c) {
    uint32_t src = arg(c, 0), dst = arg(c, 1);
    bool ok = src && dst && gm_valid(src, 8) && gm_valid(dst, 8);
    if (ok)
        memmove(g_mem + dst, g_mem + src, 8); // The virtual machine uses UTC.
    set_eax(c, ok ? 1 : 0);
}
void k_FileTimeToSystemTime(X86 *c) {
    struct tm t{};
    uint16_t ms = 0;
    uint32_t out = arg(c, 1);
    if (!out || !gm_valid(out, 16) || !filetime_fields(arg(c, 0), t, ms)) {
        set_last_error(87);
        set_eax(c, 0);
        return;
    }
    const uint16_t fields[] = {(uint16_t)(t.tm_year + 1900), (uint16_t)(t.tm_mon + 1),
                               (uint16_t)t.tm_wday,          (uint16_t)t.tm_mday,
                               (uint16_t)t.tm_hour,          (uint16_t)t.tm_min,
                               (uint16_t)t.tm_sec,           ms};
    for (uint32_t i = 0; i < 8; ++i)
        wr16(out + i * 2, fields[i]);
    set_eax(c, 1);
}
void k_FileTimeToDosDateTime(X86 *c) {
    struct tm t{};
    uint16_t ms = 0;
    uint32_t date = arg(c, 1), time = arg(c, 2);
    if (!date || !time || !gm_valid(date, 2) || !gm_valid(time, 2) ||
        !filetime_fields(arg(c, 0), t, ms) || t.tm_year < 80 || t.tm_year > 207) {
        set_last_error(87);
        set_eax(c, 0);
        return;
    }
    wr16(date, (uint16_t)(((t.tm_year - 80) << 9) | ((t.tm_mon + 1) << 5) | t.tm_mday));
    wr16(time, (uint16_t)((t.tm_hour << 11) | (t.tm_min << 5) | (t.tm_sec / 2)));
    set_eax(c, 1);
}
void k_GetDateFormatW(X86 *c) {
    uint32_t input = arg(c, 2), out = arg(c, 4), cap = arg(c, 5);
    int year, month, day;
    if (input) {
        if (!gm_valid(input, 16)) {
            set_eax(c, 0);
            return;
        }
        year = rd16(input);
        month = rd16(input + 2);
        day = rd16(input + 6);
    } else {
        struct tm t{};
        if (os_localtime((int64_t)(os_wall_time_us() / 1000000), &t) != 0) {
            set_eax(c, 0);
            return;
        }
        year = t.tm_year + 1900;
        month = t.tm_mon + 1;
        day = t.tm_mday;
    }
    if (month < 1 || month > 12 || day < 1 || day > 31) {
        set_last_error(87);
        set_eax(c, 0);
        return;
    }
    char text[32];
    snprintf(text, sizeof text, "%04d-%02d-%02d", year, month, day);
    uint32_t need = (uint32_t)strlen(text) + 1;
    if (cap == 0) {
        set_eax(c, need);
        return;
    }
    if (!out || cap < need) {
        set_last_error(122);
        set_eax(c, 0);
        return;
    }
    set_eax(c, gm_put_wstr(out, text, cap) + 1);
}

uint32_t g_thread_lcid = 0x0409; // The runtime exposes one process-wide locale.
void k_GetThreadLocale(X86 *c) {
    set_eax(c, g_thread_lcid);
}
void k_SetThreadLocale(X86 *c) {
    g_thread_lcid = arg(c, 0);
    set_eax(c, 1);
}
void k_GetUserDefaultUILanguage(X86 *c) {
    set_eax(c, 0x0409);
}
void k_GetSystemDefaultUILanguage(X86 *c) {
    set_eax(c, 0x0409);
}
void k_IsDBCSLeadByteEx(X86 *c) {
    set_eax(c, 0);
}
void k_GetConsoleCP(X86 *c) {
    set_eax(c, 437);
}
void k_GetConsoleOutputCP(X86 *c) {
    set_eax(c, 437);
}
// The callback receives temporary guest storage. guest_call dispatches through
// recomp_call and restores the caller's registers/stack; no host lock is held.
void enumerate_text(X86 *c, const char *text) {
    uint32_t callback = arg(c, 0);
    if (!callback) {
        set_last_error(87);
        set_eax(c, 0);
        return;
    }
    uint32_t tmp = heap_alloc(((uint32_t)strlen(text) + 1) * 2, true);
    if (!tmp) {
        set_last_error(8);
        set_eax(c, 0);
        return;
    }
    gm_put_wstr(tmp, text, (uint32_t)strlen(text) + 1);
    guest_call(c, callback, tmp);
    heap_free(tmp);
    set_eax(c, 1);
}
void k_EnumSystemLocalesW(X86 *c) {
    enumerate_text(c, "00000409");
}
void k_EnumCalendarInfoW(X86 *c) {
    enumerate_text(c, "1");
}
void k_GetCPInfoExW(X86 *c) {
    uint32_t p = arg(c, 2);
    if (!p || !gm_valid(p, 544)) {
        set_last_error(87);
        set_eax(c, 0);
        return;
    }
    // CPINFOEXW: DefaultChar[2], LeadByte[12], WCHAR default, UINT code page,
    // then CodePageName[260]. The 32-bit guest record is exactly 544 bytes.
    memset(g_mem + p, 0, 544);
    wr32(p, 1);
    wr8(p + 4, '?');
    wr16(p + 18, '?');
    wr32(p + 20, 1252);
    gm_put_wstr(p + 24, "1252 (ANSI - Latin I)", 260);
    set_eax(c, 1);
}

static const ImportShim g_kernel32_wide[] = {
    {"KERNEL32.dll", "GetThreadLocale", 0, k_GetThreadLocale},
    {"KERNEL32.dll", "SetThreadLocale", 1, k_SetThreadLocale},
    {"KERNEL32.dll", "EnumSystemLocalesW", 2, k_EnumSystemLocalesW},
    {"KERNEL32.dll", "EnumCalendarInfoW", 4, k_EnumCalendarInfoW},
    {"KERNEL32.dll", "GetCPInfoExW", 3, k_GetCPInfoExW},
    {"KERNEL32.dll", "GetUserDefaultUILanguage", 0, k_GetUserDefaultUILanguage},
    {"KERNEL32.dll", "GetSystemDefaultUILanguage", 0, k_GetSystemDefaultUILanguage},
    {"KERNEL32.dll", "IsDBCSLeadByteEx", 2, k_IsDBCSLeadByteEx},
    {"KERNEL32.dll", "GetConsoleCP", 0, k_GetConsoleCP},
    {"KERNEL32.dll", "GetConsoleOutputCP", 0, k_GetConsoleOutputCP},

    {"KERNEL32.dll", "lstrlenW", 1, k_lstrlenW},
    {"KERNEL32.dll", "lstrcatW", 2, k_lstrcatW},
    {"KERNEL32.dll", "FormatMessageW", 7, k_FormatMessageW},
    {"KERNEL32.dll", "OutputDebugStringW", 1, k_OutputDebugStringW},
    {"KERNEL32.dll", "GetDateFormatW", 6, k_GetDateFormatW},
    {"KERNEL32.dll", "FileTimeToLocalFileTime", 2, k_FileTimeToLocalFileTime},
    {"KERNEL32.dll", "FileTimeToSystemTime", 2, k_FileTimeToSystemTime},
    {"KERNEL32.dll", "FileTimeToDosDateTime", 3, k_FileTimeToDosDateTime},

    {"KERNEL32.dll", "CreateEventW", 4, k_CreateEventW},
    {"KERNEL32.dll", "CreateMutexW", 3, k_CreateMutexW},
    {"KERNEL32.dll", "OpenMutexW", 3, k_OpenMutexW},
    {"KERNEL32.dll", "CreateFileMappingW", 6, k_CreateFileMappingW},

    {"KERNEL32.dll", "GetPrivateProfileStringW", 6, k_GetPrivateProfileStringW},
    {"KERNEL32.dll", "WritePrivateProfileStringW", 4, k_WritePrivateProfileStringW},

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

void kernel32_wide_reset() {
    g_thread_lcid = 0x0409;
}
