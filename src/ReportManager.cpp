// ---------------------------------------------------------------------------
// ReportManager.cpp - implementation of the reporting subsystem.
//
// Every number printed here is derived from the live in-memory collections.
// Nothing is cached between runs and nothing is hard-coded, so a report always
// matches the current state of the database files.
// ---------------------------------------------------------------------------

#include "ReportManager.h"

#include "Book.h"
#include "Member.h"
#include "Transaction.h"
#include "Config.h"
#include "Utils.h"

#include <algorithm>
#include <iomanip>
#include <iostream>

namespace {

// Horizontal rule used to keep console output readable.
const char* const RULE =
    "---------------------------------------------------------------\n";

// Only loans that are still open can be overdue.
bool isOpenLoan(const Transaction& t) {
    return t.getStatus() == Transaction::Status::ISSUED ||
           t.getStatus() == Transaction::Status::OVERDUE;
}

} // namespace

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------
ReportManager::ReportManager(const std::vector<Book>&        books,
                             const std::vector<Member>&      members,
                             const std::vector<Transaction>& transactions)
    : books_(books),
      members_(members),
      transactions_(transactions) {
    computeSummary();
}

// ---------------------------------------------------------------------------
// refresh()
// ---------------------------------------------------------------------------
void ReportManager::refresh() {
    computeSummary();
}

// ---------------------------------------------------------------------------
// computeSummary()
// ---------------------------------------------------------------------------
void ReportManager::computeSummary() {
    summary_ = Summary{};

    summary_.totalBooks = books_.size();
    for (const Book& b : books_) {
        const int total     = b.getTotalCopies();
        const int available = b.getAvailableCopies();
        summary_.totalCopies     += static_cast<std::size_t>(total < 0 ? 0 : total);
        summary_.availableCopies += static_cast<std::size_t>(available < 0 ? 0 : available);
    }
    summary_.issuedCopies =
        (summary_.totalCopies > summary_.availableCopies)
            ? summary_.totalCopies - summary_.availableCopies
            : 0;

    summary_.totalMembers = members_.size();

    for (const Transaction& t : transactions_) {
        if (isOpenLoan(t)) {
            ++summary_.activeLoans;
            if (t.getLateFee() > 0.0) summary_.totalLateFees += t.getLateFee();
            if (Utils::overdueDays(t.getDueDate()) > 0) ++summary_.overdueLoans;
        }
    }
}

// ---------------------------------------------------------------------------
// printSummary()
// ---------------------------------------------------------------------------
void ReportManager::printSummary(std::ostream& os) const {
    os << "\n" << RULE
       << "  LIBRARY SUMMARY REPORT\n"
       << RULE
       << "  Titles in catalogue      : " << summary_.totalBooks        << '\n'
       << "  Total copies             : " << summary_.totalCopies       << '\n'
       << "  Available copies         : " << summary_.availableCopies   << '\n'
       << "  Issued copies            : " << summary_.issuedCopies      << '\n'
       << "  Registered members       : " << summary_.totalMembers      << '\n'
       << "  Active loans             : " << summary_.activeLoans       << '\n'
       << "  Overdue loans            : " << summary_.overdueLoans      << '\n'
       << "  Outstanding late fees    : " << std::fixed << std::setprecision(2)
       << summary_.totalLateFees << '\n'
       << RULE;
}

// ---------------------------------------------------------------------------
// printOverdue()
// ---------------------------------------------------------------------------
void ReportManager::printOverdue(std::ostream& os) const {
    os << "\n" << RULE
       << "  OVERDUE BOOKS (open loans past their due date)\n"
       << RULE;

    int shown = 0;
    const std::string today = Utils::today();

    for (const Transaction& t : transactions_) {
        if (!isOpenLoan(t)) continue;

        const int late = Utils::daysBetween(today, t.getDueDate());
        if (late <= 0) continue;

        os << "  Tx #" << t.getTransactionId()
           << " | Book ID: " << t.getBookId()
           << " | Member ID: " << t.getMemberId()
           << " | Due: " << t.getDueDate()
           << " | " << late << " day(s) late"
           << " | Fee: " << std::fixed << std::setprecision(2)
           << t.getLateFee() << '\n';
        ++shown;
    }

    if (shown == 0) {
        os << "  (no overdue loans)\n";
    }
    os << RULE;
}

// ---------------------------------------------------------------------------
// printRecentlyIssued()
// ---------------------------------------------------------------------------
void ReportManager::printRecentlyIssued(std::ostream& os, int count) const {
    if (count <= 0) count = Config::RECENT_ISSUED_COUNT;

    os << "\n" << RULE
       << "  RECENTLY ISSUED BOOKS (last " << count << ")\n"
       << RULE;

    std::vector<const Transaction*> loans;
    loans.reserve(transactions_.size());
    for (const Transaction& t : transactions_) {
        loans.push_back(&t);
    }

    // Newest issue date first; ties broken by the higher transaction id, which
    // is monotonically increasing.
    std::sort(loans.begin(), loans.end(),
              [](const Transaction* a, const Transaction* b) {
                  if (a->getIssueDate() != b->getIssueDate()) {
                      return a->getIssueDate() > b->getIssueDate();
                  }
                  return a->getTransactionId() > b->getTransactionId();
              });

    const std::size_t limit =
        std::min<std::size_t>(static_cast<std::size_t>(count), loans.size());

    for (std::size_t i = 0; i < limit; ++i) {
        const Transaction* t = loans[i];
        os << "  Tx #" << t->getTransactionId()
           << " | Book ID: " << t->getBookId()
           << " | Member ID: " << t->getMemberId()
           << " | Issued: " << t->getIssueDate()
           << " | Due: " << t->getDueDate()
           << " | " << Transaction::statusToString(t->getStatus()) << '\n';
    }

    if (limit == 0) {
        os << "  (no transactions recorded)\n";
    }
    os << RULE;
}

// ---------------------------------------------------------------------------
// printBorrowingHistory()
// ---------------------------------------------------------------------------
void ReportManager::printBorrowingHistory(int memberId, std::ostream& os) const {
    os << "\n" << RULE
       << "  BORROWING HISTORY - MEMBER " << memberId << '\n'
       << RULE;

    int shown = 0;
    for (const Transaction& t : transactions_) {
        if (t.getMemberId() != memberId) continue;

        os << "  Tx #" << t.getTransactionId()
           << " | Book ID: " << t.getBookId()
           << " | Issued: " << t.getIssueDate()
           << " | Due: " << t.getDueDate()
           << " | Returned: "
           << (t.getReturnDate().empty() ? "-" : t.getReturnDate())
           << " | Fee: " << std::fixed << std::setprecision(2) << t.getLateFee()
           << " | " << Transaction::statusToString(t.getStatus()) << '\n';
        ++shown;
    }

    if (shown == 0) {
        os << "  (this member has no borrowing history)\n";
    }
    os << RULE;
}