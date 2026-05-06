// D2R Trainer - ncurses TUI

#include "memory.h"
#include "process.h"
#include "savefile.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <clocale>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <ncurses.h>

// ─── Stat Codes ─────────────────────────────────────────────────────────────

enum StatCode : uint16_t {
    STAT_STRENGTH = 0, STAT_ENERGY = 1, STAT_DEXTERITY = 2, STAT_VITALITY = 3,
    STAT_STATPTS = 4, STAT_SKILLPTS = 5,
    STAT_HP = 6, STAT_MAXHP = 7, STAT_MANA = 8, STAT_MAXMANA = 9,
    STAT_STAMINA = 10, STAT_MAXSTAMINA = 11,
    STAT_LEVEL = 12, STAT_EXPERIENCE = 13,
    STAT_GOLD = 14, STAT_GOLDSTASH = 15,
};

struct TrainerStat {
    std::string name;
    uint16_t code;
    bool shifted;
    uintptr_t addr;
    int32_t raw_value;
};

static std::vector<TrainerStat> build_stat_table() {
    return {
        {"Strength",    0,  false, 0, 0}, {"Energy",      1,  false, 0, 0},
        {"Dexterity",   2,  false, 0, 0}, {"Vitality",    3,  false, 0, 0},
        {"Stat Points", 4,  false, 0, 0}, {"Skill Points",5,  false, 0, 0},
        {"HP",          6,  true,  0, 0}, {"Max HP",       7,  true,  0, 0},
        {"Mana",        8,  true,  0, 0}, {"Max Mana",     9,  true,  0, 0},
        {"Stamina",     10, true,  0, 0}, {"Max Stamina",  11, true,  0, 0},
        {"Level",       12, false, 0, 0}, {"Experience",   13, false, 0, 0},
        {"Gold",        14, false, 0, 0}, {"Gold Stash",   15, false, 0, 0},
    };
}

// ─── Color Pairs ────────────────────────────────────────────────────────────

enum Colors {
    C_NORMAL = 1, C_GREEN, C_RED, C_CYAN, C_YELLOW, C_MAGENTA,
    C_BORDER, C_TITLE, C_GODMODE, C_BAR_HP, C_BAR_MANA, C_BAR_STAM,
    C_BAR_EMPTY, C_HEADER, C_STATUS_ON, C_STATUS_OFF, C_HOTKEY,
};

static void init_colors() {
    start_color();
    use_default_colors();
    init_pair(C_NORMAL,     COLOR_WHITE,   -1);
    init_pair(C_GREEN,      COLOR_GREEN,   -1);
    init_pair(C_RED,        COLOR_RED,     -1);
    init_pair(C_CYAN,       COLOR_CYAN,    -1);
    init_pair(C_YELLOW,     COLOR_YELLOW,  -1);
    init_pair(C_MAGENTA,    COLOR_MAGENTA, -1);
    init_pair(C_BORDER,     COLOR_RED,     -1);
    init_pair(C_TITLE,      COLOR_RED,     -1);
    init_pair(C_GODMODE,    COLOR_RED,     -1);
    init_pair(C_BAR_HP,     COLOR_RED,     -1);
    init_pair(C_BAR_MANA,   COLOR_BLUE,    -1);
    init_pair(C_BAR_STAM,   COLOR_YELLOW,  -1);
    init_pair(C_BAR_EMPTY,  COLOR_WHITE,   -1);
    init_pair(C_HEADER,     COLOR_RED,     -1);
    init_pair(C_STATUS_ON,  COLOR_GREEN,   -1);
    init_pair(C_STATUS_OFF, COLOR_RED,     -1);
    init_pair(C_HOTKEY,     COLOR_RED,     -1);
}

// ─── Drawing Helpers ────────────────────────────────────────────────────────

