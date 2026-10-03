// ---------------------------------------------------------------------------
// Book.cpp - implementation of the Book entity.
// ---------------------------------------------------------------------------
#include "Book.h"

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
Book::Book(int id,
           std::string title,
           std::string author,
           std::string category,
           std::string isbn,
           std::string publication,
           int totalCopies,
           int availableCopies)
    : id_(id),
      title_(std::move(title)),
      author_(std::move(author)),
      category_(std::move(category)),
      isbn_(std::move(isbn)),
      publication_(std::move(publication)),
      totalCopies_(totalCopies),
      availableCopies_(availableCopies) {
    if (title_.empty()) {
        throw std::invalid_argument("Book title cannot be empty");
    }
    if (totalCopies_ < 0) {
        throw std::invalid_argument("Book total copies cannot be negative");
    }
    if (availableCopies_ < 0) {
        throw std::invalid_argument("Book available copies cannot be negative");
    }
    if (availableCopies_ > totalCopies_) {
        throw std::invalid_argument(
            "Book available copies cannot exceed total copies");
    }
}

// ---------------------------------------------------------------------------
// Mutators
// ---------------------------------------------------------------------------
void Book::setTitle(const std::string& title) {
    if (title.empty()) {
        throw std::invalid_argument("Book title cannot be empty");
    }
    title_ = title;
}

void Book::setAuthor(const std::string& author)   { author_ = author; }
void Book::setCategory(const std::string& category){ category_ = category; }
void Book::setIsbn(const std::string& isbn)        { isbn_ = isbn; }
void Book::setPublication(const std::string& pub)  { publication_ = pub; }

void Book::setTotalCopies(int total) {
    if (total < 0) {
        throw std::invalid_argument("Book total copies cannot be negative");
    }
    totalCopies_ = total;
    if (availableCopies_ > totalCopies_) {
        availableCopies_ = totalCopies_;
    }
}

void Book::setAvailableCopies(int available) {
    if (available < 0) {
        throw std::invalid_argument("Book available copies cannot be negative");
    }
    if (available > totalCopies_) {
        throw std::invalid_argument(
            "Book available copies cannot exceed total copies");
    }
    availableCopies_ = available;
}

// ---------------------------------------------------------------------------
// Behaviour
// ---------------------------------------------------------------------------
void Book::display(std::ostream& os) const {
    os << "Book ID      : " << id_ << '\n'
       << "  Title       : " << title_ << '\n'
       << "  Author      : " << author_ << '\n'
       << "  Category    : " << category_ << '\n'
       << "  ISBN        : " << isbn_ << '\n'
       << "  Publication : " << publication_ << '\n'
       << "  Copies      : " << availableCopies_ << " available / "
       << totalCopies_ << " total\n";
}

bool Book::isValid() const noexcept {
    return id_ > 0 &&
           !title_.empty() &&
           totalCopies_ >= 0 &&
           availableCopies_ >= 0 &&
           availableCopies_ <= totalCopies_;
}

// ---------------------------------------------------------------------------
// Serialization - order: id, title, author, category, isbn, publication,
// totalCopies, availableCopies.
// ---------------------------------------------------------------------------
std::size_t Book::serialize(std::ostream& os) const {
    const std::streampos start = os.tellp();
    if (start == std::streampos(-1)) {
        return 0;
    }

    writeInt(os, id_);
    writeString(os, title_);
    writeString(os, author_);
    writeString(os, category_);
    writeString(os, isbn_);
    writeString(os, publication_);
    writeInt(os, totalCopies_);
    writeInt(os, availableCopies_);

    if (!os) {
        return 0;
    }

    const std::streampos end = os.tellp();
    if (end == std::streampos(-1)) {
        return 0;
    }
    return static_cast<std::size_t>(end - start);
}

bool Book::deserialize(std::istream& is) {
    try {
        int id = 0;
        std::string title, author, category, isbn, publication;
        int totalCopies = 0;
        int availableCopies = 0;

        if (!readInt(is, id))              return false;
        if (!readString(is, title))         return false;
        if (!readString(is, author))        return false;
        if (!readString(is, category))      return false;
        if (!readString(is, isbn))          return false;
        if (!readString(is, publication))   return false;
        if (!readInt(is, totalCopies))      return false;
        if (!readInt(is, availableCopies))  return false;

        if (title.empty())  return false;
        if (totalCopies < 0) return false;
        if (availableCopies < 0 || availableCopies > totalCopies) return false;

        id_             = id;
        title_          = std::move(title);
        author_         = std::move(author);
        category_       = std::move(category);
        isbn_           = std::move(isbn);
        publication_    = std::move(publication);
        totalCopies_    = totalCopies;
        availableCopies_= availableCopies;
        return true;
    } catch (...) {
        return false;
    }
}
