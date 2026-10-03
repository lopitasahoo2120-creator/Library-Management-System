// ---------------------------------------------------------------------------
// Transaction.cpp - implementation of the Transaction entity.
// ---------------------------------------------------------------------------
#include "Transaction.h"

#include <cstdint>
#include <iomanip>
#include <stdexcept>
#include <iostream>

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

} // namespace

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------
Transaction::Transaction(int transactionId,
                         int bookId,
                         int memberId,
                         std::string issueDate,
                         std::string dueDate,
                         std::string returnDate,
                         double lateFee,
                         Status status)
    : transactionId_(transactionId),
      bookId_(bookId),
      memberId_(memberId),
      issueDate_(std::move(issueDate)),
      dueDate_(std::move(dueDate)),
      returnDate_(std::move(returnDate)),
      lateFee_(lateFee),
      status_(status) {
    if (transactionId_ <= 0) {
        throw std::invalid_argument("Transaction ID must be a positive integer");
    }
    if (bookId_ <= 0) {
        throw std::invalid_argument("Book ID must be a positive integer");
    }
    if (memberId_ <= 0) {
        throw std::invalid_argument("Member ID must be a positive integer");
    }
    if (issueDate_.empty()) {
        throw std::invalid_argument("Issue date cannot be empty");
    }
    if (dueDate_.empty()) {
        throw std::invalid_argument("Due date cannot be empty");
    }
}

// ---------------------------------------------------------------------------
// Behaviour
// ---------------------------------------------------------------------------
void Transaction::display(std::ostream& os) const {
    os << "Transaction #" << transactionId_
       << " | Book ID: " << bookId_
       << " | Member ID: " << memberId_
       << " | Issued: " << issueDate_
       << " | Due: " << dueDate_
       << " | Returned: " << (returnDate_.empty() ? "-" : returnDate_)
       << " | Late fee: " << std::fixed << std::setprecision(2) << lateFee_
       << " | Status: " << statusToString(status_) << '\n';
}

// ---------------------------------------------------------------------------
// Serialization (binary)
// ---------------------------------------------------------------------------
std::size_t Transaction::serialize(std::ostream& os) const {
    os.write(reinterpret_cast<const char*>(&transactionId_), sizeof(transactionId_));
    os.write(reinterpret_cast<const char*>(&bookId_), sizeof(bookId_));
    os.write(reinterpret_cast<const char*>(&memberId_), sizeof(memberId_));
    writeString(os, issueDate_);
    writeString(os, dueDate_);
    writeString(os, returnDate_);
    os.write(reinterpret_cast<const char*>(&lateFee_), sizeof(lateFee_));

    const int statusValue = static_cast<int>(status_);
    os.write(reinterpret_cast<const char*>(&statusValue), sizeof(statusValue));

    if (!os) return 0;
    return static_cast<std::size_t>(
        3 * sizeof(int) +
        3 * sizeof(std::uint32_t) +
        issueDate_.size() + dueDate_.size() + returnDate_.size() +
        sizeof(lateFee_) + sizeof(statusValue));
}

bool Transaction::deserialize(std::istream& is) {
    try {
        is.read(reinterpret_cast<char*>(&transactionId_), sizeof(transactionId_));
        is.read(reinterpret_cast<char*>(&bookId_), sizeof(bookId_));
        is.read(reinterpret_cast<char*>(&memberId_), sizeof(memberId_));
        if (!readString(is, issueDate_)) return false;
        if (!readString(is, dueDate_)) return false;
        if (!readString(is, returnDate_)) return false;
        is.read(reinterpret_cast<char*>(&lateFee_), sizeof(lateFee_));
        if (!is) return false;

        int statusValue = 0;
        is.read(reinterpret_cast<char*>(&statusValue), sizeof(statusValue));
        if (!is) return false;

        if (statusValue >= static_cast<int>(Status::ISSUED) &&
            statusValue <= static_cast<int>(Status::OVERDUE)) {
            status_ = static_cast<Status>(statusValue);
        } else {
            status_ = Status::ISSUED;
        }
        return true;
    } catch (...) {
        return false;
    }
}

// ---------------------------------------------------------------------------
// Status helpers
// ---------------------------------------------------------------------------
std::string Transaction::statusToString(Status s) noexcept {
    switch (s) {
        case Status::ISSUED:   return "ISSUED";
        case Status::RETURNED: return "RETURNED";
        case Status::OVERDUE:  return "OVERDUE";
        default:              return "ISSUED";
    }
}

Transaction::Status Transaction::stringToStatus(const std::string& s) noexcept {
    if (s == "ISSUED")   return Status::ISSUED;
    if (s == "RETURNED") return Status::RETURNED;
    if (s == "OVERDUE")  return Status::OVERDUE;
    return Status::ISSUED;
}
