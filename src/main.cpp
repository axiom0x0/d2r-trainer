#include "memory.h"
#include "process.h"
#include "savefile.h"

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <chrono>
#include <algorithm>
#include <vector>
#include <map>

#include <readline/readline.h>
#include <readline/history.h>

// Known D2R stat codes
enum StatCode : uint16_t {
    STAT_STRENGTH   = 0,
    STAT_ENERGY     = 1,
    STAT_DEXTERITY  = 2,
    STAT_VITALITY   = 3,
    STAT_STATPTS    = 4,
    STAT_SKILLPTS   = 5,
    STAT_HP         = 6,
    STAT_MAXHP      = 7,
    STAT_MANA       = 8,
    STAT_MAXMANA    = 9,
    STAT_STAMINA    = 10,
    STAT_MAXSTAMINA = 11,
    STAT_LEVEL      = 12,
    STAT_EXPERIENCE = 13,
    STAT_GOLD       = 14,
    STAT_GOLDSTASH  = 15,
};

struct TrainerStat {
    std::string name;
    uint16_t code;
    bool shifted;     // HP/Mana/Stamina are stored << 8
    bool god;         // included in god mode
    int32_t god_value;
    uintptr_t addr;   // resolved memory address of value field
    int32_t raw_value;
};

static std::vector<TrainerStat> build_stat_table() {
    return {
        {"Strength",   STAT_STRENGTH,   false, false, 0,         0, 0},
        {"Energy",     STAT_ENERGY,     false, false, 0,         0, 0},
        {"Dexterity",  STAT_DEXTERITY,  false, false, 0,         0, 0},
        {"Vitality",   STAT_VITALITY,   false, false, 0,         0, 0},
        {"Stat Points",STAT_STATPTS,    false, false, 0,         0, 0},
        {"Skill Points",STAT_SKILLPTS,  false, false, 0,         0, 0},
        {"HP",         STAT_HP,         true,  true,  2097151,  0, 0},
        {"Max HP",     STAT_MAXHP,      true,  true,  2097151,  0, 0},
        {"Mana",       STAT_MANA,       true,  true,  2097151,  0, 0},
        {"Max Mana",   STAT_MAXMANA,    true,  true,  2097151,  0, 0},
        {"Stamina",    STAT_STAMINA,    true,  false, 0,         0, 0},
        {"Max Stamina",STAT_MAXSTAMINA, true,  false, 0,         0, 0},
        {"Level",      STAT_LEVEL,      false, false, 0,         0, 0},
        {"Experience", STAT_EXPERIENCE, false, false, 0,         0, 0},
        {"Gold",       STAT_GOLD,       false, false, 0,         0, 0},
        {"Gold Stash", STAT_GOLDSTASH,  false, false, 0,         0, 0},
    };
}

static bool resolve_stats(pid_t pid, std::vector<TrainerStat>& stats) {
    auto found = find_stat_array(pid);
    if (found.empty()) return false;

    for (auto& s : stats) {
        auto it = found.find(s.code);
        if (it != found.end()) {
            s.addr = it->second.value_addr;
            s.raw_value = it->second.value;
        } else {
            s.addr = 0;
            s.raw_value = 0;
        }
    }
    return true;
}

static void refresh_values(pid_t pid, std::vector<TrainerStat>& stats) {
    for (auto& s : stats) {
        if (s.addr != 0) {
            read_memory(pid, s.addr, &s.raw_value, sizeof(s.raw_value));
        }
    }
}

static std::string format_value(const TrainerStat& s) {
    if (s.addr == 0) return "\033[1;31m<not found>\033[0m";
    if (s.shifted) {
        return std::to_string(s.raw_value >> 8) + " (raw: " + std::to_string(s.raw_value) + ")";
    }
    return std::to_string(s.raw_value);
}

