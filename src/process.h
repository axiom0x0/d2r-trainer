#pragma once

#include <cstdint>
#include <string>
#include <sys/types.h>

pid_t find_process(const std::string& name);
uintptr_t find_module_base(pid_t pid, const std::string& module_name);