static void draw_box(int y, int x, int h, int w, const char* title = nullptr) {
    attron(COLOR_PAIR(C_BORDER));
    // Corners
    mvaddch(y, x, ACS_ULCORNER);
    mvaddch(y, x + w - 1, ACS_URCORNER);
    mvaddch(y + h - 1, x, ACS_LLCORNER);
    mvaddch(y + h - 1, x + w - 1, ACS_LRCORNER);
    // Horizontal
    for (int i = 1; i < w - 1; i++) {
        mvaddch(y, x + i, ACS_HLINE);
        mvaddch(y + h - 1, x + i, ACS_HLINE);
    }
    // Vertical
    for (int i = 1; i < h - 1; i++) {
        mvaddch(y + i, x, ACS_VLINE);
        mvaddch(y + i, x + w - 1, ACS_VLINE);
    }
    if (title) {
        mvprintw(y, x + 2, " %s ", title);
    }
    attroff(COLOR_PAIR(C_BORDER));
}

static void draw_bar(int y, int x, int width, int current, int maximum, int color_pair) {
    if (maximum <= 0) maximum = 1;
    int filled = (current * width) / maximum;
    if (filled > width) filled = width;
    if (filled < 0) filled = 0;

    attron(COLOR_PAIR(color_pair) | A_BOLD);
    for (int i = 0; i < filled; i++) mvaddch(y, x + i, ACS_CKBOARD);
    attroff(A_BOLD);

    attron(COLOR_PAIR(C_BAR_EMPTY) | A_DIM);
    for (int i = filled; i < width; i++) mvaddch(y, x + i, ACS_CKBOARD);
    attroff(A_DIM);
    attroff(COLOR_PAIR(C_BAR_EMPTY));
}

static void draw_hotkey(int y, int x, char key, const char* label) {
    attron(COLOR_PAIR(C_HOTKEY) | A_BOLD);
    mvprintw(y, x, "[%c]", key);
    attroff(COLOR_PAIR(C_HOTKEY) | A_BOLD);
    attron(COLOR_PAIR(C_NORMAL));
    printw(" %s", label);
    attroff(COLOR_PAIR(C_NORMAL));
}

static std::string format_num(int64_t n) {
    if (n < 1000) return std::to_string(n);
    if (n < 1000000) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%d,%03d", (int)(n / 1000), (int)(n % 1000));
        return buf;
    }
    char buf[32];
    snprintf(buf, sizeof(buf), "%.1fM", n / 1000000.0);
    return buf;
}

// ─── Globals ────────────────────────────────────────────────────────────────

static pid_t g_pid = 0;
static std::vector<TrainerStat> g_stats;
static std::string g_save_dir;
static std::string g_home;

// God mode state
static std::atomic<bool> g_god_mode{false};
static std::atomic<bool> g_running{true};
static std::atomic<int>  g_god_fixes{0};
static std::atomic<int>  g_god_checks{0};
static std::atomic<int>  g_god_rescan{0};

struct GodAddr {
    uintptr_t hp_addr, maxhp_addr, mana_addr, maxmana_addr;
};
static std::vector<GodAddr> g_god_addrs;
static std::mutex g_god_mutex;

static std::atomic<uint32_t> g_player_str{0};
static std::atomic<uint32_t> g_player_ene{0};
static std::atomic<uint32_t> g_player_dex{0};
static std::atomic<uint32_t> g_player_vit{0};

// Status messages
static std::string g_status_msg;
static std::chrono::steady_clock::time_point g_status_time;
static std::mutex g_status_mutex;

static void set_status(const std::string& msg) {
    std::lock_guard<std::mutex> lock(g_status_mutex);
    g_status_msg = msg;
    g_status_time = std::chrono::steady_clock::now();
}

// ─── Connection / Scanning ──────────────────────────────────────────────────

static bool try_connect() {
    g_pid = find_process("D2R.exe");
    return g_pid > 0;
}