static void print_stats(const std::vector<TrainerStat>& stats) {
    for (size_t i = 0; i < stats.size(); i++) {
        std::string val = format_value(stats[i]);
        std::string tag = stats[i].god ? " \033[1;31m[GOD]\033[0m" : "";
        std::cout << "  \033[1;33m[" << std::setw(2) << (i + 1) << "]\033[0m "
                  << std::left << std::setw(14) << stats[i].name
                  << val << tag << "\n";
    }
}

static void print_help() {
    std::cout << "\nCommands:\n"
              << "  \033[1ms<N> <value>\033[0m   Set stat #N to value (e.g. s7 99999999)\n"
              << "  \033[1mf<N> <value>\033[0m   Freeze stat #N at value\n"
              << "  \033[1mu<N>\033[0m           Unfreeze stat #N\n"
              << "  \033[1mg\033[0m              Toggle god mode (HP/Mana guardian, data-only)\n"
              << "  \033[1mw\033[0m              Write all stats to save file\n"
              << "  \033[1mws<N> <value>\033[0m  Write stat #N to save file (same index as s)\n"
              << "  \033[1mc\033[0m              Connect/reconnect to D2R process\n"
              << "  \033[1md\033[0m              Dump raw memory around stat array\n"
              << "  \033[1mr\033[0m              Refresh values\n"
              << "  \033[1mR\033[0m              Full re-scan\n"
              << "  \033[1mq\033[0m              Quit\n\n";
}

struct FreezeEntry {
    size_t index;
    int32_t value;
};

