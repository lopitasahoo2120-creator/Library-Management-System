#ifndef LIBRARY_TRANSACTION_H
#define LIBRARY_TRANSACTION_H

#include <string>
#include <iostream>

// ---------------------------------------------------------------------------
// Transaction - one issue/return event.
//
// Statuses:  ISSUED   - book is currently on loan
//            RETURNED - book has been returned
//            OVERDUE   - book is past its due date (informational)
// ---------------------------------------------------------------------------
class Transaction {
public:
    enum class Status { ISSUED, RETURNED, OVERDUE };

    Transaction() = default;

    explicit Transaction(int transactionId,
                         int bookId,
                         int memberId,
                         std::string issueDate,
                         std::string dueDate,
                         std::string returnDate = std::string(),
                         double lateFee = 0.0,
                         Status status = Status::ISSUED);

    // ---- accessors ------------------------------------------------------
    int          getTransactionId() const noexcept { return transactionId_; }
    int          getBookId()        const noexcept { return bookId_; }
    int          getMemberId()      const noexcept { return memberId_; }
    const std::string& getIssueDate() const noexcept { return issueDate_; }
    const std::string& getDueDate()   const noexcept { return dueDate_; }
    const std::string& getReturnDate()const noexcept { return returnDate_; }
    double        getLateFee()     const noexcept { return lateFee_; }
    Status        getStatus()      const noexcept { return status_; }

    // ---- mutators -------------------------------------------------------
    void setReturnDate(const std::string& date) noexcept { returnDate_ = date; }
    void setDueDate(const std::string& date)   noexcept { dueDate_ = date; }
    void setLateFee(double fee)      noexcept { lateFee_ = fee; }
    void setStatus(Status s)         noexcept { status_ = s; }

    // ---- behaviour ------------------------------------------------------
    void display(std::ostream& os = std::cout) const;

    // An open loan: the book has been issued and not yet given back.
    // OVERDUE is also open - it only means the due date has passed.
    bool isActive() const noexcept {
        return status_ == Status::ISSUED || status_ == Status::OVERDUE;
    }

    // ---- serialization (binary) ----------------------------------------
    std::size_t serialize(std::ostream& os) const;
    bool        deserialize(std::istream& is);

    static std::string statusToString(Status s) noexcept;
    static Status      stringToStatus(const std::string& s) noexcept;

private:
    int         transactionId_ = 0;
    int         bookId_        = 0;
    int         memberId_     = 0;
    std::string issueDate_;
    std::string dueDate_;
    std::string returnDate_;
    double      lateFee_       = 0.0;
    Status      status_        = Status::ISSUED;
};

#endif // LIBRARY_TRANSACTION_H