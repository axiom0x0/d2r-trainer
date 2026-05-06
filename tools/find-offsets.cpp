// Standalone tool to trace from a known gold address back to PlayerUnit,
// then search for a static pointer to PlayerUnit in the D2R.exe PE image.
// Usage: sudo ./find-offsets <pid> <gold_address_hex>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>
#include <sys/uio.h>

static bool read_mem(pid_t pid, uintptr_t addr, void* buf, size_t size) {
    struct iovec local  = { buf, size };
    struct iovec remote = { reinterpret_cast<void*>(addr), size };
    return process_vm_readv(pid, &local, 1, &remote, 1, 0) == static_cast<ssize_t>(size);
}

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

static uintptr_t find_pe_base(pid_t pid, const std::vector<Region>& regions) {
    for (auto& r : regions) {
        if (r.name.find("memfd:wine-mapping") == std::string::npos) continue;
        if (r.perms.find('r') == std::string::npos) continue;
        if (r.end - r.start < 10 * 1024 * 1024) continue;

        uint8_t mz[2] = {};
        if (!read_mem(pid, r.start, mz, 2)) continue;
        if (mz[0] != 'M' || mz[1] != 'Z') continue;

        uint32_t pe_off = 0;
        if (!read_mem(pid, r.start + 0x3C, &pe_off, 4)) continue;
        if (pe_off == 0 || pe_off > 0x1000) continue;

        uint32_t pe_sig = 0;
        if (!read_mem(pid, r.start + pe_off, &pe_sig, 4)) continue;
        if (pe_sig == 0x00004550) return r.start;
    }
    return 0;
}

struct StatEntry {
    uint16_t unk;
    uint16_t code;
    uint32_t value;
};

