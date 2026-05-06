#include "memory.h"

#include <cstring>
#include <fstream>
#include <string>
#include <sys/uio.h>
#include <cstdio>

bool read_memory(pid_t pid, uintptr_t addr, void* buf, size_t size) {
    struct iovec local  = { buf, size };
    struct iovec remote = { reinterpret_cast<void*>(addr), size };
    return process_vm_readv(pid, &local, 1, &remote, 1, 0) == static_cast<ssize_t>(size);
}

bool write_memory(pid_t pid, uintptr_t addr, const void* buf, size_t size) {
    struct iovec local  = { const_cast<void*>(buf), size };
    struct iovec remote = { reinterpret_cast<void*>(addr), size };
    return process_vm_writev(pid, &local, 1, &remote, 1, 0) == static_cast<ssize_t>(size);
}

uintptr_t walk_pointer_chain(pid_t pid, uintptr_t base,
                              const std::vector<uintptr_t>& offsets) {
    uintptr_t addr = base;

    for (size_t i = 0; i < offsets.size() - 1; i++) {
        addr += offsets[i];

        uintptr_t next = 0;
        if (!read_memory(pid, addr, &next, sizeof(next))) {
            return 0;
        }
        if (next == 0) {
            return 0;
        }
        addr = next;
    }

    addr += offsets.back();
    return addr;
}

// D2R stat entry: [u16 unk][u16 code][u32 value] = 8 bytes
struct RawStatEntry {
    uint16_t unk;
    uint16_t code;
    uint32_t value;
};

struct Region {
    uintptr_t start;
    uintptr_t end;
    std::string perms;
    std::string name;
};

static std::vector<Region> get_regions(pid_t pid) {
    std::vector<Region> regions;
    std::string path = "/proc/" + std::to_string(pid) + "/maps";
    std::ifstream maps(path);
    std::string line;

    while (std::getline(maps, line)) {
        Region r;
        char perms[8] = {};
        char name[512] = {};
        unsigned long start, end;

        int n = sscanf(line.c_str(), "%lx-%lx %7s %*s %*s %*s %511[^\n]",
                       &start, &end, perms, name);
        r.start = start;
        r.end = end;
        r.perms = perms;
        r.name = (n >= 4) ? name : "";
        size_t pos = r.name.find_first_not_of(" \t");
        if (pos != std::string::npos) r.name = r.name.substr(pos);
        regions.push_back(r);
    }
    return regions;
}

std::map<uint16_t, D2RStat> find_stat_array(pid_t pid) {
    auto all = find_all_stat_arrays(pid);
    if (all.empty()) return {};
    return all[0];
}

std::vector<std::map<uint16_t, D2RStat>> find_all_stat_arrays(pid_t pid) {
    std::vector<std::map<uint16_t, D2RStat>> results;
    auto regions = get_regions(pid);

    for (auto& r : regions) {
        if (r.perms.find('r') == std::string::npos) continue;
        if (r.perms.find('w') == std::string::npos) continue;
        size_t size = r.end - r.start;
        if (size < 128 || size > 200 * 1024 * 1024) continue;
        if (!r.name.empty() &&
            r.name.find("memfd:") == std::string::npos &&
            r.name.find("(deleted)") == std::string::npos &&
            r.name[0] == '/') continue;

        const size_t chunk_size = 4096 * 32;
        for (uintptr_t addr = r.start; addr < r.end - 64; addr += chunk_size) {
            size_t to_read = std::min(chunk_size + 64, (size_t)(r.end - addr));
            std::vector<uint8_t> buf(to_read);
            if (!read_memory(pid, addr, buf.data(), to_read)) break;

            for (size_t off = 0; off + 32 <= to_read; off += 8) {
                RawStatEntry e0, e1, e2, e3;
                memcpy(&e0, buf.data() + off,      8);
                memcpy(&e1, buf.data() + off + 8,   8);
                memcpy(&e2, buf.data() + off + 16,  8);
                memcpy(&e3, buf.data() + off + 24,  8);

                if (e0.code == 0 && e1.code == 1 && e2.code == 2 && e3.code == 3 &&
                    e0.value > 0 && e0.value < 10000 &&
                    e1.value > 0 && e1.value < 10000 &&
                    e2.value > 0 && e2.value < 10000 &&
                    e3.value > 0 && e3.value < 10000) {

                    std::map<uint16_t, D2RStat> stats;
                    uintptr_t array_start = addr + off;

                    // Skip if too close to a previous match (same array)
                    bool duplicate = false;
                    for (auto& prev : results) {
                        if (prev.count(0)) {
                            uintptr_t prev_addr = prev[0].value_addr - 4;
                            if (array_start >= prev_addr - 256 && array_start <= prev_addr + 256) {
                                duplicate = true;
                                break;
                            }
                        }
                    }
                    if (duplicate) continue;

                    for (int i = 0; i < 100; i++) {
                        RawStatEntry s;
                        if (!read_memory(pid, array_start + i * 8, &s, sizeof(s))) break;
                        if (s.code > 5000) break;

                        D2RStat stat;
                        stat.code = s.code;
                        stat.value = s.value;
                        stat.value_addr = array_start + i * 8 + 4;
                        stats[s.code] = stat;
                    }

                    for (int i = 1; i <= 20; i++) {
                        uintptr_t check = array_start - i * 8;
                        if (check < r.start) break;
                        RawStatEntry s;
                        if (!read_memory(pid, check, &s, sizeof(s))) break;
                        if (s.code > 500) break;
                        if (s.value > 0 && s.value < 100000000 &&
                            stats.find(s.code) == stats.end()) {
                            D2RStat stat;
                            stat.code = s.code;
                            stat.value = s.value;
                            stat.value_addr = check + 4;
                            stats[s.code] = stat;
                        }
                    }

                    results.push_back(stats);
                }
            }
        }
    }

    return results;
}

