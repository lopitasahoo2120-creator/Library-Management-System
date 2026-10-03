// ---------------------------------------------------------------------------
// Authentication.cpp - admin credential handling for the Library Management
// System.
//
// SECURITY NOTE (educational project):
//   Passwords are stored as "salt:hash" where salt is 16 hex characters and
//   hash is the 64-character hex SHA-256 digest of (salt + plaintext).  The
//   SHA-256 implementation below is written from scratch so the project has no
//   external crypto dependency.
//
//   This is deliberately simple and is NOT production-grade: SHA-256 is fast,
//   so a stolen admin.dat can be brute forced.  Real systems must use a slow,
//   salted KDF such as bcrypt, scrypt or Argon2id.  The value here is that
//   plaintext passwords are never written to disk.
// ---------------------------------------------------------------------------
#include "Authentication.h"

#include "FileManager.h"
#include "Utils.h"
#include "Config.h"

#include <fstream>
#include <iostream>
#include <sstream>
#include <random>
#include <cstring>
#include <stdexcept>
#include <cstdint>
#include <cctype>

#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#include <sys/stat.h>
#endif

namespace {

// ===========================================================================
// SHA-256 - compact, dependency-free implementation (FIPS 180-4).
// ===========================================================================
namespace sha256 {

inline std::uint32_t rotr(std::uint32_t x, std::uint32_t n) {
    return (x >> n) | (x << (32 - n));
}

const std::uint32_t k[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
    0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
    0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
    0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
    0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
};

void transform(std::uint32_t state[8], const unsigned char block[64]) {
    std::uint32_t w[64];
    for (int i = 0; i < 16; ++i) {
        w[i] = (static_cast<std::uint32_t>(block[i * 4 + 0]) << 24) |
               (static_cast<std::uint32_t>(block[i * 4 + 1]) << 16) |
               (static_cast<std::uint32_t>(block[i * 4 + 2]) << 8)  |
               (static_cast<std::uint32_t>(block[i * 4 + 3]));
    }
    for (int i = 16; i < 64; ++i) {
        const std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    std::uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
    std::uint32_t e = state[4], f = state[5], g = state[6], h = state[7];

    for (int i = 0; i < 64; ++i) {
        const std::uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        const std::uint32_t ch = (e & f) ^ ((~e) & g);
        const std::uint32_t temp1 = h + S1 + ch + k[i] + w[i];
        const std::uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        const std::uint32_t temp2 = S0 + maj;

        h = g; g = f; f = e; e = d + temp1;
        d = c; c = b; b = a; a = temp1 + temp2;
    }

    state[0] += a; state[1] += b; state[2] += c; state[3] += d;
    state[4] += e; state[5] += f; state[6] += g; state[7] += h;
}

} // namespace sha256

std::string sha256Hex(const std::string& input) {
    std::uint32_t state[8] = {
        0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
        0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u
    };

    std::string msg = input;

    // Padding: 0x80, then zeros, then the 64-bit big-endian bit length.
    const std::uint64_t bitLength = static_cast<std::uint64_t>(input.size()) * 8u;
    msg.push_back(static_cast<char>(0x80));
    while (msg.size() % 64 != 56) {
        msg.push_back('\0');
    }
    for (int i = 7; i >= 0; --i) {
        msg.push_back(static_cast<char>((bitLength >> (i * 8)) & 0xFFu));
    }

    for (std::size_t off = 0; off < msg.size(); off += 64) {
        sha256::transform(state,
                          reinterpret_cast<const unsigned char*>(msg.data() + off));
    }

    static const char* hex = "0123456789abcdef";
    std::string out;
    out.reserve(64);
    for (int i = 0; i < 8; ++i) {
        for (int b = 3; b >= 0; --b) {
            const unsigned char byte =
                static_cast<unsigned char>((state[i] >> (b * 8)) & 0xFFu);
            out.push_back(hex[byte >> 4]);
            out.push_back(hex[byte & 0x0Fu]);
        }
    }
    return out;
}

// Constant-time-ish string comparison to limit timing side channels.
bool secureEquals(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    unsigned char diff = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        diff = static_cast<unsigned char>(diff | (a[i] ^ b[i]));
    }
    return diff == 0;
}

// Restrict a credentials file to owner read/write (0600 on POSIX).
void restrictPermissions(const std::string& path) {
#if defined(_WIN32)
    (void)path; // Windows ACLs are out of scope for this course project.
#else
    ::chmod(path.c_str(), S_IRUSR | S_IWUSR);
#endif
}

bool fileExists(const std::string& path) {
    std::ifstream probe(path);
    return probe.good();
}

} // namespace

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------
Authentication::Authentication() {
    loadAdmin();
}