int main(int argc, char* argv[]) {
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <pid> <gold_address_hex>\n", argv[0]);
        fprintf(stderr, "Example: %s 12345 5a9c50ac\n", argv[0]);
        return 1;
    }

    pid_t pid = std::stoi(argv[1]);
    uintptr_t gold_addr = std::stoull(argv[2], nullptr, 16);

    printf("PID: %d\n", pid);
    printf("Gold address: 0x%lx\n", gold_addr);

    int32_t gold_val = 0;
    if (!read_mem(pid, gold_addr, &gold_val, sizeof(gold_val))) {
        fprintf(stderr, "Cannot read gold address\n");
        return 1;
    }
    printf("Gold value: %d\n\n", gold_val);

    auto regions = get_regions(pid);

    uintptr_t pe_base = find_pe_base(pid, regions);
    if (pe_base == 0) {
        fprintf(stderr, "Could not find D2R.exe PE base\n");
        return 1;
    }
    printf("D2R.exe PE base: 0x%lx\n\n", pe_base);

    // Step 1: Gold is inside a stat array. Each stat is 8 bytes: {u16 unk, u16 code, u32 value}
    // Gold code = 14. Find the stat entry containing our gold value.
    printf("=== Step 1: Verify gold stat entry ===\n");

    // The address from scanmem points to the value (int32).
    // In the stat struct: [2 bytes unk][2 bytes code][4 bytes value]
    // So the entry starts at gold_addr - 4.
    uintptr_t gold_entry = gold_addr - 4;
    StatEntry entry;
    if (!read_mem(pid, gold_entry, &entry, sizeof(entry))) {
        fprintf(stderr, "Cannot read stat entry at gold_addr - 4\n");
        return 1;
    }

    printf("Stat entry at 0x%lx: code=%u value=%u\n", gold_entry, entry.code, entry.value);

    if (entry.code != 14) {
        printf("Code is not 14 (gold). Scanning nearby for code 14 with value %d...\n", gold_val);
        bool found = false;
        for (int offset = -512; offset <= 512; offset += 8) {
            StatEntry test;
            uintptr_t test_addr = gold_addr + offset;
            if (read_mem(pid, test_addr, &test, sizeof(test))) {
                if (test.code == 14 && test.value == (uint32_t)gold_val) {
                    gold_entry = test_addr;
                    entry = test;
                    printf("Found gold stat entry at 0x%lx\n", test_addr);
                    found = true;
                    break;
                }
            }
        }
        if (!found) {
            // Maybe scanmem found the raw value, not offset by 4
            // Try treating gold_addr as the start of the entry
            if (read_mem(pid, gold_addr, &entry, sizeof(entry)) && entry.code == 14) {
                gold_entry = gold_addr;
                printf("Gold entry starts at scanmem address: 0x%lx\n", gold_addr);
            } else {
                fprintf(stderr, "Could not locate stat code 14 near gold address\n");
                return 1;
            }
        }
    }
    printf("Confirmed: code=%u value=%u ✓\n", entry.code, entry.value);

    // Step 2: Find stat array start by scanning backwards
    printf("\n=== Step 2: Find stat array base ===\n");

    uintptr_t stat_array_start = gold_entry;
    for (int i = 0; i < 300; i++) {
        uintptr_t prev = stat_array_start - 8;
        StatEntry test;
        if (!read_mem(pid, prev, &test, sizeof(test))) break;
        if (test.code > 1000) break;
        stat_array_start = prev;
    }
    printf("Stat array starts at: 0x%lx\n", stat_array_start);
    printf("Gold is at index %lu in the array\n", (gold_entry - stat_array_start) / 8);

    // Dump all stats in the array
    printf("\n=== Stat Array Dump ===\n");
    printf("%-6s %-8s %-14s %s\n", "Index", "Code", "Value", "Known Name");

    const char* stat_names[] = {
        [0]="Strength", [1]="Energy", [2]="Dexterity", [3]="Vitality",
        [4]="StatPts", [5]="SkillPts",
        [6]="HP", [7]="MaxHP", [8]="Mana", [9]="MaxMana",
        [10]="Stamina", [11]="MaxStamina", [12]="Level", [13]="Experience",
        [14]="Gold", [15]="GoldStash",
    };
    const int num_names = sizeof(stat_names) / sizeof(stat_names[0]);

    for (uint16_t i = 0; i < 100; i++) {
        StatEntry s;
        if (!read_mem(pid, stat_array_start + i * 8, &s, sizeof(s))) break;
        if (s.code > 5000) break;

        const char* name = (s.code < num_names && stat_names[s.code]) ? stat_names[s.code] : "";

        // HP/Mana/Stamina are stored << 8
        bool shifted = (s.code >= 6 && s.code <= 11);
        if (shifted) {
            printf("%-6u %-8u %-14u %s (display: %u)\n", i, s.code, s.value, name, s.value >> 8);
        } else {
            printf("%-6u %-8u %-14u %s\n", i, s.code, s.value, name);
        }
    }

    // Step 3: Find StatList (pStats at +0x30 points to stat array)
    printf("\n=== Step 3: Find StatList ===\n");

    uintptr_t stat_list_addr = 0;
    for (auto& r : regions) {
        if (r.perms.find('r') == std::string::npos) continue;
        if (r.perms.find('w') == std::string::npos) continue;
        size_t size = r.end - r.start;
        if (size > 200 * 1024 * 1024 || size < 0x40) continue;
        // Skip file-backed regions (DLLs, data files) but allow anonymous + memfd
        if (!r.name.empty() &&
            r.name.find("memfd:") == std::string::npos &&
            r.name.find("(deleted)") == std::string::npos &&
            r.name[0] == '/') continue;

        const size_t chunk_size = 4096 * 16;
        for (uintptr_t addr = r.start; addr < r.end - 8; addr += chunk_size) {
            size_t to_read = std::min(chunk_size + 8, (size_t)(r.end - addr));
            std::vector<uint8_t> buf(to_read);
            if (!read_mem(pid, addr, buf.data(), to_read)) break;

            for (size_t off = 0; off + 8 <= to_read; off += 8) {
                uintptr_t val;
                memcpy(&val, buf.data() + off, 8);
                if (val == stat_array_start) {
                    uintptr_t candidate = addr + off - 0x30;
                    uintptr_t check = 0;
                    if (read_mem(pid, candidate + 0x30, &check, 8) && check == stat_array_start) {
                        uint16_t count = 0;
                        read_mem(pid, candidate + 0x38, &count, 2);
                        printf("StatList at 0x%lx (count=%u) ✓\n", candidate, count);
                        if (stat_list_addr == 0) stat_list_addr = candidate;
                    }
                }
            }
        }
        if (stat_list_addr != 0) break;
    }

    if (stat_list_addr == 0) {
        fprintf(stderr, "Could not find StatList\n");
        return 1;
    }

    // Step 4: Find PlayerUnit (pStats at +0x88 points to StatList)
    printf("\n=== Step 4: Find PlayerUnit ===\n");

    // Only scan rw- regions, allow anonymous + memfd, skip file-backed
    printf("Searching heap/memfd regions for pointers to StatList (0x%lx)...\n", stat_list_addr);

    struct PointerHit {
        uintptr_t addr;
        uintptr_t container;
        uint32_t unit_type;
    };
    std::vector<PointerHit> hits;

    for (auto& r : regions) {
        if (r.perms.find('r') == std::string::npos) continue;
        if (r.perms.find('w') == std::string::npos) continue;
        size_t size = r.end - r.start;
        if (size < 0x90) continue;
        if (size > 200 * 1024 * 1024) continue;
        // Skip file-backed regions but allow anonymous + memfd
        if (!r.name.empty() &&
            r.name.find("memfd:") == std::string::npos &&
            r.name.find("(deleted)") == std::string::npos &&
            r.name[0] == '/') continue;

        const size_t chunk_size = 4096 * 16;
        for (uintptr_t addr = r.start; addr < r.end - 8; addr += chunk_size) {
            size_t to_read = std::min(chunk_size + 8, (size_t)(r.end - addr));
            std::vector<uint8_t> buf(to_read);
            if (!read_mem(pid, addr, buf.data(), to_read)) break;

            for (size_t off = 0; off + 8 <= to_read; off += 8) {
                uintptr_t val;
                memcpy(&val, buf.data() + off, 8);
                if (val == stat_list_addr) {
                    uintptr_t found = addr + off;
                    uintptr_t candidate = found - 0x88;
                    uint32_t unit_type = 0xFFFFFFFF;
                    read_mem(pid, candidate, &unit_type, 4);
                    hits.push_back({found, candidate, unit_type});
                }
            }
        }
    }

    printf("Found %zu pointers to StatList:\n", hits.size());

    uintptr_t player_unit_addr = 0;
    for (auto& h : hits) {
        std::string region_info = "unknown";
        for (auto& r2 : regions) {
            if (h.addr >= r2.start && h.addr < r2.end) {
                region_info = r2.name.empty() ? "(anonymous)" : r2.name;
                break;
            }
        }

        printf("  ptr at 0x%lx [%s]\n", h.addr, region_info.c_str());
        printf("    candidate PlayerUnit at 0x%lx (type=%u)\n", h.container, h.unit_type);

        // Try to read player name to verify
        if (h.unit_type == 0) {
            uintptr_t pdata = 0;
            if (read_mem(pid, h.container + 0x10, &pdata, 8) && pdata != 0) {
                char name[64] = {};
                if (read_mem(pid, pdata, name, 63) && name[0] >= 'A' && name[0] <= 'z') {
                    printf("    Player name: %s ← LIKELY MATCH\n", name);
                    if (player_unit_addr == 0) player_unit_addr = h.container;
                }
            }
        }

        // Also check if pStats offset might not be 0x88 - show what offset this would be
        printf("    (pointer is at offset +0x%lx from candidate)\n", h.addr - h.container);
    }

    // If type==0 didn't work, try the first hit anyway
    if (player_unit_addr == 0 && !hits.empty()) {
        printf("\nNo type=0 match. Trying first hit as PlayerUnit...\n");
        player_unit_addr = hits[0].container;
    }

    if (player_unit_addr == 0) {
        fprintf(stderr, "Could not find PlayerUnit\n");
        return 1;
    }

    // Step 5: Search for a static pointer to PlayerUnit
    printf("\n=== Step 5: Search for static pointer to PlayerUnit ===\n");

    uintptr_t search_end = pe_base + 100 * 1024 * 1024;
    int found_count = 0;

    for (auto& r : regions) {
        if (r.perms.find('r') == std::string::npos) continue;
        if (r.end < pe_base || r.start > search_end) continue;

        const size_t chunk_size = 4096 * 16;
        for (uintptr_t addr = r.start; addr < r.end - 8; addr += chunk_size) {
            size_t to_read = std::min(chunk_size + 8, (size_t)(r.end - addr));
            std::vector<uint8_t> buf(to_read);
            if (!read_mem(pid, addr, buf.data(), to_read)) break;

            for (size_t off = 0; off + 8 <= to_read; off += 8) {
                uintptr_t val;
                memcpy(&val, buf.data() + off, 8);
                if (val == player_unit_addr) {
                    uintptr_t found_addr = addr + off;
                    printf("  STATIC: 0x%lx (D2R.exe + 0x%lx)\n",
                           found_addr, found_addr - pe_base);
                    found_count++;
                }
            }
        }
    }

    if (found_count == 0) {
        printf("No static pointer in PE region. Searching all memory...\n");

        // Also look for pointers to PlayerUnit in hash tables - D2R uses
        // a unit hash table where PlayerUnit might be in a linked list.
        // Search more broadly.
        for (auto& r : regions) {
            if (r.perms.find('r') == std::string::npos) continue;
            size_t size = r.end - r.start;
            if (size > 100 * 1024 * 1024) continue;

            const size_t chunk_size = 4096 * 16;
            for (uintptr_t addr = r.start; addr < r.end - 8; addr += chunk_size) {
                size_t to_read = std::min(chunk_size + 8, (size_t)(r.end - addr));
                std::vector<uint8_t> buf(to_read);
                if (!read_mem(pid, addr, buf.data(), to_read)) break;

                for (size_t off = 0; off + 8 <= to_read; off += 8) {
                    uintptr_t val;
                    memcpy(&val, buf.data() + off, 8);
                    if (val == player_unit_addr) {
                        uintptr_t found_addr = addr + off;
                        std::string region_info = "unknown";
                        bool in_pe = false;
                        for (auto& r2 : regions) {
                            if (found_addr >= r2.start && found_addr < r2.end) {
                                region_info = r2.name.empty() ? "(anonymous)" : r2.name;
                                if (r2.name.find("memfd:wine-mapping") != std::string::npos &&
                                    r2.start >= pe_base && r2.start < search_end)
                                    in_pe = true;
                                break;
                            }
                        }
                        printf("  0x%lx (offset from PE: 0x%lx) [%s]%s\n",
                               found_addr, found_addr - pe_base,
                               region_info.c_str(),
                               in_pe ? " ← PE DATA" : "");
                        found_count++;
                    }
                }
            }
        }
    }

    printf("\n=== Summary ===\n");
    printf("PE base:      0x%lx\n", pe_base);
    printf("PlayerUnit:   0x%lx\n", player_unit_addr);
    printf("StatList:     0x%lx (PlayerUnit + 0x88)\n", stat_list_addr);
    printf("Stat array:   0x%lx (StatList + 0x30)\n", stat_array_start);
    printf("Gold entry:   0x%lx (stat code 14)\n", gold_entry);
    printf("\nChain: [static_ptr] → PlayerUnit → +0x88 → StatList → +0x30 → stat array → find code 14\n");

    return 0;
}