static bool resolve_stats() {
    auto found = find_stat_array(g_pid);
    if (found.empty()) return false;
    for (auto& s : g_stats) {
        auto it = found.find(s.code);
        if (it != found.end()) {
            s.addr = it->second.value_addr;
            s.raw_value = it->second.value;
        }
    }
    return true;
}

static void refresh_values() {
    for (auto& s : g_stats) {
        if (s.addr == 0) continue;
        int32_t val = 0;
        if (read_memory(g_pid, s.addr, &val, sizeof(val)))
            s.raw_value = val;
    }
}

static int display_val(const TrainerStat& s) {
    return s.shifted ? (s.raw_value >> 8) : s.raw_value;
}

// ─── Find Save Dir ──────────────────────────────────────────────────────────

static void find_save_dir() {
    if (getenv("SUDO_USER")) {
        std::string pw_line;
        std::ifstream pw("/etc/passwd");
        std::string sudo_user = getenv("SUDO_USER");
        while (std::getline(pw, pw_line)) {
            if (pw_line.substr(0, sudo_user.size() + 1) == sudo_user + ":") {
                int field = 0;
                for (size_t i = 0; i < pw_line.size(); i++) {
                    if (pw_line[i] == ':') field++;
                    if (field == 5) {
                        size_t end = pw_line.find(':', i + 1);
                        g_home = pw_line.substr(i + 1, end - i - 1);
                        break;
                    }
                }
                break;
            }
        }
    }
    if (g_home.empty()) g_home = getenv("HOME") ? getenv("HOME") : "";

    std::vector<std::string> dirs = {
        g_home + "/Games/battlenet/drive_c/users/steamuser/Saved Games/Diablo II Resurrected",
        g_home + "/Games/battlenet/drive_c/users/" +
            (getenv("SUDO_USER") ? getenv("SUDO_USER") : "") +
            "/Saved Games/Diablo II Resurrected",
    };
    for (auto& d : dirs) {
        if (std::filesystem::exists(d)) { g_save_dir = d; return; }
    }
}

// ─── God Mode Thread ────────────────────────────────────────────────────────

static bool is_sane_vital(int32_t raw_val) {
    if (raw_val <= 0) return false;
    int32_t display = raw_val >> 8;
    if (display <= 0 || display > 50000) return false;
    return true;
}

