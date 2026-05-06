#pragma once

#include <cstdint>
#include <cstddef>
#include <map>
#include <string>
#include <vector>
#include <sys/types.h>

bool read_memory(pid_t pid, uintptr_t addr, void* buf, size_t size);
bool write_memory(pid_t pid, uintptr_t addr, const void* buf, size_t size);
uintptr_t walk_pointer_chain(pid_t pid, uintptr_t base, const std::vector<uintptr_t>& offsets);

struct D2RStat {
    uint16_t code;
    uint32_t value;
    uintptr_t value_addr;
};

std::map<uint16_t, D2RStat> find_stat_array(pid_t pid);
std::vector<std::map<uint16_t, D2RStat>> find_all_stat_arrays(pid_t pid);

void dump_stat_region(pid_t pid, int before = 10, int after = 40);
