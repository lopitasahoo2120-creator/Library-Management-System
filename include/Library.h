#ifndef LIBRARY_LIBRARY_H
#define LIBRARY_LIBRARY_H

#include <vector>
#include <memory>
#include <string>

// The entity types are needed in full here: Library stores them by value in
// its collections and hands them out through the search helpers.
#include "Book.h"
#include "Config.h"
#include "DriverClient.h"
#include "Member.h"
#include "Transaction.h"

class Authentication;
class FileManager;
class Logger;
class ReportManager;
class ProcessManager;

// ---------------------------------------------------------------------------
// Library - the central business-logic coordinator.
//
// Layer 2 of the architecture: it owns the collections and orchestrates
// issue/return/search/report operations.  It delegates persistence to
// FileManager and observation to Logger.
// ---------------------------------------------------------------------------
class Library {
public:
    Library();
    ~Library();

    Library(const Library&)            = delete;
    Library& operator=(const Library&) = delete;

    // ---- lifecycle ------------------------------------------------------
    bool initialise();   // load data files, prepare logging/IPC
    void shutdown();     // graceful stop of all subsystems

    // ---- authentication ------------------------------------------------
    bool authenticate(const std::string& username,
                      const std::string& password);
    bool changePassword(const std::string& user,
                        const std::string& oldPass,
                        const std::string& newPass);
    bool isAdminActive() const noexcept { return authenticated_; }

    // ---- book management ------------------------------------------------
    // Why an add was refused, so the UI can explain it instead of printing a
    // bare "failed".  The enum is additive: addBook() still returns bool.
    enum class AddResult { Added, InvalidRecord, DuplicateId };

    bool addBook(const Book& book);
    AddResult addBookChecked(const Book& book);

    bool removeBook(int id);
    bool updateBook(const Book& book);
    const Book* findBook(int id) const;
    std::vector<Book> searchBooks(const std::string& query,
                                  const std::string& field) const;
    const std::vector<Book>& allBooks() const noexcept { return books_; }

    // ---- member management ----------------------------------------------
    bool addMember(const Member& member);
    bool removeMember(int memberId);
    bool updateMember(const Member& member);
    const Member* findMember(int memberId) const;
    std::vector<Member> searchMembers(const std::string& query) const;
    const std::vector<Member>& allMembers() const noexcept { return members_; }

    // ---- transactions ---------------------------------------------------
    bool issueBook(int bookId, int memberId);
    bool returnBook(int transactionId);
    const Transaction* findTransaction(int transactionId) const;
    std::vector<Transaction> activeTransactions() const;
    std::vector<Transaction> memberHistory(int memberId) const;

    // ---- persistence ----------------------------------------------------
    bool saveAll();
    bool loadAll();

    // ---- reports --------------------------------------------------------
    void printSummary();
    void printOverdue();
    void printRecentlyIssued();
    void printMemberHistory(int memberId);

    // ---- access to subsystems (for testing/diagnostics) -----------------
    Logger&             logger()       noexcept { return *logger_; }
    const Logger&       logger() const noexcept { return *logger_; }
    Authentication&     auth()         noexcept { return *auth_; }
    FileManager&        fileManager()  noexcept { return *fileManager_; }
    const ReportManager& reportManager() const noexcept { return *report_; }
    DriverClient&       driver()       noexcept { return *driver_; }

    // ---- driver integration --------------------------------------------
    // Broadcasts one event to every subscriber: the kernel driver and (when
    // attached) the background monitor process.  Both are optional, so a
    // failure here never breaks the operation that triggered it.
    void notifyDriver(const std::string& event);

    // Let main() hand over the monitor process so business events reach the
    // child as well.  Passing nullptr disables that channel.
    void attachMonitor(ProcessManager* monitor) noexcept { monitor_ = monitor; }

    // True when /dev/library_driver is present and usable.
    bool driverAvailable() const noexcept { return driver_ && driver_->isAvailable(); }

private:
    void ensureDataDirectory();

    // Recomputes Transaction::Status::OVERDUE for open loans whose due date
    // has passed, and refreshes the late fee.  Called before reporting.
    void refreshOverdueStatus();

    std::unique_ptr<Authentication> auth_;
    std::unique_ptr<Logger>         logger_;
    std::unique_ptr<FileManager>    fileManager_;
    std::unique_ptr<ReportManager>  report_;
    std::unique_ptr<DriverClient>   driver_;
    ProcessManager*                 monitor_ = nullptr;

    std::vector<Book>        books_;
    std::vector<Member>      members_;
    std::vector<Transaction> transactions_;

    int  nextTransactionId_ = Config::TRANSACTION_ID_START;
    bool authenticated_    = false;
};

#endif // LIBRARY_LIBRARY_H