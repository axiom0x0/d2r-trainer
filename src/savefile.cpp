#include "savefile.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iostream>

// Bit lengths for stats (D2R version 97+)
int D2SSaveEditor::stat_bit_length(uint16_t id) {
    switch (id) {
        case 0:  return 10; // Strength
        case 1:  return 10; // Energy
        case 2:  return 10; // Dexterity
        case 3:  return 10; // Vitality
        case 4:  return 10; // Stat points
        case 5:  return 8;  // Skill points
        case 6:  return 21; // Current HP
        case 7:  return 21; // Max HP
        case 8:  return 21; // Current Mana
        case 9:  return 21; // Max Mana
        case 10: return 21; // Current Stamina
        case 11: return 21; // Max Stamina
        case 12: return 7;  // Level
        case 13: return 32; // Experience
        case 14: return 25; // Gold
        case 15: return 25; // Gold Stash
        default: return 0;
    }
}

// Read bits from the data buffer. D2S uses reversed bit order per byte.
uint32_t D2SSaveEditor::read_bits(size_t bit_offset, int count) const {
    uint32_t result = 0;
    for (int i = 0; i < count; i++) {
        size_t byte_idx = (bit_offset + i) / 8;
        int bit_idx = (bit_offset + i) % 8;
        if (byte_idx >= data_.size()) break;

        // Bits are stored in reverse order within each byte
        uint8_t byte_val = data_[byte_idx];
        int bit = (byte_val >> bit_idx) & 1;
        result |= (bit << i);
    }
    return result;
}

void D2SSaveEditor::write_bits(size_t bit_offset, int count, uint32_t value) {
    for (int i = 0; i < count; i++) {
        size_t byte_idx = (bit_offset + i) / 8;
        int bit_idx = (bit_offset + i) % 8;
        if (byte_idx >= data_.size()) break;

        int bit = (value >> i) & 1;
        if (bit) {
            data_[byte_idx] |= (1 << bit_idx);
        } else {
            data_[byte_idx] &= ~(1 << bit_idx);
        }
    }
}

std::vector<D2SSaveEditor::StatLocation> D2SSaveEditor::parse_stats() const {
    std::vector<StatLocation> result;
    if (stats_offset_ == 0) return result;

    // Start after the "gf" marker (2 bytes = 16 bits)
    size_t bit_pos = (stats_offset_ + 2) * 8;

    while (true) {
        // Read 9-bit stat ID
        uint32_t id = read_bits(bit_pos, 9);
        bit_pos += 9;

        if (id == 0x1FF) break; // terminator

        int bit_len = stat_bit_length(id);
        if (bit_len == 0) {
            // Unknown stat, can't continue
            break;
        }

        uint32_t value = read_bits(bit_pos, bit_len);

        StatLocation loc;
        loc.id = id;
        loc.bit_offset = bit_pos;
        loc.bit_length = bit_len;
        loc.value = value;
        result.push_back(loc);

        bit_pos += bit_len;
    }

    return result;
}

bool D2SSaveEditor::load(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) return false;

    data_.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    file.close();

    if (data_.size() < 768) return false;

    // Verify signature
    uint32_t sig;
    memcpy(&sig, data_.data(), 4);
    if (sig != 0xAA55AA55) return false;

    // Find the "gf" stats marker at byte 765
    stats_offset_ = 765;
    if (data_[stats_offset_] == 'g' && data_[stats_offset_ + 1] == 'f') {
        return true;
    }

    // If not at 765, search for it
    for (size_t i = 700; i < data_.size() - 1; i++) {
        if (data_[i] == 'g' && data_[i + 1] == 'f') {
            stats_offset_ = i;
            return true;
        }
    }

    return false;
}

bool D2SSaveEditor::save(const std::string& path) {
    fix_checksum();

    // Update file size field
    uint32_t fsize = data_.size();
    memcpy(data_.data() + 8, &fsize, 4);

    std::ofstream file(path, std::ios::binary);
    if (!file.is_open()) return false;
    file.write(reinterpret_cast<const char*>(data_.data()), data_.size());
    return file.good();
}

void D2SSaveEditor::fix_checksum() {
    // Zero out the checksum field first
    memset(data_.data() + 12, 0, 4);

    uint32_t sum = 0;
    for (size_t i = 0; i < data_.size(); i++) {
        sum = ((sum << 1) | (sum >> 31)) + data_[i];
    }

    memcpy(data_.data() + 12, &sum, 4);
}

bool D2SSaveEditor::has_stat(uint16_t id) const {
    auto stats = parse_stats();
    for (auto& s : stats) {
        if (s.id == id) return true;
    }
    return false;
}

