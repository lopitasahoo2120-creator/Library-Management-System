#ifndef LIBRARY_REPORT_MANAGER_H
#define LIBRARY_REPORT_MANAGER_H

#include <vector>
#include <string>
#include <ostream>

class Book;
class Member;
class Transaction;

// ---------------------------------------------------------------------------
// ReportManager - generates summary reports from live data.
// All numbers are computed from the in-memory collections, never fabricated.
// ---------------------------------------------------------------------------
class ReportManager {
public:
    struct Summary {
        size_t totalBooks       = 0;
        size_t totalCopies      = 0;
        size_t availableCopies  = 0;
        size_t issuedCopies     = 0;
        size_t totalMembers     = 0;
        size_t activeLoans      = 0;
        size_t overdueLoans     = 0;
        double  totalLateFees   = 0.0;
    };

    ReportManager(const std::vector<Book>&       books,
                  const std::vector<Member>&     members,
                  const std::vector<Transaction>& transactions);

    void printSummary(std::ostream& os) const;
    void printOverdue(std::ostream& os) const;
    void printRecentlyIssued(std::ostream& os, int count) const;
    void printBorrowingHistory(int memberId, std::ostream& os) const;

    const Summary& summary() const noexcept { return summary_; }

    // Recompute the cached counters from the live collections.  The vectors
    // are held by reference, so this picks up any change made to them.
    void refresh();

private:
    void computeSummary();

    const std::vector<Book>&       books_;
    const std::vector<Member>&     members_;
    const std::vector<Transaction>& transactions_;
    Summary summary_;
};

#endif // LIBRARY_REPORT_MANAGER_H