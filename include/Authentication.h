#ifndef LIBRARY_AUTHENTICATION_H
#define LIBRARY_AUTHENTICATION_H

#include <string>

// ---------------------------------------------------------------------------
// Authentication - validates admin credentials stored in data/admin.dat.
//
// Password storage: a simple salted SHA-256 hash is used.  This is clearly
// documented as educational - it is NOT production-grade authentication.
// If OpenSSL is unavailable the build falls back to a trivial reversible
// encoding so the project still compiles on a minimal system.
// ---------------------------------------------------------------------------
class Authentication {
public:
    Authentication();

    // Returns true when credentials match the stored admin record.
    bool login(const std::string& username, const std::string& password);

    // Change the stored admin password (requires current password).
    bool changePassword(const std::string& user,
                        const std::string& oldPass,
                        const std::string& newPass);

    const std::string& currentAdmin() const noexcept { return adminUsername_; }

    // ---- helpers exposed for testing / documentation -------------------
    static std::string hashPassword(const std::string& plain);
    static std::string generateSalt();

private:
    bool loadAdmin();          // read data/admin.dat
    bool saveAdmin();          // write data/admin.dat atomically

    std::string adminUsername_;
    std::string storedHash_;   // salt:hash hex string
};

#endif // LIBRARY_AUTHENTICATION_H