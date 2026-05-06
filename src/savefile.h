#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

// D2S save file editor for D2R
// Handles the bitfield-encoded stats section

struct D2SStat {
    uint16_t id;
    uint32_t value;
};

class D2SSaveEditor {
public:
    bool load(const std::string& path);
    bool save(const std::string& path);

    // Get/set stats by ID (0=str, 6=hp, 14=gold, etc.)
    bool has_stat(uint16_t id) const;
    uint32_t get_stat(uint16_t id) const;
    void set_stat(uint16_t id, uint32_t value);

    std::string character_name() const;
    std::map<uint16_t, uint32_t> all_stats() const;

    static int stat_bit_length_pub(uint16_t id) { return stat_bit_length(id); }

private:
    std::vector<uint8_t> data_;
    size_t stats_offset_ = 0;

    static int stat_bit_length(uint16_t id);
    uint32_t read_bits(size_t bit_offset, int count) const;
    void write_bits(size_t bit_offset, int count, uint32_t value);

    struct StatLocation {
        uint16_t id;
        size_t bit_offset;
        int bit_length;
        uint32_t value;
    };
    std::vector<StatLocation> parse_stats() const;

    void rebuild_stats(const std::map<uint16_t, uint32_t>& stats);
    void fix_checksum();
};
