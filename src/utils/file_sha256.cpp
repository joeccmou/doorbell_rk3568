#include "utils/file_sha256.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <iomanip>
#include <memory>
#include <sstream>

namespace {
constexpr std::array<uint32_t, 64> kRoundConstants{
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U,
    0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
    0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
    0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
    0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
    0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
    0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
    0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
    0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
    0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
    0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U,
    0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
    0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U,
    0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
    0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
    0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U,
};

uint32_t rotate_right(uint32_t value, uint32_t bits) {
    return (value >> bits) | (value << (32U - bits));
}

class Sha256 {
public:
    Sha256()
        : state_{
              0x6a09e667U,
              0xbb67ae85U,
              0x3c6ef372U,
              0xa54ff53aU,
              0x510e527fU,
              0x9b05688cU,
              0x1f83d9abU,
              0x5be0cd19U,
          } {}

    void update(const unsigned char *bytes, std::size_t size) {
        for (std::size_t index = 0; index < size; ++index) {
            block_[block_size_++] = bytes[index];
            if (block_size_ == block_.size()) {
                transform();
                bit_length_ += 512;
                block_size_ = 0;
            }
        }
    }

    std::array<unsigned char, 32> finish() {
        std::size_t index = block_size_;
        block_[index++] = 0x80;
        if (index > 56) {
            while (index < block_.size()) block_[index++] = 0;
            transform();
            index = 0;
        }
        while (index < 56) block_[index++] = 0;
        bit_length_ += static_cast<uint64_t>(block_size_) * 8U;
        for (int shift = 56; shift >= 0; shift -= 8) {
            block_[index++] =
                static_cast<unsigned char>((bit_length_ >> shift) & 0xffU);
        }
        transform();

        std::array<unsigned char, 32> result{};
        for (std::size_t word = 0; word < state_.size(); ++word) {
            result[word * 4] =
                static_cast<unsigned char>((state_[word] >> 24) & 0xffU);
            result[word * 4 + 1] =
                static_cast<unsigned char>((state_[word] >> 16) & 0xffU);
            result[word * 4 + 2] =
                static_cast<unsigned char>((state_[word] >> 8) & 0xffU);
            result[word * 4 + 3] =
                static_cast<unsigned char>(state_[word] & 0xffU);
        }
        return result;
    }

private:
    void transform() {
        std::array<uint32_t, 64> words{};
        for (std::size_t index = 0; index < 16; ++index) {
            const std::size_t offset = index * 4;
            words[index] =
                (static_cast<uint32_t>(block_[offset]) << 24) |
                (static_cast<uint32_t>(block_[offset + 1]) << 16) |
                (static_cast<uint32_t>(block_[offset + 2]) << 8) |
                static_cast<uint32_t>(block_[offset + 3]);
        }
        for (std::size_t index = 16; index < words.size(); ++index) {
            const uint32_t s0 =
                rotate_right(words[index - 15], 7) ^
                rotate_right(words[index - 15], 18) ^
                (words[index - 15] >> 3);
            const uint32_t s1 =
                rotate_right(words[index - 2], 17) ^
                rotate_right(words[index - 2], 19) ^
                (words[index - 2] >> 10);
            words[index] =
                words[index - 16] + s0 + words[index - 7] + s1;
        }

        uint32_t a = state_[0];
        uint32_t b = state_[1];
        uint32_t c = state_[2];
        uint32_t d = state_[3];
        uint32_t e = state_[4];
        uint32_t f = state_[5];
        uint32_t g = state_[6];
        uint32_t h = state_[7];
        for (std::size_t index = 0; index < words.size(); ++index) {
            const uint32_t choice = (e & f) ^ (~e & g);
            const uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
            const uint32_t sum0 =
                rotate_right(a, 2) ^ rotate_right(a, 13) ^ rotate_right(a, 22);
            const uint32_t sum1 =
                rotate_right(e, 6) ^ rotate_right(e, 11) ^ rotate_right(e, 25);
            const uint32_t temporary1 =
                h + sum1 + choice + kRoundConstants[index] + words[index];
            const uint32_t temporary2 = sum0 + majority;
            h = g;
            g = f;
            f = e;
            e = d + temporary1;
            d = c;
            c = b;
            b = a;
            a = temporary1 + temporary2;
        }
        state_[0] += a;
        state_[1] += b;
        state_[2] += c;
        state_[3] += d;
        state_[4] += e;
        state_[5] += f;
        state_[6] += g;
        state_[7] += h;
    }

    std::array<unsigned char, 64> block_{};
    std::size_t block_size_ = 0;
    uint64_t bit_length_ = 0;
    std::array<uint32_t, 8> state_{};
};
}  // namespace

bool file_sha256(const std::string &path,
                 std::string *digest,
                 std::string *error) {
    if (!digest) {
        if (error) *error = "sha256 output is null";
        return false;
    }
    std::unique_ptr<std::FILE, decltype(&std::fclose)> input(
        std::fopen(path.c_str(), "rb"),
        &std::fclose);
    if (!input) {
        if (error) *error = "open file for sha256 failed";
        return false;
    }
    Sha256 calculator;
    std::array<unsigned char, 64 * 1024> buffer{};
    while (true) {
        const std::size_t count =
            std::fread(buffer.data(), 1, buffer.size(), input.get());
        if (count > 0) calculator.update(buffer.data(), count);
        if (count < buffer.size()) {
            if (std::ferror(input.get())) {
                if (error) *error = "read file for sha256 failed";
                return false;
            }
            break;
        }
    }
    const auto bytes = calculator.finish();
    std::ostringstream value;
    value << std::hex << std::setfill('0');
    for (const auto byte : bytes) {
        value << std::setw(2) << static_cast<unsigned int>(byte);
    }
    *digest = value.str();
    return true;
}
