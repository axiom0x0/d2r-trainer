#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct CheatEntry {
    std::string name;
    uintptr_t base_offset;
    std::vector<uintptr_t> offsets;
    std::string type;       // "int32", "int16", "float", "int64"
    bool god = false;       // if true, included in god mode toggle
    std::string god_value;  // value to freeze at when god mode is on
};

struct TrainerConfig {
    std::string process_name;
    std::string module;
    std::vector<CheatEntry> cheats;
};

TrainerConfig load_config(const std::string& path);
size_t type_size(const std::string& type);