int main() {
    std::string process_name = "D2R.exe";

    pid_t pid = -1;
    auto stats = build_stat_table();

    auto try_connect = [&]() -> bool {
        pid = find_process(process_name);
        if (pid <= 0) return false;
        std::cout << "Found PID: " << pid << "\n";
        std::cout << "Scanning for player stats...\n";
        if (!resolve_stats(pid, stats)) {
            std::cout << "\033[1;33m⚠\033[0m Stat array not found (not in a game yet?)\n";
            return true; // process found, just no stats yet
        }
        return true;
    };

    std::cout << "Looking for " << process_name << "...\n";
    if (!try_connect()) {
        std::cout << "\033[1;33m⚠\033[0m D2R not running. Save file commands (ws) available.\n";
        std::cout << "  Use \033[1mc\033[0m to connect when D2R is running.\n";
    }

    std::cout << "\033[1;36mD2R Trainer v0.3\n"
              << "────────────────\033[0m\n"
              << "PID: " << pid << "\n";

    // Find save file directory
    std::string save_dir;
    // Get real user home (works even under sudo)
    std::string home;
    if (getenv("SUDO_USER")) {
        // Running under sudo - get the real user's home
        std::string pw_line;
        std::ifstream pw("/etc/passwd");
        std::string sudo_user = getenv("SUDO_USER");
        while (std::getline(pw, pw_line)) {
            if (pw_line.substr(0, sudo_user.size() + 1) == sudo_user + ":") {
                // 6th field is home dir
                int field = 0;
                for (size_t i = 0; i < pw_line.size(); i++) {
                    if (pw_line[i] == ':') field++;
                    if (field == 5) { 
                        size_t end = pw_line.find(':', i + 1);
                        home = pw_line.substr(i + 1, end - i - 1);
                        break;
                    }
                }
                break;
            }
        }
    }
    if (home.empty()) home = getenv("HOME") ? getenv("HOME") : "";

    std::vector<std::string> search_dirs = {
        home + "/Games/battlenet/drive_c/users/steamuser/Saved Games/Diablo II Resurrected",
        home + "/Games/battlenet/drive_c/users/" + (getenv("SUDO_USER") ? getenv("SUDO_USER") : "") + "/Saved Games/Diablo II Resurrected",
    };
    for (auto& dir : search_dirs) {
        if (std::filesystem::exists(dir)) {
            save_dir = dir;
            break;
        }
    }
    if (!save_dir.empty()) {
        std::cout << "Save dir: " << save_dir << "\n";
    }
    std::cout << "\n";

    print_stats(stats);
    print_help();

    std::vector<FreezeEntry> frozen;
    std::mutex freeze_mutex;
    std::atomic<bool> god_mode{false};
    std::atomic<bool> running{true};

    struct GodAddr {
        uintptr_t hp_addr;
        uintptr_t maxhp_addr;
        uintptr_t mana_addr;
        uintptr_t maxmana_addr;
    };
    std::vector<GodAddr> god_addrs;
    std::atomic<int> god_fixes{0};
    std::atomic<int> god_checks{0};
    std::atomic<int> god_rescan_count{0};
    std::mutex god_mutex;

    std::atomic<int> freeze_writes{0};
    std::atomic<int> freeze_fails{0};
    std::atomic<int> freeze_verified{0};
    std::atomic<int> freeze_arrays{0};
    std::atomic<uint32_t> player_str{0};
    std::atomic<uint32_t> player_ene{0};
    std::atomic<uint32_t> player_dex{0};
    std::atomic<uint32_t> player_vit{0};

    if (stats[0].addr != 0) {
        player_str = stats[0].raw_value;
        player_ene = stats[1].raw_value;
        player_dex = stats[2].raw_value;
        player_vit = stats[3].raw_value;
    }

    struct FreezeAddr {
        uintptr_t addr;
        int32_t value;
    };
    std::vector<FreezeAddr> cached_freeze_addrs;
    std::atomic<bool> need_rescan{true};

    std::thread freeze_thread([&]() {
        while (running) {
            {
                std::lock_guard<std::mutex> lock(freeze_mutex);
                if (!frozen.empty() && pid > 0) {

                    if (need_rescan) {
                        auto all_arrays = find_all_stat_arrays(pid);
                        cached_freeze_addrs.clear();
                        int matched = 0;

                        uint32_t ps = player_str.load();
                        uint32_t pe = player_ene.load();
                        uint32_t pd = player_dex.load();
                        uint32_t pv = player_vit.load();

                        for (auto& arr : all_arrays) {
                            auto s0 = arr.find(0);
                            auto s1 = arr.find(1);
                            auto s2 = arr.find(2);
                            auto s3 = arr.find(3);
                            if (s0 == arr.end() || s1 == arr.end() ||
                                s2 == arr.end() || s3 == arr.end()) continue;

                            bool is_player =
                                (s0->second.value >= ps && s0->second.value < ps + 500) &&
                                (s1->second.value >= pe && s1->second.value < pe + 500) &&
                                (s2->second.value >= pd && s2->second.value < pd + 500) &&
                                (s3->second.value >= pv && s3->second.value < pv + 500);

                            if (!is_player) continue;
                            matched++;

                            for (auto& f : frozen) {
                                if (f.index >= stats.size()) continue;
                                uint16_t code = stats[f.index].code;
                                auto it = arr.find(code);
                                if (it != arr.end() && it->second.value_addr != 0) {
                                    // Verify: write then read back
                                    write_memory(pid, it->second.value_addr, &f.value, sizeof(f.value));
                                    int32_t readback = 0;
                                    read_memory(pid, it->second.value_addr, &readback, sizeof(readback));
                                    if (readback == f.value) {
                                        cached_freeze_addrs.push_back({it->second.value_addr, f.value});
                                    }
                                }
                            }
                        }
                        freeze_arrays = matched;
                        freeze_verified = cached_freeze_addrs.size();
                        need_rescan = false;

                        if (!all_arrays.empty()) {
                            for (auto& s : stats) {
                                auto it = all_arrays[0].find(s.code);
                                if (it != all_arrays[0].end()) {
                                    s.addr = it->second.value_addr;
                                    s.raw_value = it->second.value;
                                }
                            }
                        }
                    }

                    bool any_stale = false;
                    for (auto& fa : cached_freeze_addrs) {
                        write_memory(pid, fa.addr, &fa.value, sizeof(fa.value));
                        freeze_writes++;

                        // Periodically verify (every ~50ms worth of writes)
                        if (freeze_writes % 10 == 0) {
                            int32_t readback = 0;
                            read_memory(pid, fa.addr, &readback, sizeof(readback));
                            // If readback is wildly different and not just game-modified,
                            // the address is stale (array moved)
                            if (readback != fa.value) {
                                // Try writing again
                                write_memory(pid, fa.addr, &fa.value, sizeof(fa.value));
                                read_memory(pid, fa.addr, &readback, sizeof(readback));
                                if (readback != fa.value) {
                                    any_stale = true;
                                    freeze_fails++;
                                }
                            }
                        }
                    }
                    if (any_stale) {
                        need_rescan = true;
                    }
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    });

    std::thread god_thread([&]() {
        while (running) {
            if (!god_mode || pid <= 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                continue;
            }

            static auto last_scan = std::chrono::steady_clock::now();
            auto now = std::chrono::steady_clock::now();
            bool do_rescan = god_addrs.empty() ||
                std::chrono::duration_cast<std::chrono::seconds>(now - last_scan).count() >= 5;

            if (do_rescan) {
                auto all = find_all_stat_arrays(pid);
                uint32_t ps = player_str.load();
                uint32_t pe = player_ene.load();
                uint32_t pd = player_dex.load();
                uint32_t pv = player_vit.load();

                std::lock_guard<std::mutex> lock(god_mutex);
                god_addrs.clear();

                for (auto& arr : all) {
                    auto s0 = arr.find(0), s1 = arr.find(1);
                    auto s2 = arr.find(2), s3 = arr.find(3);
                    if (s0 == arr.end() || s1 == arr.end() ||
                        s2 == arr.end() || s3 == arr.end()) continue;

                    bool is_player =
                        (s0->second.value >= ps && s0->second.value < ps + 500) &&
                        (s1->second.value >= pe && s1->second.value < pe + 500) &&
                        (s2->second.value >= pd && s2->second.value < pd + 500) &&
                        (s3->second.value >= pv && s3->second.value < pv + 500);
                    if (!is_player) continue;

                    auto hp = arr.find(6), maxhp = arr.find(7);
                    auto mp = arr.find(8), maxmp = arr.find(9);
                    if (hp != arr.end() && maxhp != arr.end()) {
                        GodAddr ga = {0, 0, 0, 0};
                        ga.hp_addr = hp->second.value_addr;
                        ga.maxhp_addr = maxhp->second.value_addr;
                        if (mp != arr.end()) ga.mana_addr = mp->second.value_addr;
                        if (maxmp != arr.end()) ga.maxmana_addr = maxmp->second.value_addr;
                        god_addrs.push_back(ga);
                    }
                }

                last_scan = now;
                god_rescan_count++;
            }

            {
                std::lock_guard<std::mutex> lock(god_mutex);
                for (auto& ga : god_addrs) {
                    int32_t hp = 0, maxhp = 0;
                    read_memory(pid, ga.hp_addr, &hp, 4);
                    read_memory(pid, ga.maxhp_addr, &maxhp, 4);

                    god_checks++;

                    if (maxhp > 0 && hp < maxhp) {
                        write_memory(pid, ga.hp_addr, &maxhp, 4);
                        god_fixes++;
                    }

                    if (ga.mana_addr && ga.maxmana_addr) {
                        int32_t mp = 0, maxmp = 0;
                        read_memory(pid, ga.mana_addr, &mp, 4);
                        read_memory(pid, ga.maxmana_addr, &maxmp, 4);
                        if (maxmp > 0 && mp < maxmp) {
                            write_memory(pid, ga.mana_addr, &maxmp, 4);
                        }
                    }
                }
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });

    while (true) {
        char* raw = readline("\033[1;32m> \033[0m");
        if (!raw) break;
        std::string line(raw);
        if (!line.empty()) add_history(raw);
        free(raw);
        if (line.empty()) continue;

        if (line == "q" || line == "quit") break;

        if (line == "c" || line == "connect") {
            std::lock_guard<std::mutex> lock(freeze_mutex);
            {
                std::lock_guard<std::mutex> lock(god_mutex);
                god_addrs.clear();
            }
            frozen.clear();
            god_mode = false;
            if (try_connect()) {
                if (stats[0].addr != 0) {
                    player_str = stats[0].raw_value;
                    player_ene = stats[1].raw_value;
                    player_dex = stats[2].raw_value;
                    player_vit = stats[3].raw_value;
                    print_stats(stats);
                } else {
                    std::cout << "Connected to PID " << pid << " - use \033[1mR\033[0m to scan when in-game\n";
                }
            } else {
                std::cout << "\033[1;31m✗\033[0m D2R not found\n";
            }
            continue;
        }

        if (line == "g" || line == "god") {
            if (pid <= 0) { std::cout << "Not connected. Use \033[1mc\033[0m first.\n"; continue; }
            if (!god_mode) {
                if (stats[0].addr != 0) {
                    player_str = stats[0].raw_value;
                    player_ene = stats[1].raw_value;
                    player_dex = stats[2].raw_value;
                    player_vit = stats[3].raw_value;
                }
                god_fixes = 0;
                god_checks = 0;
                god_rescan_count = 0;
                god_mode = true;
                std::cout << "\033[1;31m⚔ GOD MODE ON\033[0m - HP/Mana guardian active (data-only, 1ms)\n";
            } else {
                god_mode = false;
                {
                    std::lock_guard<std::mutex> lock(god_mutex);
                    god_addrs.clear();
                }
                std::cout << "\033[1;32m⚔ GOD MODE OFF\033[0m - "
                          << god_fixes.load() << " fixes / "
                          << god_checks.load() << " checks, "
                          << god_rescan_count.load() << " rescans\n";
            }
            continue;
        }


        // ws<N> <value> - write a stat to save file by display index (e.g. ws15 999999)
        // wc<code> <value> - write a stat to save file by raw stat code (e.g. wc14 999999)
        if (line.size() > 2 && (line[0] == 'w') &&
            (line[1] == 's' || line[1] == 'c') && std::isdigit(line[2])) {
            if (save_dir.empty()) {
                std::cout << "\033[1;31m✗\033[0m Save directory not found\n";
                continue;
            }
            size_t space = line.find(' ');
            if (space == std::string::npos) {
                std::cout << "Usage: ws<N> <value> (display index) or wc<code> <value> (stat code)\n";
                continue;
            }

            int code;
            if (line[1] == 's') {
                size_t idx = std::stoul(line.substr(2, space - 2)) - 1;
                if (idx >= stats.size()) {
                    std::cout << "Invalid index\n";
                    continue;
                }
                code = stats[idx].code;
            } else {
                code = std::stoi(line.substr(2, space - 2));
            }
            uint32_t val = static_cast<uint32_t>(std::stoul(line.substr(space + 1)));

            int bl = D2SSaveEditor::stat_bit_length_pub(code);
            if (bl == 0) {
                std::cout << "\033[1;31m✗\033[0m Unknown stat code " << code << "\n";
                continue;
            }
            uint32_t max_val = (1ULL << bl) - 1;
            if (val > max_val) {
                std::cout << "\033[1;33m⚠\033[0m Clamped to max " << max_val << "\n";
                val = max_val;
            }

            // Find and edit save
            std::string target_save;
            for (auto& entry : std::filesystem::directory_iterator(save_dir)) {
                if (entry.path().extension() == ".d2s") {
                    D2SSaveEditor tmp;
                    if (tmp.load(entry.path().string())) {
                        target_save = entry.path().string();
                    }
                }
            }
            if (target_save.empty()) {
                std::cout << "\033[1;31m✗\033[0m No save file found\n";
                continue;
            }

            D2SSaveEditor editor;
            if (!editor.load(target_save)) {
                std::cout << "\033[1;31m✗\033[0m Failed to load save\n";
                continue;
            }

            std::string backup = target_save + ".bak";
            if (!std::filesystem::exists(backup))
                std::filesystem::copy_file(target_save, backup);

            editor.set_stat(code, val);
            if (editor.save(target_save)) {
                std::cout << "\033[1;32m✓\033[0m " << editor.character_name()
                          << ": stat " << code << " = " << val
                          << " (save & reload to apply)\n";
            } else {
                std::cout << "\033[1;31m✗\033[0m Failed to write save\n";
            }
            continue;
        }

        // w - bulk write all memory stats to save file
        if (line == "w" || line == "write") {
            if (save_dir.empty()) {
                std::cout << "\033[1;31m✗\033[0m Save directory not found\n";
                continue;
            }

            {
                std::lock_guard<std::mutex> lock(freeze_mutex);
                resolve_stats(pid, stats);
                refresh_values(pid, stats);
            }

            std::string target_save;
            for (auto& entry : std::filesystem::directory_iterator(save_dir)) {
                if (entry.path().extension() == ".d2s") {
                    D2SSaveEditor tmp;
                    if (tmp.load(entry.path().string())) {
                        target_save = entry.path().string();
                    }
                }
            }
            if (target_save.empty()) {
                std::cout << "\033[1;31m✗\033[0m No save file found\n";
                continue;
            }

            D2SSaveEditor editor;
            if (!editor.load(target_save)) {
                std::cout << "\033[1;31m✗\033[0m Failed to load save\n";
                continue;
            }

            std::string backup = target_save + ".bak";
            if (!std::filesystem::exists(backup)) {
                std::filesystem::copy_file(target_save, backup);
                std::cout << "Backup: " << backup << "\n";
            }

            int written = 0;
            for (auto& s : stats) {
                if (s.addr == 0 || s.raw_value == 0) continue;
                int bl = D2SSaveEditor::stat_bit_length_pub(s.code);
                if (bl == 0) continue;
                editor.set_stat(s.code, s.raw_value);
                written++;
            }
            if (editor.save(target_save)) {
                std::cout << "\033[1;32m✓\033[0m " << editor.character_name()
                          << ": saved " << written << " stats\n";
            } else {
                std::cout << "\033[1;31m✗\033[0m Failed to write save\n";
            }
            continue;
        }

        if (line == "r" || line == "refresh") {
            if (pid <= 0) { std::cout << "Not connected. Use \033[1mc\033[0m first.\n"; continue; }
            std::lock_guard<std::mutex> lock(freeze_mutex);
            refresh_values(pid, stats);
            std::cout << "\n";
            print_stats(stats);
            if (god_mode) {
                std::lock_guard<std::mutex> lock(god_mutex);
                std::cout << "\n  \033[1;31m⚔ GOD MODE\033[0m - "
                          << god_addrs.size() << " arrays, "
                          << god_fixes.load() << " fixes / "
                          << god_checks.load() << " checks, "
                          << god_rescan_count.load() << " rescans\n";
                for (auto& ga : god_addrs) {
                    int32_t hp = 0, maxhp = 0;
                    read_memory(pid, ga.hp_addr, &hp, 4);
                    read_memory(pid, ga.maxhp_addr, &maxhp, 4);
                    std::cout << "    HP: " << std::hex << ga.hp_addr << std::dec
                              << " = " << (hp >> 8) << "/" << (maxhp >> 8) << "\n";
                }
                if (!frozen.empty()) {
                    std::cout << "  Freeze: " << freeze_writes.load() << " writes, "
                              << freeze_fails.load() << " fails, "
                              << freeze_arrays.load() << " arrays found, "
                              << frozen.size() << " entries\n";
                }
            }
            std::cout << "\n";
            continue;
        }

        if (line == "R" || line == "rescan") {
            if (pid <= 0) { std::cout << "Not connected. Use \033[1mc\033[0m first.\n"; continue; }
            std::lock_guard<std::mutex> lock(freeze_mutex);
            std::cout << "Re-scanning for stat array...\n";
            if (resolve_stats(pid, stats)) {
                std::cout << "Found!\n\n";
                print_stats(stats);
            } else {
                std::cout << "\033[1;31mStat array not found. Are you in a game?\033[0m\n";
            }
            std::cout << "\n";
            continue;
        }

        if (line == "d" || line == "dump") {
            if (pid <= 0) { std::cout << "Not connected. Use \033[1mc\033[0m first.\n"; continue; }
            dump_stat_region(pid);
            continue;
        }

        // threads - list D2R threads (useful for identifying integrity scanner)
        if (line == "threads" || line == "thr") {
            if (pid <= 0) { std::cout << "Not connected. Use \033[1mc\033[0m first.\n"; continue; }
            std::string task_path = "/proc/" + std::to_string(pid) + "/task";
            std::vector<std::pair<int, std::string>> threads;
            for (auto& entry : std::filesystem::directory_iterator(task_path)) {
                int tid = std::stoi(entry.path().filename().string());
                std::string stat_path = task_path + "/" + std::to_string(tid) + "/stat";
                std::ifstream sf(stat_path);
                std::string stat_line;
                std::getline(sf, stat_line);
                // Parse: pid (comm) state ...
                auto p1 = stat_line.find('(');
                auto p2 = stat_line.rfind(')');
                std::string comm = (p1 != std::string::npos && p2 != std::string::npos)
                    ? stat_line.substr(p1 + 1, p2 - p1 - 1) : "?";
                char state = (p2 + 2 < stat_line.size()) ? stat_line[p2 + 2] : '?';
                // Parse utime and stime (fields 14 and 15, 1-indexed after comm)
                std::istringstream iss(stat_line.substr(p2 + 2));
                std::string field;
                long utime = 0, stime = 0;
                for (int f = 1; f <= 13; f++) iss >> field;  // skip to field 14
                iss >> utime >> stime;
                char buf[256];
                snprintf(buf, sizeof(buf), "  TID %6d  [%c]  %-20s  cpu: %ld+%ld",
                         tid, state, comm.c_str(), utime, stime);
                threads.push_back({tid, buf});
            }
            std::sort(threads.begin(), threads.end());
            std::cout << "D2R threads (" << threads.size() << "):\n";
            for (auto& [tid, info] : threads) std::cout << info << "\n";
            continue;
        }

        // Raw write by dump index: p<idx> <value> (e.g. p17 5)
        if (line[0] == 'p' && line.size() > 1 && (std::isdigit(line[1]) || line[1] == '-')) {
            size_t space = line.find(' ');
            if (space == std::string::npos) {
                std::cout << "Usage: p<dump_idx> <value> (write to dump entry by index)\n";
                continue;
            }
            int dump_idx = std::stoi(line.substr(1, space - 1));
            uint32_t val = static_cast<uint32_t>(std::stoul(line.substr(space + 1)));

            // Find the stat array base address
            auto found = find_stat_array(pid);
            if (found.count(0) == 0) {
                std::cout << "Stat array not found\n";
                continue;
            }
            // code 0 (Str) value_addr points at value field (+4 into entry)
            uintptr_t array_base = found[0].value_addr - 4;
            uintptr_t target = array_base + dump_idx * 8 + 4; // +4 for value field

            if (write_memory(pid, target, &val, sizeof(val))) {
                std::cout << "\033[1;32m✓\033[0m Wrote " << val
                          << " to dump entry [" << dump_idx << "] @ 0x"
                          << std::hex << target << std::dec << "\n";
            } else {
                std::cout << "\033[1;31m✗\033[0m Failed to write\n";
            }
            continue;
        }

        // Set: s7 99999999
        if (line[0] == 's' && line.size() > 1 && std::isdigit(line[1])) {
            size_t space = line.find(' ');
            if (space == std::string::npos) {
                std::cout << "Usage: s<N> <value>\n";
                continue;
            }
            size_t idx = std::stoul(line.substr(1, space - 1)) - 1;
            int32_t val = std::stoi(line.substr(space + 1));

            if (idx >= stats.size()) {
                std::cout << "Invalid index\n";
                continue;
            }

            std::lock_guard<std::mutex> lock(freeze_mutex);
            resolve_stats(pid, stats);
            if (stats[idx].addr == 0) {
                std::cout << "Stat not found in memory\n";
                continue;
            }

            if (write_memory(pid, stats[idx].addr, &val, sizeof(val))) {
                std::cout << "\033[1;32m✓\033[0m " << stats[idx].name
                          << " set to " << val << "\n";

                // Auto-save gold/stash to .d2s file
                uint16_t code = stats[idx].code;
                if ((code == STAT_GOLD || code == STAT_GOLDSTASH) && !save_dir.empty()) {
                    for (auto& entry : std::filesystem::directory_iterator(save_dir)) {
                        if (entry.path().extension() != ".d2s") continue;
                        D2SSaveEditor editor;
                        if (!editor.load(entry.path().string())) continue;
                        std::string bak = entry.path().string() + ".bak";
                        if (!std::filesystem::exists(bak))
                            std::filesystem::copy_file(entry.path(), bak);
                        editor.set_stat(code, static_cast<uint32_t>(val));
                        if (editor.save(entry.path().string())) {
                            std::cout << "  \033[1;33m↳ saved to " << entry.path().filename().string() << "\033[0m\n";
                        }
                    }
                }
            } else {
                std::cout << "\033[1;31m✗\033[0m Failed to write\n";
            }
            continue;
        }

        // Freeze: f7 99999999
        if (line[0] == 'f' && line.size() > 1 && std::isdigit(line[1])) {
            size_t space = line.find(' ');
            if (space == std::string::npos) {
                std::cout << "Usage: f<N> <value>\n";
                continue;
            }
            size_t idx = std::stoul(line.substr(1, space - 1)) - 1;
            int32_t val = std::stoi(line.substr(space + 1));

            if (idx >= stats.size()) {
                std::cout << "Invalid index\n";
                continue;
            }

            std::lock_guard<std::mutex> lock(freeze_mutex);
            frozen.erase(
                std::remove_if(frozen.begin(), frozen.end(),
                    [idx](const FreezeEntry& f) { return f.index == idx; }),
                frozen.end());

            frozen.push_back({idx, val});
            std::cout << "\033[1;34m❄\033[0m " << stats[idx].name
                      << " frozen at " << val << "\n";
            continue;
        }

        // Unfreeze: u7
        if (line[0] == 'u' && line.size() > 1 && std::isdigit(line[1])) {
            size_t idx = std::stoul(line.substr(1)) - 1;
            std::lock_guard<std::mutex> lock(freeze_mutex);
            frozen.erase(
                std::remove_if(frozen.begin(), frozen.end(),
                    [idx](const FreezeEntry& f) { return f.index == idx; }),
                frozen.end());
            if (idx < stats.size()) {
                std::cout << "\033[1;32m✓\033[0m " << stats[idx].name
                          << " unfrozen\n";
            }
            continue;
        }

        std::cout << "Unknown command. Type 'r' to refresh, 'q' to quit.\n";
    }

    god_mode = false;
    running = false;
    god_thread.join();
    freeze_thread.join();

    return 0;
}