// Stat name lookup for dump
static const char* stat_name(uint16_t code) {
    switch (code) {
        case 0: return "Str";    case 1: return "Ene";
        case 2: return "Dex";    case 3: return "Vit";
        case 4: return "StatPt"; case 5: return "SkillPt";
        case 6: return "HP";     case 7: return "MaxHP";
        case 8: return "Mana";   case 9: return "MaxMana";
        case 10: return "Stam";  case 11: return "MaxStam";
        case 12: return "Lvl";   case 13: return "Exp";
        case 14: return "Gold";  case 15: return "GoldSt";
        default: return "?";
    }
}

void dump_stat_region(pid_t pid, int before, int after) {
    // First, find the stat array location
    auto regions = get_regions(pid);

    for (auto& r : regions) {
        if (r.perms.find('r') == std::string::npos) continue;
        if (r.perms.find('w') == std::string::npos) continue;
        size_t size = r.end - r.start;
        if (size < 128 || size > 200 * 1024 * 1024) continue;
        if (!r.name.empty() &&
            r.name.find("memfd:") == std::string::npos &&
            r.name.find("(deleted)") == std::string::npos &&
            r.name[0] == '/') continue;

        const size_t chunk_size = 4096 * 32;
        for (uintptr_t addr = r.start; addr < r.end - 64; addr += chunk_size) {
            size_t to_read = std::min(chunk_size + 64, (size_t)(r.end - addr));
            std::vector<uint8_t> buf(to_read);
            if (!read_memory(pid, addr, buf.data(), to_read)) break;

            for (size_t off = 0; off + 32 <= to_read; off += 8) {
                RawStatEntry e0, e1, e2, e3;
                memcpy(&e0, buf.data() + off,      8);
                memcpy(&e1, buf.data() + off + 8,   8);
                memcpy(&e2, buf.data() + off + 16,  8);
                memcpy(&e3, buf.data() + off + 24,  8);

                if (e0.code == 0 && e1.code == 1 && e2.code == 2 && e3.code == 3 &&
                    e0.value > 0 && e0.value < 10000 &&
                    e1.value > 0 && e1.value < 10000 &&
                    e2.value > 0 && e2.value < 10000 &&
                    e3.value > 0 && e3.value < 10000) {

                    uintptr_t array_start = addr + off;
                    printf("\nStat array found at 0x%lx\n", array_start);
                    printf("Dumping entries [-%d to +%d]:\n\n", before, after);
                    printf("  %-4s  %-16s  %-6s  %-8s  %s\n",
                           "Idx", "Address", "Unk", "Code", "Value");
                    printf("  %-4s  %-16s  %-6s  %-8s  %s\n",
                           "---", "----------------", "-----", "-------", "----------");

                    for (int i = -before; i < after; i++) {
                        uintptr_t entry_addr = array_start + i * 8;
                        if (entry_addr < r.start) continue;
                        RawStatEntry s;
                        if (!read_memory(pid, entry_addr, &s, sizeof(s))) continue;

                        const char* name = (s.code <= 15) ? stat_name(s.code) : "";
                        const char* marker = (i == 0) ? " <<< pattern start" : "";
                        printf("  %+3d   0x%014lx  %5u  %5u %-6s  %10u%s\n",
                               i, entry_addr, s.unk, s.code, name, s.value, marker);
                    }
                    return;
                }
            }
        }
    }
    printf("Stat array not found!\n");
}
