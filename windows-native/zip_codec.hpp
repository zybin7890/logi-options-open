#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <map>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace zip_codec {

inline std::uint32_t crc32(std::span<const std::uint8_t> data) {
    std::uint32_t crc = 0xFFFFFFFFu;
    for (std::uint8_t byte : data) {
        crc ^= byte;
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

inline std::uint16_t u16(std::span<const std::uint8_t> b, std::size_t off) {
    return static_cast<std::uint16_t>(b[off]) | (static_cast<std::uint16_t>(b[off + 1]) << 8);
}

inline std::uint32_t u32(std::span<const std::uint8_t> b, std::size_t off) {
    return static_cast<std::uint32_t>(b[off]) | (static_cast<std::uint32_t>(b[off + 1]) << 8) |
           (static_cast<std::uint32_t>(b[off + 2]) << 16) | (static_cast<std::uint32_t>(b[off + 3]) << 24);
}

inline void push_u16(std::vector<std::uint8_t>& out, std::uint16_t v) {
    out.push_back(static_cast<std::uint8_t>(v & 0xFF));
    out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
}

inline void push_u32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    out.push_back(static_cast<std::uint8_t>(v & 0xFF));
    out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((v >> 16) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((v >> 24) & 0xFF));
}

namespace detail {

struct BitReader {
    std::span<const std::uint8_t> src;
    std::size_t byte_pos{};
    std::uint32_t bit_buf{};
    int bit_count{};

    void ensure(int n) {
        while (bit_count < n) {
            if (byte_pos >= src.size()) throw std::runtime_error("Truncated deflate stream");
            bit_buf |= static_cast<std::uint32_t>(src[byte_pos++]) << bit_count;
            bit_count += 8;
        }
    }
    std::uint32_t read(int n) {
        if (n == 0) return 0;
        ensure(n);
        const std::uint32_t v = bit_buf & ((1u << n) - 1u);
        bit_buf >>= n;
        bit_count -= n;
        return v;
    }
    void align_byte() {
        const int discard = bit_count & 7;
        bit_buf >>= discard;
        bit_count -= discard;
    }
    void copy_bytes(std::size_t len, std::vector<std::uint8_t>& out) {
        align_byte();
        while (bit_count >= 8 && len > 0) {
            out.push_back(static_cast<std::uint8_t>(bit_buf & 0xFF));
            bit_buf >>= 8;
            bit_count -= 8;
            --len;
        }
        if (byte_pos + len > src.size()) throw std::runtime_error("Truncated stored deflate block");
        out.insert(out.end(), src.data() + byte_pos, src.data() + byte_pos + len);
        byte_pos += len;
    }
};

struct Huffman {
    static constexpr int max_bits = 15;
    std::array<int, max_bits + 1> counts{};
    std::array<int, max_bits + 1> offsets{};
    std::vector<int> symbols;

    void build(std::span<const int> lengths) {
        counts.fill(0);
        for (int len : lengths) {
            if (len < 0 || len > max_bits) throw std::runtime_error("Bad Huffman code length");
            if (len) ++counts[static_cast<std::size_t>(len)];
        }
        int total = 0;
        for (int b = 1; b <= max_bits; ++b) {
            offsets[static_cast<std::size_t>(b)] = total;
            total += counts[static_cast<std::size_t>(b)];
        }
        symbols.assign(static_cast<std::size_t>(total), 0);
        auto next_off = offsets;
        for (std::size_t sym = 0; sym < lengths.size(); ++sym) {
            const int len = lengths[sym];
            if (len) symbols[static_cast<std::size_t>(next_off[static_cast<std::size_t>(len)]++)] = static_cast<int>(sym);
        }
    }

    int decode(BitReader& r) const {
        int code = 0, first = 0, index = 0;
        for (int len = 1; len <= max_bits; ++len) {
            code |= static_cast<int>(r.read(1));
            const int count = counts[static_cast<std::size_t>(len)];
            if (code - count < first)
                return symbols[static_cast<std::size_t>(index + (code - first))];
            index += count;
            first = (first + count) << 1;
            code <<= 1;
        }
        throw std::runtime_error("Invalid Huffman symbol");
    }
};

inline std::vector<std::uint8_t> inflate(std::span<const std::uint8_t> compressed, std::size_t expected_size = 0) {
    static constexpr std::array<int, 29> len_base{
        3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258};
    static constexpr std::array<int, 29> len_extra{
        0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
    static constexpr std::array<int, 30> dist_base{
        1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,
        1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};
    static constexpr std::array<int, 30> dist_extra{
        0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};
    static constexpr std::array<int, 19> cl_order{
        16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};

    BitReader r{compressed};
    std::vector<std::uint8_t> out;
    if (expected_size > 0 && expected_size <= 8 * 1024 * 1024) out.reserve(expected_size);
    bool final_block = false;
    while (!final_block) {
        final_block = r.read(1) != 0;
        const auto type = r.read(2);
        if (type == 0) {
            r.align_byte();
            if (r.byte_pos + 4 > compressed.size()) throw std::runtime_error("Bad stored block");
            const std::uint16_t len = u16(compressed, r.byte_pos);
            const std::uint16_t nlen = u16(compressed, r.byte_pos + 2);
            r.byte_pos += 4;
            if ((len ^ 0xFFFFu) != nlen) throw std::runtime_error("Corrupt stored block length");
            r.copy_bytes(len, out);
        } else if (type == 1 || type == 2) {
            Huffman lit_tree, dist_tree;
            if (type == 1) {
                std::vector<int> ll(288), dl(32, 5);
                for (int i = 0; i <= 143; ++i) ll[static_cast<std::size_t>(i)] = 8;
                for (int i = 144; i <= 255; ++i) ll[static_cast<std::size_t>(i)] = 9;
                for (int i = 256; i <= 279; ++i) ll[static_cast<std::size_t>(i)] = 7;
                for (int i = 280; i <= 287; ++i) ll[static_cast<std::size_t>(i)] = 8;
                lit_tree.build(ll);
                dist_tree.build(dl);
            } else {
                const int hlit = static_cast<int>(r.read(5)) + 257;
                const int hdist = static_cast<int>(r.read(5)) + 1;
                const int hclen = static_cast<int>(r.read(4)) + 4;
                std::array<int, 19> cl_len{};
                for (int i = 0; i < hclen; ++i)
                    cl_len[static_cast<std::size_t>(cl_order[static_cast<std::size_t>(i)])] = static_cast<int>(r.read(3));
                Huffman cl_tree;
                cl_tree.build(cl_len);
                std::vector<int> lengths(static_cast<std::size_t>(hlit + hdist), 0);
                int idx = 0;
                while (idx < hlit + hdist) {
                    const int sym = cl_tree.decode(r);
                    if (sym < 16) {
                        lengths[static_cast<std::size_t>(idx++)] = sym;
                    } else {
                        int rep = 0, val = 0;
                        if (sym == 16) {
                            if (idx == 0) throw std::runtime_error("Bad repeat code");
                            val = lengths[static_cast<std::size_t>(idx - 1)];
                            rep = static_cast<int>(r.read(2)) + 3;
                        } else if (sym == 17) {
                            rep = static_cast<int>(r.read(3)) + 3;
                        } else {
                            rep = static_cast<int>(r.read(7)) + 11;
                        }
                        if (idx + rep > hlit + hdist) throw std::runtime_error("Overflow code lengths");
                        for (int k = 0; k < rep; ++k) lengths[static_cast<std::size_t>(idx++)] = val;
                    }
                }
                lit_tree.build(std::span<const int>(lengths).first(static_cast<std::size_t>(hlit)));
                dist_tree.build(std::span<const int>(lengths).subspan(static_cast<std::size_t>(hlit), static_cast<std::size_t>(hdist)));
            }
            for (;;) {
                const int sym = lit_tree.decode(r);
                if (sym < 256) {
                    out.push_back(static_cast<std::uint8_t>(sym));
                } else if (sym == 256) {
                    break;
                } else {
                    const int li = sym - 257;
                    if (li < 0 || li >= 29) throw std::runtime_error("Invalid length symbol");
                    const std::size_t length = static_cast<std::size_t>(len_base[static_cast<std::size_t>(li)]) + r.read(len_extra[static_cast<std::size_t>(li)]);
                    const int di = dist_tree.decode(r);
                    if (di < 0 || di >= 30) throw std::runtime_error("Invalid distance symbol");
                    const std::size_t dist = static_cast<std::size_t>(dist_base[static_cast<std::size_t>(di)]) + r.read(dist_extra[static_cast<std::size_t>(di)]);
                    if (dist == 0 || dist > out.size()) throw std::runtime_error("Invalid backreference distance");
                    for (std::size_t k = 0; k < length; ++k)
                        out.push_back(out[out.size() - dist]);
                }
                if (out.size() > 16 * 1024 * 1024) throw std::runtime_error("Decompressed entry too large");
            }
        } else {
            throw std::runtime_error("Unsupported deflate block type");
        }
    }
    return out;
}

} // namespace detail

inline bool is_zip(std::span<const std::uint8_t> bytes) {
    return bytes.size() >= 4 && bytes[0] == 'P' && bytes[1] == 'K' && bytes[2] == 3 && bytes[3] == 4;
}

inline std::map<std::string, std::vector<std::uint8_t>> unpack(std::span<const std::uint8_t> zip_bytes) {
    if (zip_bytes.size() < 22) throw std::runtime_error("Invalid ZIP archive");
    std::size_t eocd = zip_bytes.size() - 22;
    const std::size_t floor = zip_bytes.size() > 65557 ? zip_bytes.size() - 65557 : 0;
    bool found_eocd = false;
    for (std::size_t i = eocd;; --i) {
        if (zip_bytes[i] == 'P' && zip_bytes[i + 1] == 'K' && zip_bytes[i + 2] == 5 && zip_bytes[i + 3] == 6) {
            eocd = i;
            found_eocd = true;
            break;
        }
        if (i == floor) break;
    }
    if (!found_eocd) throw std::runtime_error("Missing ZIP EOCD");
    const std::uint16_t entries = u16(zip_bytes, eocd + 10);
    const std::uint32_t cd_offset = u32(zip_bytes, eocd + 16);
    std::size_t pos = cd_offset;
    std::map<std::string, std::vector<std::uint8_t>> files;
    for (std::uint16_t i = 0; i < entries; ++i) {
        if (pos + 46 > zip_bytes.size() || zip_bytes[pos] != 'P' || zip_bytes[pos + 1] != 'K' ||
            zip_bytes[pos + 2] != 1 || zip_bytes[pos + 3] != 2)
            throw std::runtime_error("Invalid ZIP central directory entry");
        const std::uint16_t method = u16(zip_bytes, pos + 10);
        const std::uint32_t expected_crc = u32(zip_bytes, pos + 16);
        const std::uint32_t comp_size = u32(zip_bytes, pos + 20);
        const std::uint32_t uncomp_size = u32(zip_bytes, pos + 24);
        const std::uint16_t name_len = u16(zip_bytes, pos + 28);
        const std::uint16_t extra_len = u16(zip_bytes, pos + 30);
        const std::uint16_t comm_len = u16(zip_bytes, pos + 32);
        const std::uint32_t lfh_offset = u32(zip_bytes, pos + 42);
        if (pos + 46 + name_len + extra_len + comm_len > zip_bytes.size())
            throw std::runtime_error("Truncated ZIP central directory name");
        std::string name(reinterpret_cast<const char*>(zip_bytes.data() + pos + 46), name_len);
        pos += 46 + name_len + extra_len + comm_len;
        if (lfh_offset + 30 > zip_bytes.size() || zip_bytes[lfh_offset] != 'P' || zip_bytes[lfh_offset + 1] != 'K' ||
            zip_bytes[lfh_offset + 2] != 3 || zip_bytes[lfh_offset + 3] != 4)
            throw std::runtime_error("Invalid ZIP local header");
        const std::uint16_t l_name = u16(zip_bytes, lfh_offset + 26);
        const std::uint16_t l_extra = u16(zip_bytes, lfh_offset + 28);
        const std::size_t data_off = static_cast<std::size_t>(lfh_offset) + 30 + l_name + l_extra;
        if (data_off + comp_size > zip_bytes.size()) throw std::runtime_error("Truncated ZIP file payload");
        const auto comp_span = zip_bytes.subspan(data_off, comp_size);
        std::vector<std::uint8_t> data;
        if (method == 0) {
            data.assign(comp_span.begin(), comp_span.end());
        } else if (method == 8) {
            data = detail::inflate(comp_span, uncomp_size);
        } else {
            throw std::runtime_error("Unsupported ZIP compression method");
        }
        if (crc32(data) != expected_crc) throw std::runtime_error("ZIP CRC mismatch");
        if (!name.empty() && name.back() != '/') files.emplace(std::move(name), std::move(data));
    }
    return files;
}

inline std::vector<std::uint8_t> pack_stored(const std::map<std::string, std::vector<std::uint8_t>>& files) {
    std::vector<std::uint8_t> out;
    struct CentralRecord {
        std::string name;
        std::uint32_t crc;
        std::uint32_t size;
        std::uint32_t offset;
    };
    std::vector<CentralRecord> records;
    records.reserve(files.size());
    for (const auto& [name, data] : files) {
        const auto offset = static_cast<std::uint32_t>(out.size());
        const auto crc = crc32(data);
        const auto size = static_cast<std::uint32_t>(data.size());
        records.push_back({name, crc, size, offset});
        out.insert(out.end(), {'P', 'K', 3, 4});
        push_u16(out, 20);
        push_u16(out, 0x0800); // UTF-8 filename flag
        push_u16(out, 0);      // stored
        push_u16(out, 0);
        push_u16(out, 0);
        push_u32(out, crc);
        push_u32(out, size);
        push_u32(out, size);
        push_u16(out, static_cast<std::uint16_t>(name.size()));
        push_u16(out, 0);
        out.insert(out.end(), name.begin(), name.end());
        out.insert(out.end(), data.begin(), data.end());
    }
    const auto cd_offset = static_cast<std::uint32_t>(out.size());
    for (const auto& rec : records) {
        out.insert(out.end(), {'P', 'K', 1, 2});
        push_u16(out, 20);
        push_u16(out, 20);
        push_u16(out, 0x0800);
        push_u16(out, 0);
        push_u16(out, 0);
        push_u16(out, 0);
        push_u32(out, rec.crc);
        push_u32(out, rec.size);
        push_u32(out, rec.size);
        push_u16(out, static_cast<std::uint16_t>(rec.name.size()));
        push_u16(out, 0);
        push_u16(out, 0);
        push_u16(out, 0);
        push_u16(out, 0);
        push_u32(out, 0);
        push_u32(out, rec.offset);
        out.insert(out.end(), rec.name.begin(), rec.name.end());
    }
    const auto cd_size = static_cast<std::uint32_t>(out.size() - cd_offset);
    out.insert(out.end(), {'P', 'K', 5, 6});
    push_u16(out, 0);
    push_u16(out, 0);
    push_u16(out, static_cast<std::uint16_t>(records.size()));
    push_u16(out, static_cast<std::uint16_t>(records.size()));
    push_u32(out, cd_size);
    push_u32(out, cd_offset);
    push_u16(out, 0);
    return out;
}

} // namespace zip_codec