static void god_thread_fn() {
    while (g_running) {
        if (!g_god_mode || g_pid <= 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }

        // Rescan every 5 seconds
        static auto last_scan = std::chrono::steady_clock::now();
        auto now = std::chrono::steady_clock::now();
        bool do_rescan = g_god_addrs.empty() ||
            std::chrono::duration_cast<std::chrono::seconds>(now - last_scan).count() >= 5;

        if (do_rescan) {
            auto all = find_all_stat_arrays(g_pid);
            uint32_t ps = g_player_str, pe = g_player_ene;
            uint32_t pd = g_player_dex, pv = g_player_vit;

            std::lock_guard<std::mutex> lock(g_god_mutex);
            g_god_addrs.clear();

            for (auto& arr : all) {
                auto s0 = arr.find(0), s1 = arr.find(1);
                auto s2 = arr.find(2), s3 = arr.find(3);
                if (s0 == arr.end() || s1 == arr.end() ||
                    s2 == arr.end() || s3 == arr.end()) continue;

                if (!(s0->second.value >= ps && s0->second.value < ps + 500 &&
                      s1->second.value >= pe && s1->second.value < pe + 500 &&
                      s2->second.value >= pd && s2->second.value < pd + 500 &&
                      s3->second.value >= pv && s3->second.value < pv + 500)) continue;

                auto hp = arr.find(6), maxhp = arr.find(7);
                auto mp = arr.find(8), maxmp = arr.find(9);
                if (hp == arr.end() || maxhp == arr.end()) continue;

                // Reject arrays with garbage vitals
                if (!is_sane_vital(maxhp->second.value)) continue;

                GodAddr ga = {hp->second.value_addr, maxhp->second.value_addr, 0, 0};
                if (mp != arr.end() && maxmp != arr.end() && is_sane_vital(maxmp->second.value)) {
                    ga.mana_addr = mp->second.value_addr;
                    ga.maxmana_addr = maxmp->second.value_addr;
                }
                g_god_addrs.push_back(ga);
            }
            last_scan = now;
            g_god_rescan++;
        }

        {
            std::lock_guard<std::mutex> lock(g_god_mutex);
            for (auto& ga : g_god_addrs) {
                int32_t hp = 0, maxhp = 0;
                read_memory(g_pid, ga.hp_addr, &hp, 4);
                read_memory(g_pid, ga.maxhp_addr, &maxhp, 4);
                g_god_checks++;

                // Safety: skip if values look like garbage
                if (!is_sane_vital(maxhp)) continue;

                if (hp > 0 && hp < maxhp) {
                    write_memory(g_pid, ga.hp_addr, &maxhp, 4);
                    g_god_fixes++;
                }
                if (ga.mana_addr && ga.maxmana_addr) {
                    int32_t mp = 0, maxmp = 0;
                    read_memory(g_pid, ga.mana_addr, &mp, 4);
                    read_memory(g_pid, ga.maxmana_addr, &maxmp, 4);
                    if (is_sane_vital(maxmp) && mp < maxmp)
                        write_memory(g_pid, ga.mana_addr, &maxmp, 4);
                }
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

// ─── Resize Handling ────────────────────────────────────────────────────────

static std::atomic<bool> g_needs_resize{false};

static void handle_winch(int) {
    g_needs_resize = true;
}

// ─── Draw the TUI ───────────────────────────────────────────────────────────

static auto g_start_time = std::chrono::steady_clock::now();

static void draw_screen() {
    erase();
    int W = 64;

    // ─── Outer border + title ───
    draw_box(0, 0, 28, W);

    // Banner
    attron(COLOR_PAIR(C_RED) | A_BOLD);
    mvprintw(1, 3, " Diablo II - Resurrected \\\\ Trainer v.10");
    attroff(COLOR_PAIR(C_RED) | A_BOLD);

    // God mode indicator
    if (g_god_mode) {
        attron(COLOR_PAIR(C_RED) | A_BOLD);
        mvprintw(1, W - 10, "[GODMODE]");
        attroff(COLOR_PAIR(C_RED) | A_BOLD);
    }

    // ─── Status bar ───
    draw_box(2, 1, 3, W - 2, "STATUS");
    int status_color = g_pid > 0 ? C_STATUS_ON : C_STATUS_OFF;
    attron(COLOR_PAIR(status_color) | A_BOLD);
    mvprintw(3, 4, g_pid > 0 ? "CONNECTED" : "DISCONNECTED");
    attroff(COLOR_PAIR(status_color) | A_BOLD);

    attron(COLOR_PAIR(C_NORMAL));
    if (g_pid > 0) mvprintw(3, 18, "PID: %d", g_pid);

    // Uptime
    auto elapsed = std::chrono::steady_clock::now() - g_start_time;
    int secs = std::chrono::duration_cast<std::chrono::seconds>(elapsed).count();
    mvprintw(3, W - 18, "UP: %02d:%02d:%02d", secs / 3600, (secs / 60) % 60, secs % 60);
    attroff(COLOR_PAIR(C_NORMAL));

    // ─── Vitals ───
    draw_box(5, 1, 6, W - 2, "VITALS");

    int bar_w = 30;
    int hp = 0, maxhp = 0, mana = 0, maxmana = 0, stam = 0, maxstam = 0;
    for (auto& s : g_stats) {
        int dv = display_val(s);
        switch (s.code) {
            case 6: hp = dv; break;       case 7: maxhp = dv; break;
            case 8: mana = dv; break;     case 9: maxmana = dv; break;
            case 10: stam = dv; break;    case 11: maxstam = dv; break;
        }
    }

    attron(COLOR_PAIR(C_BAR_HP) | A_BOLD);
    mvprintw(6, 4, "HP  ");
    attroff(COLOR_PAIR(C_BAR_HP) | A_BOLD);
    draw_bar(6, 9, bar_w, hp, maxhp > 0 ? maxhp : 1, C_BAR_HP);
    attron(COLOR_PAIR(C_NORMAL));
    mvprintw(6, 9 + bar_w + 1, "%d/%d", hp, maxhp);
    if (g_god_mode) { attron(COLOR_PAIR(C_RED) | A_BOLD); printw(" %s", "\xf0\x9f\x92\x80"); attroff(COLOR_PAIR(C_RED) | A_BOLD); }
    attroff(COLOR_PAIR(C_NORMAL));

    attron(COLOR_PAIR(C_BAR_MANA) | A_BOLD);
    mvprintw(7, 4, "MANA");
    attroff(COLOR_PAIR(C_BAR_MANA) | A_BOLD);
    draw_bar(7, 9, bar_w, mana, maxmana > 0 ? maxmana : 1, C_BAR_MANA);
    attron(COLOR_PAIR(C_NORMAL));
    mvprintw(7, 9 + bar_w + 1, "%d/%d", mana, maxmana);
    attroff(COLOR_PAIR(C_NORMAL));

    attron(COLOR_PAIR(C_BAR_STAM) | A_BOLD);
    mvprintw(8, 4, "STAM");
    attroff(COLOR_PAIR(C_BAR_STAM) | A_BOLD);
    draw_bar(8, 9, bar_w, stam, maxstam > 0 ? maxstam : 1, C_BAR_STAM);
    attron(COLOR_PAIR(C_NORMAL));
    mvprintw(8, 9 + bar_w + 1, "%d/%d", stam, maxstam);
    attroff(COLOR_PAIR(C_NORMAL));

    int disp[16] = {};
    for (auto& s : g_stats) disp[s.code] = display_val(s);

    // ─── Stats + Economy side by side ───
    draw_box(11, 1, 6, 30, "STATS");
    attron(COLOR_PAIR(C_NORMAL));
    mvprintw(12, 4, "STR  %-6d DEX  %-6d", disp[0], disp[2]);
    mvprintw(13, 4, "ENE  %-6d VIT  %-6d", disp[1], disp[3]);
    mvprintw(14, 4, "LVL  %-6d EXP  %s", disp[12], format_num(disp[13]).c_str());
    mvprintw(15, 4, "PTS  %-6d SKL  %-6d", disp[4], disp[5]);
    attroff(COLOR_PAIR(C_NORMAL));

    draw_box(11, 31, 6, W - 32, "ECONOMY");
    attron(COLOR_PAIR(C_YELLOW) | A_BOLD);
    mvprintw(12, 34, "GOLD");
    attroff(A_BOLD);
    mvprintw(12, 40, "%s", format_num(disp[14]).c_str());
    attron(A_BOLD);
    mvprintw(13, 34, "STASH");
    attroff(A_BOLD);
    mvprintw(13, 40, "%s", format_num(disp[15]).c_str());
    attroff(COLOR_PAIR(C_YELLOW));

    // ─── God Mode Panel ───
    draw_box(17, 1, 5, W - 2, "GOD MODE");

    if (g_god_mode) {
        attron(COLOR_PAIR(C_RED) | A_BOLD);
        mvprintw(18, 4, "ACTIVE");
        attroff(COLOR_PAIR(C_RED) | A_BOLD);
    } else {
        attron(COLOR_PAIR(C_STATUS_OFF) | A_DIM);
        mvprintw(18, 4, "OFF");
        attroff(COLOR_PAIR(C_STATUS_OFF) | A_DIM);
    }

    attron(COLOR_PAIR(C_NORMAL));
    mvprintw(18, 15, "Fixes: %s", format_num(g_god_fixes.load()).c_str());
    mvprintw(19, 4, "HP Guard [%s]  Mana Guard [%s]",
             g_god_mode.load() ? "ON " : "OFF",
             g_god_mode.load() ? "ON " : "OFF");
    attroff(COLOR_PAIR(C_NORMAL));

    // ─── Status message ───
    {
        std::lock_guard<std::mutex> lock(g_status_mutex);
        if (!g_status_msg.empty()) {
            auto age = std::chrono::steady_clock::now() - g_status_time;
            if (std::chrono::duration_cast<std::chrono::seconds>(age).count() < 5) {
                attron(COLOR_PAIR(C_GREEN) | A_BOLD);
                mvprintw(22, 4, "%s", g_status_msg.c_str());
                attroff(COLOR_PAIR(C_GREEN) | A_BOLD);
            }
        }
    }

    // ─── Hotkey bar ───
    draw_box(23, 1, 4, W - 2, "COMMANDS");
    draw_hotkey(24, 4,  'G', "God Mode");
    draw_hotkey(24, 20, 'C', "Connect");
    draw_hotkey(24, 35, 'R', "Rescan");
    draw_hotkey(24, 49, 'Q', "Quit");
    draw_hotkey(25, 4,  'S', "Set Stat");
    draw_hotkey(25, 20, 'D', "Set Gold");
    draw_hotkey(25, 35, 'W', "Save Edit");

    refresh();
}

// ─── Input: prompt for a number ─────────────────────────────────────────────

static bool prompt_int(const char* prompt, int& out_idx, int32_t& out_val) {
    echo();
    curs_set(1);
    attron(COLOR_PAIR(C_CYAN));
    mvprintw(22, 4, "%-56s", "");  // clear line
    mvprintw(22, 4, "%s: ", prompt);
    attroff(COLOR_PAIR(C_CYAN));

    char buf[64] = {};
    getnstr(buf, sizeof(buf) - 1);
    noecho();
    curs_set(0);

    // Parse "idx value"
    int idx = 0;
    int32_t val = 0;
    if (sscanf(buf, "%d %d", &idx, &val) == 2) {
        out_idx = idx;
        out_val = val;
        return true;
    }
    // Single value (for gold shortcut)
    if (sscanf(buf, "%d", &val) == 1) {
        out_idx = -1;
        out_val = val;
        return true;
    }
    return false;
}

// ─── Main ───────────────────────────────────────────────────────────────────

int main() {
    g_stats = build_stat_table();
    find_save_dir();

    // Try initial connect
    if (try_connect()) {
        resolve_stats();
        if (g_stats[0].addr != 0) {
            g_player_str = g_stats[0].raw_value;
            g_player_ene = g_stats[1].raw_value;
            g_player_dex = g_stats[2].raw_value;
            g_player_vit = g_stats[3].raw_value;
        }
    }

    setlocale(LC_ALL, "");

    // Handle terminal resize
    struct sigaction sa = {};
    sa.sa_handler = handle_winch;
    sigaction(SIGWINCH, &sa, nullptr);

    // Init ncurses
    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);
    nodelay(stdscr, TRUE);
    init_colors();

    // Start god mode thread
    std::thread god_thr(god_thread_fn);

    // Main loop: draw + handle input
    while (g_running) {
        if (g_needs_resize.exchange(false)) {
            endwin();
            refresh();
        }

        // Refresh stat values from memory
        if (g_pid > 0) refresh_values();

        draw_screen();

        // Non-blocking input with 100ms timeout
        timeout(100);
        int ch = getch();

        switch (ch) {
        case 'q': case 'Q':
            g_running = false;
            break;

        case 'g': case 'G':
            if (g_pid <= 0) {
                set_status("Not connected. Press C first.");
                break;
            }
            if (!g_god_mode) {
                if (g_stats[0].addr != 0) {
                    g_player_str = g_stats[0].raw_value;
                    g_player_ene = g_stats[1].raw_value;
                    g_player_dex = g_stats[2].raw_value;
                    g_player_vit = g_stats[3].raw_value;
                }
                g_god_fixes = 0;
                g_god_checks = 0;
                g_god_rescan = 0;
                g_god_mode = true;
                set_status("GOD MODE ACTIVATED");
            } else {
                g_god_mode = false;
                {
                    std::lock_guard<std::mutex> lock(g_god_mutex);
                    g_god_addrs.clear();
                }
                set_status("God mode disabled");
            }
            break;

        case 'c': case 'C':
            g_god_mode = false;
            {
                std::lock_guard<std::mutex> lock(g_god_mutex);
                g_god_addrs.clear();
            }
            if (try_connect()) {
                if (resolve_stats()) {
                    g_player_str = g_stats[0].raw_value;
                    g_player_ene = g_stats[1].raw_value;
                    g_player_dex = g_stats[2].raw_value;
                    g_player_vit = g_stats[3].raw_value;
                    set_status("Connected to D2R (PID " + std::to_string(g_pid) + ")");
                } else {
                    set_status("Connected - press R in-game to scan");
                }
            } else {
                set_status("D2R not found");
            }
            break;

        case 'r': case 'R':
            if (g_pid <= 0) { set_status("Not connected"); break; }
            if (resolve_stats()) {
                g_player_str = g_stats[0].raw_value;
                g_player_ene = g_stats[1].raw_value;
                g_player_dex = g_stats[2].raw_value;
                g_player_vit = g_stats[3].raw_value;
                set_status("Scan complete - stats found");
            } else {
                set_status("Stat array not found. In a game?");
            }
            break;

        case 's': case 'S': {
            if (g_pid <= 0) { set_status("Not connected"); break; }
            int idx; int32_t val;
            if (prompt_int("Set stat (# value)", idx, val)) {
                idx--; // 1-based to 0-based
                if (idx >= 0 && idx < (int)g_stats.size() && g_stats[idx].addr != 0) {
                    if (write_memory(g_pid, g_stats[idx].addr, &val, sizeof(val))) {
                        set_status(g_stats[idx].name + " set to " + std::to_string(val));
                    } else {
                        set_status("Write failed");
                    }
                } else {
                    set_status("Invalid stat index");
                }
            }
            break;
        }

        case 'd': case 'D': {
            if (g_pid <= 0) { set_status("Not connected"); break; }
            int idx; int32_t val;
            if (prompt_int("Gold amount", idx, val)) {
                // idx == -1 means single value entered
                int32_t gold = (idx == -1) ? val : idx;
                // Find gold stat
                for (auto& s : g_stats) {
                    if (s.code == STAT_GOLD && s.addr != 0) {
                        write_memory(g_pid, s.addr, &gold, sizeof(gold));
                        break;
                    }
                }
                set_status("Gold set to " + format_num(gold));
            }
            break;
        }

        case 'w': case 'W': {
            if (g_save_dir.empty()) { set_status("Save directory not found"); break; }
            int idx; int32_t val;
            if (prompt_int("Save edit (# value)", idx, val)) {
                idx--;
                if (idx >= 0 && idx < (int)g_stats.size()) {
                    for (auto& entry : std::filesystem::directory_iterator(g_save_dir)) {
                        if (entry.path().extension() != ".d2s") continue;
                        D2SSaveEditor editor;
                        if (!editor.load(entry.path().string())) continue;
                        editor.set_stat(g_stats[idx].code, static_cast<uint32_t>(val));
                        if (editor.save(entry.path().string())) {
                            set_status(g_stats[idx].name + " saved to " + entry.path().filename().string());
                        }
                    }
                } else {
                    set_status("Invalid stat index");
                }
            }
            break;
        }

        case KEY_RESIZE:
            g_needs_resize = true;
            break;

        default:
            break;
        }
    }

    // Cleanup
    g_god_mode = false;
    g_running = false;
    god_thr.join();

    endwin();
    return 0;
}
