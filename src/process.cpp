#include "process.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include <cstdint>
#include <sys/uio.h>

namespace fs = std::filesystem;

pid_t find_process(const std::string& name) {
    for (auto& entry : fs::directory_iterator("/proc")) {
        if (!entry.is_directory()) continue;

        std::string pid_str = entry.path().filename().string();
        if (!std::all_of(pid_str.begin(), pid_str.end(), ::isdigit)) continue;

        std::ifstream cmdline(entry.path() / "cmdline");
        if (!cmdline.is_open()) continue;

        std::string cmd;
        std::getline(cmdline, cmd, '\0');

        if (cmd.find(name) != std::string::npos) {
            return std::stoi(pid_str);
        }
    }
    return -1;
}

// Read a small chunk from another process's memory.
static bool peek(pid_t pid, uintptr_t addr, void* buf, size_t size) {
    struct iovec local  = { buf, size };
    struct iovec remote = { reinterpret_cast<void*>(addr), size };
    return process_vm_readv(pid, &local, 1, &remote, 1, 0) == static_cast<ssize_t>(size);
}

uintptr_t find_module_base(pid_t pid, const std::string& module_name) {
    std::string maps_path = "/proc/" + std::to_string(pid) + "/maps";
    std::ifstream maps(maps_path);
    if (!maps.is_open()) return 0;

    // First try: match by filename in maps (works on plain Wine)
    std::string line;
    while (std::getline(maps, line)) {
        if (line.find(module_name) != std::string::npos) {
            std::string addr_range = line.substr(0, line.find(' '));
            std::string start_addr = addr_range.substr(0, addr_range.find('-'));
            return std::stoull(start_addr, nullptr, 16);
        }
    }

    // Second try: Wine/Proton maps the PE as anonymous memfd regions.
    // Scan readable memfd:wine-mapping regions for a valid PE header.
    // Only read the first 4 bytes of each region (MZ check) plus a small
    // PE header read - this is very lightweight.
    maps.clear();
    maps.seekg(0);

    struct Region {
        uintptr_t start;
        uintptr_t end;
    };
    std::vector<Region> candidates;

    while (std::getline(maps, line)) {
        // Only look at readable memfd:wine-mapping regions > 1MB
        if (line.find("memfd:wine-mapping") == std::string::npos) continue;
        if (line.find('r') == std::string::npos) continue;

        std::string addr_range = line.substr(0, line.find(' '));
        auto dash = addr_range.find('-');
        uintptr_t start = std::stoull(addr_range.substr(0, dash), nullptr, 16);
        uintptr_t end   = std::stoull(addr_range.substr(dash + 1), nullptr, 16);

        if (end - start > 1024 * 1024) {
            candidates.push_back({start, end});
        }
    }

    for (auto& reg : candidates) {
        uint8_t mz[2] = {0};
        if (!peek(pid, reg.start, mz, 2)) continue;
        if (mz[0] != 'M' || mz[1] != 'Z') continue;

        // Read PE offset at 0x3C
        uint32_t pe_off = 0;
        if (!peek(pid, reg.start + 0x3C, &pe_off, 4)) continue;
        if (pe_off == 0 || pe_off > 0x1000) continue;

        // Verify PE signature
        uint32_t pe_sig = 0;
        if (!peek(pid, reg.start + pe_off, &pe_sig, 4)) continue;
        if (pe_sig != 0x00004550) continue; // "PE\0\0"

        // Check size - D2R.exe is large (>10MB), skip small PEs
        if (reg.end - reg.start > 10 * 1024 * 1024) {
            return reg.start;
        }
    }

    return 0;
}