uint32_t D2SSaveEditor::get_stat(uint16_t id) const {
    auto stats = parse_stats();
    for (auto& s : stats) {
        if (s.id == id) return s.value;
    }
    return 0;
}

void D2SSaveEditor::set_stat(uint16_t id, uint32_t value) {
    auto stats = parse_stats();

    // Check if the stat exists
    bool found = false;
    for (auto& s : stats) {
        if (s.id == id) {
            // Check if value fits in the bit length
            uint32_t max_val = (1ULL << s.bit_length) - 1;
            if (value > max_val) value = max_val;
            write_bits(s.bit_offset, s.bit_length, value);
            found = true;
            break;
        }
    }

    if (!found) {
        // Stat doesn't exist yet - need to rebuild the section to insert it
        auto all = all_stats();
        all[id] = value;
        rebuild_stats(all);
    }
}

std::string D2SSaveEditor::character_name() const {
    if (data_.size() < 320) return "";

    // Version check
    uint32_t version;
    memcpy(&version, data_.data() + 4, 4);

    // Search for the name - different versions use different offsets
    // Common offsets: 20 (classic D2), 267 (D2R early), 299 (D2R v105+)
    std::vector<size_t> try_offsets = {299, 267, 20};
    for (size_t off : try_offsets) {
        if (off + 16 > data_.size()) continue;
        // Check if it looks like a valid name (ASCII letters)
        char c = data_[off];
        if (c >= 'A' && c <= 'z') {
            char name[17] = {};
            memcpy(name, data_.data() + off, 16);
            return std::string(name);
        }
    }
    return "";
}

std::map<uint16_t, uint32_t> D2SSaveEditor::all_stats() const {
    std::map<uint16_t, uint32_t> result;
    auto stats = parse_stats();
    for (auto& s : stats) {
        result[s.id] = s.value;
    }
    return result;
}

void D2SSaveEditor::rebuild_stats(const std::map<uint16_t, uint32_t>& stats) {
    // Calculate total bits needed for the new stats section
    int total_bits = 16; // "gf" marker

    for (auto& [id, value] : stats) {
        int bit_len = stat_bit_length(id);
        if (bit_len == 0) continue;
        total_bits += 9 + bit_len; // 9-bit id + value bits
    }
    total_bits += 9; // terminator 0x1FF

    int total_bytes = (total_bits + 7) / 8;

    // Find the end of the current stats section
    size_t old_bit_pos = (stats_offset_ + 2) * 8;
    while (true) {
        uint32_t id = read_bits(old_bit_pos, 9);
        old_bit_pos += 9;
        if (id == 0x1FF) break;
        int bit_len = stat_bit_length(id);
        if (bit_len == 0) break;
        old_bit_pos += bit_len;
    }
    size_t old_end_byte = (old_bit_pos + 7) / 8;
    size_t old_section_bytes = old_end_byte - stats_offset_;

    // Build new section
    std::vector<uint8_t> new_section(total_bytes, 0);
    // Write "gf"
    new_section[0] = 'g';
    new_section[1] = 'f';

    size_t bit_pos = 16;
    for (auto& [id, value] : stats) {
        int bit_len = stat_bit_length(id);
        if (bit_len == 0) continue;

        uint32_t max_val = (1ULL << bit_len) - 1;
        uint32_t clamped = std::min(value, max_val);

        // Write 9-bit ID
        for (int i = 0; i < 9; i++) {
            size_t byte_idx = bit_pos / 8;
            int bit_idx = bit_pos % 8;
            int bit = (id >> i) & 1;
            if (bit) new_section[byte_idx] |= (1 << bit_idx);
            bit_pos++;
        }

        // Write value
        for (int i = 0; i < bit_len; i++) {
            size_t byte_idx = bit_pos / 8;
            int bit_idx = bit_pos % 8;
            int bit = (clamped >> i) & 1;
            if (bit) new_section[byte_idx] |= (1 << bit_idx);
            bit_pos++;
        }
    }

    // Write terminator 0x1FF (9 bits, all 1)
    for (int i = 0; i < 9; i++) {
        size_t byte_idx = bit_pos / 8;
        int bit_idx = bit_pos % 8;
        new_section[byte_idx] |= (1 << bit_idx);
        bit_pos++;
    }

    // Replace old section with new section in data_
    auto it_start = data_.begin() + stats_offset_;
    auto it_end = data_.begin() + stats_offset_ + old_section_bytes;
    data_.erase(it_start, it_end);
    data_.insert(data_.begin() + stats_offset_, new_section.begin(), new_section.end());
}
