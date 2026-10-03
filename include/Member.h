#ifndef LIBRARY_MEMBER_H
#define LIBRARY_MEMBER_H

#include <string>
#include <iostream>

// ---------------------------------------------------------------------------
// Member - represents a library member (student / staff / external).
// ---------------------------------------------------------------------------
class Member {
public:
    Member() = default;

    explicit Member(int memberId,
                    std::string name,
                    std::string email,
                    std::string phone,
                    std::string department,
                    int issuedBooks = 0);

    // ---- accessors ------------------------------------------------------
    int          getMemberId()     const noexcept { return memberId_; }
    const std::string& getName()   const noexcept { return name_; }
    const std::string& getEmail()  const noexcept { return email_; }
    const std::string& getPhone()  const noexcept { return phone_; }
    const std::string& getDepartment() const noexcept { return department_; }
    int          getIssuedBooks()  const noexcept { return issuedBooks_; }

    // ---- mutators -------------------------------------------------------
    void setName(const std::string& name);
    void setEmail(const std::string& email);
    void setPhone(const std::string& phone);
    void setDepartment(const std::string& department);
    void setIssuedBooks(int count) noexcept { issuedBooks_ = count; }
    void incrementIssued()  noexcept { ++issuedBooks_; }
    void decrementIssued()  noexcept { if (issuedBooks_ > 0) --issuedBooks_; }

    // ---- behaviour ------------------------------------------------------
    void display(std::ostream& os = std::cout) const;
    bool isValid() const noexcept;
    bool canBorrow() const noexcept;   // respects MAX_BOOKS_PER_MEMBER

    // ---- serialization (binary) ----------------------------------------
    std::size_t serialize(std::ostream& os) const;
    bool        deserialize(std::istream& is);

private:
    int         memberId_    = 0;
    std::string name_;
    std::string email_;
    std::string phone_;
    std::string department_;
    int         issuedBooks_ = 0;
};

#endif // LIBRARY_MEMBER_H