// ---------------------------------------------------------------------------
// Salt generation
// ---------------------------------------------------------------------------
std::string Authentication::generateSalt() {
    static const char* hex = "0123456789abcdef";
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<int> dist(0, 15);

    std::string salt;
    salt.reserve(16);
    for (int i = 0; i < 16; ++i) {
        salt.push_back(hex[dist(gen)]);
    }
    return salt;
}

// ---------------------------------------------------------------------------
// Hashing
// ---------------------------------------------------------------------------
std::string Authentication::hashPassword(const std::string& plain) {
    const std::string salt = generateSalt();
    const std::string digest = sha256Hex(salt + plain);
    return salt + ":" + digest;
}

// ---------------------------------------------------------------------------
// Login
// ---------------------------------------------------------------------------
bool Authentication::login(const std::string& username,
                            const std::string& password) {
    if (!loadAdmin()) return false;
    if (adminUsername_.empty() || storedHash_.empty()) return false;
    if (username != adminUsername_) return false;

    const std::size_t sep = storedHash_.find(':');
    if (sep == std::string::npos) return false;

    const std::string salt = storedHash_.substr(0, sep);
    const std::string expected = storedHash_.substr(sep + 1);
    const std::string actual = sha256Hex(salt + password);

    // Never print the password (or its hash) to stdout/stderr/logs.
    return secureEquals(actual, expected);
}

// ---------------------------------------------------------------------------
// Password change
// ---------------------------------------------------------------------------
bool Authentication::changePassword(const std::string& user,
                                    const std::string& oldPass,
                                    const std::string& newPass) {
    if (user != adminUsername_) return false;
    if (!login(user, oldPass))   return false;
    if (newPass.empty())         return false;

    storedHash_ = hashPassword(newPass);
    return saveAdmin();
}

// ---------------------------------------------------------------------------
// Persistence
// ---------------------------------------------------------------------------
bool Authentication::loadAdmin() {
    const std::string path = Config::ADMIN_FILE;

    if (!fileExists(path)) {
        // First run: provision the default educational account.
        adminUsername_ = "admin";
        storedHash_    = hashPassword("admin123");
        return saveAdmin();
    }

    std::ifstream in(path);
    if (!in.is_open()) return false;

    std::string line;
    std::string username;
    std::string hash;
    while (std::getline(in, line)) {
        line = Utils::trim(line);
        if (line.rfind("username=", 0) == 0) {
            username = Utils::trim(line.substr(9));
        } else if (line.rfind("hash=", 0) == 0) {
            hash = Utils::trim(line.substr(5));
        }
    }
    in.close();

    if (username.empty() || hash.empty()) return false;
    adminUsername_ = username;
    storedHash_    = hash;
    return true;
}

bool Authentication::saveAdmin() {
    std::ostringstream buffer;
    buffer << "username=" << adminUsername_ << '\n'
           << "hash="     << storedHash_    << '\n';

    const std::string path = Config::ADMIN_FILE;
    const std::string temp = path + ".tmp";

    if (!FileManager::atomicWrite(temp, buffer.str())) {
        return false;
    }
    if (!FileManager::atomicWrite(path, buffer.str())) {
        std::remove(temp.c_str());
        return false;
    }
    std::remove(temp.c_str());

    restrictPermissions(path);
    return true;
}
