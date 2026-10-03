// ---------------------------------------------------------------------------
// Member.cpp - implementation of the Member entity.
// ---------------------------------------------------------------------------
#include "Member.h"

#include "Config.h"
#include "Utils.h"

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

// Length-prefixed string write: uint32_t size followed by raw bytes.
void writeString(std::ostream& os, const std::string& value) {
    const std::uint32_t size = static_cast<std::uint32_t>(value.size());
    os.write(reinterpret_cast<const char*>(&size), sizeof(size));
    if (size > 0) {
        os.write(value.data(), static_cast<std::streamsize>(size));
    }
}

bool readString(std::istream& is, std::string& out) {
    std::uint32_t size = 0;
    is.read(reinterpret_cast<char*>(&size), sizeof(size));
    if (!is) return false;
    if (size == 0) {
        out.clear();
        return true;
    }
    out.assign(size, '\0');
    is.read(&out[0], static_cast<std::streamsize>(size));
    return static_cast<bool>(is);
}

template <typename T>
void writeInt(std::ostream& os, const T& value) {
    os.write(reinterpret_cast<const char*>(&value), sizeof(T));
}

template <typename T>
bool readInt(std::istream& is, T& value) {
    is.read(reinterpret_cast<char*>(&value), sizeof(T));
    return static_cast<bool>(is);
}

} // namespace

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------
Member::Member(int memberId,
               std::string name,
               std::string email,
               std::string phone,
               std::string department,
               int issuedBooks)
    : memberId_(memberId),
      name_(std::move(name)),
      email_(std::move(email)),
      phone_(std::move(phone)),
      department_(std::move(department)),
      issuedBooks_(issuedBooks) {
    if (memberId_ <= 0) {
        throw std::invalid_argument("Member ID must be a positive integer");
    }
    if (name_.empty()) {
        throw std::invalid_argument("Member name cannot be empty");
    }
}

// ---------------------------------------------------------------------------
// Mutators
// ---------------------------------------------------------------------------
void Member::setName(const std::string& name) {
    if (name.empty()) {
        throw std::invalid_argument("Member name cannot be empty");
    }
    name_ = name;
}

void Member::setEmail(const std::string& email) {
    if (!Utils::isValidEmail(email)) {
        throw std::invalid_argument("Invalid member email: " + email);
    }
    email_ = email;
}

void Member::setPhone(const std::string& phone) {
    if (!Utils::isValidPhone(phone)) {
        throw std::invalid_argument("Invalid member phone: " + phone);
    }
    phone_ = phone;
}

void Member::setDepartment(const std::string& department) {
    department_ = department;   // free-form
}

// ---------------------------------------------------------------------------
// Behaviour
// ---------------------------------------------------------------------------
void Member::display(std::ostream& os) const {
    os << "Member ID    : " << memberId_ << '\n'
       << "  Name        : " << name_ << '\n'
       << "  Email       : " << email_ << '\n'
       << "  Phone       : " << phone_ << '\n'
       << "  Department  : " << department_ << '\n'
       << "  Books issued: " << issuedBooks_ << " / "
       << Config::MAX_BOOKS_PER_MEMBER << '\n';
}

bool Member::isValid() const noexcept {
    return memberId_ > 0 && !name_.empty();
}

bool Member::canBorrow() const noexcept {
    return issuedBooks_ < Config::MAX_BOOKS_PER_MEMBER;
}

// ---------------------------------------------------------------------------
// Serialization - order: memberId, name, email, phone, department, issuedBooks.
// ---------------------------------------------------------------------------
std::size_t Member::serialize(std::ostream& os) const {
    const std::streampos start = os.tellp();
    if (start == std::streampos(-1)) {
        return 0;
    }

    writeInt(os, memberId_);
    writeString(os, name_);
    writeString(os, email_);
    writeString(os, phone_);
    writeString(os, department_);
    writeInt(os, issuedBooks_);

    if (!os) {
        return 0;
    }

    const std::streampos end = os.tellp();
    if (end == std::streampos(-1)) {
        return 0;
    }
    return static_cast<std::size_t>(end - start);
}

bool Member::deserialize(std::istream& is) {
    try {
        int memberId = 0;
        std::string name, email, phone, department;
        int issuedBooks = 0;

        if (!readInt(is, memberId))     return false;
        if (!readString(is, name))       return false;
        if (!readString(is, email))      return false;
        if (!readString(is, phone))      return false;
        if (!readString(is, department)) return false;
        if (!readInt(is, issuedBooks))   return false;

        if (memberId <= 0)  return false;
        if (name.empty())   return false;
        if (issuedBooks < 0) return false;

        memberId_    = memberId;
        name_        = std::move(name);
        email_       = std::move(email);
        phone_       = std::move(phone);
        department_  = std::move(department);
        issuedBooks_ = issuedBooks;
        return true;
    } catch (...) {
        return false;
    }
}
