// ---------------------------------------------------------------------------
// Library.cpp - the central business-logic coordinator.
//
// Responsibilities
//   * own the in-memory collections (books / members / transactions)
//   * enforce the library policy (loan period, borrow limit, late fee)
//   * delegate persistence to FileManager
//   * record everything that happens in the Logger
//   * mirror significant events to the kernel driver and the monitor process
// ---------------------------------------------------------------------------

#include "Library.h"

#include "Authentication.h"
#include "FileManager.h"
#include "Logger.h"
#include "ReportManager.h"
#include "system/process_manager.h"

#include "Utils.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <iostream>
#include <sstream>
#include <sys/stat.h>
#include <sys/types.h>
#include <system_error>

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------
Library::Library()
    : auth_(std::make_unique<Authentication>()),
      logger_(std::make_unique<Logger>()),
      fileManager_(std::make_unique<FileManager>()),
      driver_(std::make_unique<DriverClient>()) {
    // ReportManager holds references to the collections, so it can only be
    // built once the vectors exist (they are members, hence already alive).
    report_ = std::make_unique<ReportManager>(books_, members_, transactions_);
}

Library::~Library() {
    try {
        shutdown();
    } catch (...) {
        // A destructor must never throw.
    }
}

// ---------------------------------------------------------------------------
// ensureDataDirectory()
// ---------------------------------------------------------------------------
void Library::ensureDataDirectory() {
    // FileManager::open() already creates missing parents; this exists so the
    // directory is present even before the first write, which makes the
    // "data persists" behaviour obvious to a viva examiner running ls.
    const std::string dir = Config::DATA_DIR;
    struct stat st {};
    if (::stat(dir.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) return;

    if (::mkdir(dir.c_str(), 0775) != 0 && errno != EEXIST) {
        logger_->log(Logger::Event::ERROR,
                     "cannot create data directory '" + dir + "': " +
                         std::string(std::strerror(errno)));
    }
}

// ---------------------------------------------------------------------------
// initialise()
// ---------------------------------------------------------------------------
bool Library::initialise() {
    ensureDataDirectory();

    logger_->log(Logger::Event::SYSTEM_START,
                 "Library subsystem initialising");

    const bool loaded = loadAll();

    if (driver_ && driver_->isAvailable()) {
        logger_->log(Logger::Event::DRIVER_EVENT,
                     "character device " + driver_->devicePath() +
                         " is available; kernel event reporting enabled");
    } else {
        logger_->log(Logger::Event::DRIVER_EVENT,
                     "character device " + std::string(Config::DRIVER_DEVICE) +
                         " unavailable; continuing without kernel event reporting");
    }

    logger_->log(Logger::Event::SYSTEM_START,
                 "Library ready: " + std::to_string(books_.size()) +
                     " book(s), " + std::to_string(members_.size()) +
                     " member(s), " + std::to_string(transactions_.size()) +
                     " transaction(s)");
    return loaded;
}

// ---------------------------------------------------------------------------
// shutdown()
// ---------------------------------------------------------------------------
void Library::shutdown() {
    if (authenticated_) {
        // Persist before tearing anything down.
        saveAll();
        logger_->log(Logger::Event::LOGOUT, "Session closed at shutdown");
        authenticated_ = false;
    }
    logger_->log(Logger::Event::SYSTEM_SHUTDOWN, "Library subsystem shutting down");
    logger_->flush();
    logger_->shutdown();
}

// ---------------------------------------------------------------------------
// Authentication
// ---------------------------------------------------------------------------
bool Library::authenticate(const std::string& username,
                           const std::string& password) {
    if (auth_->login(username, password)) {
        authenticated_ = true;
        logger_->log(Logger::Event::LOGIN,
                     "Admin '" + username + "' signed in");
        return true;
    }

    logger_->log(Logger::Event::ERROR,
                 "Failed sign-in attempt for '" + username + "'");
    return false;
}

bool Library::changePassword(const std::string& user,
                             const std::string& oldPass,
                             const std::string& newPass) {
    if (newPass.size() < 6) {
        logger_->log(Logger::Event::ERROR,
                     "Password change rejected: new password too short");
        return false;
    }

    if (auth_->changePassword(user, oldPass, newPass)) {
        logger_->log(Logger::Event::SYSTEM_START,
                     "Admin password changed for '" + user + "'");
        return true;
    }

    logger_->log(Logger::Event::ERROR,
                 "Password change failed for '" + user + "'");
    return false;
}

// ---------------------------------------------------------------------------
// Book management
// ---------------------------------------------------------------------------
bool Library::addBook(const Book& book) {
    return addBookChecked(book) == AddResult::Added;
}

// addBookChecked() is the real implementation.  The bool overload above stays
// for callers that only care whether it worked (and for the existing tests);
// the UI uses this one so it can tell the user *why* an add was refused.
Library::AddResult Library::addBookChecked(const Book& book) {
    if (!book.isValid()) {
        logger_->log(Logger::Event::ERROR,
                     "addBook rejected: record failed validation");
        return AddResult::InvalidRecord;
    }
    if (findBook(book.getId()) != nullptr) {
        logger_->log(Logger::Event::ERROR,
                     "addBook rejected: duplicate book id " +
                         std::to_string(book.getId()));
        return AddResult::DuplicateId;
    }

    books_.push_back(book);
    logger_->log(Logger::Event::BOOK_ADD,
                 "Book " + std::to_string(book.getId()) + " added: '" +
                     book.getTitle() + "' by " + book.getAuthor());
    notifyDriver("BOOK_ADD:" + std::to_string(book.getId()));
    saveAll();
    return AddResult::Added;
}

bool Library::removeBook(int id) {
    auto it = std::find_if(books_.begin(), books_.end(),
                           [id](const Book& b) { return b.getId() == id; });
    if (it == books_.end()) {
        logger_->log(Logger::Event::ERROR,
                     "removeBook failed: no book with id " + std::to_string(id));
        return false;
    }

    // Refuse to remove a title that still has an open loan - otherwise the
    // catalogue and the transaction history would disagree.
    const bool hasOpenLoan =
        std::any_of(transactions_.begin(), transactions_.end(),
                    [id](const Transaction& t) {
                        return t.getBookId() == id && t.isActive();
                    });
    if (hasOpenLoan) {
        logger_->log(Logger::Event::ERROR,
                     "removeBook refused: book " + std::to_string(id) +
                         " is currently issued");
        return false;
    }

    const std::string title = it->getTitle();
    books_.erase(it);
    logger_->log(Logger::Event::BOOK_REMOVE,
                 "Book " + std::to_string(id) + " removed: '" + title + "'");
    notifyDriver("BOOK_REMOVE:" + std::to_string(id));
    saveAll();
    return true;
}

bool Library::updateBook(const Book& book) {
    if (!book.isValid()) {
        logger_->log(Logger::Event::ERROR,
                     "updateBook rejected: record failed validation");
        return false;
    }

    auto it = std::find_if(books_.begin(), books_.end(),
                           [&book](const Book& b) {
                               return b.getId() == book.getId();
                           });
    if (it == books_.end()) {
        logger_->log(Logger::Event::ERROR,
                     "updateBook failed: no book with id " +
                         std::to_string(book.getId()));
        return false;
    }

    // Copies on loan cannot be silently redefined.
    const int outstanding = it->getTotalCopies() - it->getAvailableCopies();
    if (book.getAvailableCopies() < outstanding) {
        logger_->log(Logger::Event::ERROR,
                     "updateBook rejected: " + std::to_string(outstanding) +
                         " cop(y/ies) of book " + std::to_string(book.getId()) +
                         " are still on loan");
        return false;
    }

    *it = book;
    logger_->log(Logger::Event::BOOK_UPDATE,
                 "Book " + std::to_string(book.getId()) + " updated: '" +
                     book.getTitle() + "'");
    notifyDriver("BOOK_UPDATE:" + std::to_string(book.getId()));
    saveAll();
    return true;
}

const Book* Library::findBook(int id) const {
    auto it = std::find_if(books_.begin(), books_.end(),
                           [id](const Book& b) { return b.getId() == id; });
    return (it == books_.end()) ? nullptr : &(*it);
}

std::vector<Book> Library::searchBooks(const std::string& query,
                                       const std::string& field) const {
    std::vector<Book> hits;

    // An empty field name means "search everything".
    const bool all = field.empty() || Utils::toLower(field) == "all";

    for (const Book& b : books_) {
        bool hit = false;
        if (all) {
            hit = Utils::containsIgnoreCase(b.getTitle(), query) ||
                  Utils::containsIgnoreCase(b.getAuthor(), query) ||
                  Utils::containsIgnoreCase(b.getCategory(), query) ||
                  Utils::containsIgnoreCase(b.getIsbn(), query);
        } else if (Utils::toLower(field) == "title") {
            hit = Utils::containsIgnoreCase(b.getTitle(), query);
        } else if (Utils::toLower(field) == "author") {
            hit = Utils::containsIgnoreCase(b.getAuthor(), query);
        } else if (Utils::toLower(field) == "category") {
            hit = Utils::containsIgnoreCase(b.getCategory(), query);
        } else if (Utils::toLower(field) == "isbn") {
            hit = Utils::containsIgnoreCase(b.getIsbn(), query);
        }
        if (hit) hits.push_back(b);
    }
    return hits;
}

// ---------------------------------------------------------------------------
// Member management
// ---------------------------------------------------------------------------
bool Library::addMember(const Member& member) {
    if (!member.isValid()) {
        logger_->log(Logger::Event::ERROR,
                     "addMember rejected: record failed validation");
        return false;
    }
    if (findMember(member.getMemberId()) != nullptr) {
        logger_->log(Logger::Event::ERROR,
                     "addMember rejected: duplicate member id " +
                         std::to_string(member.getMemberId()));
        return false;
    }

    members_.push_back(member);
    logger_->log(Logger::Event::MEMBER_ADD,
                 "Member " + std::to_string(member.getMemberId()) +
                     " registered: " + member.getName());
    notifyDriver("MEMBER_ADD:" + std::to_string(member.getMemberId()));
    saveAll();
    return true;
}

bool Library::removeMember(int memberId) {
    auto it = std::find_if(members_.begin(), members_.end(),
                           [memberId](const Member& m) {
                               return m.getMemberId() == memberId;
                           });
    if (it == members_.end()) {
        logger_->log(Logger::Event::ERROR,
                     "removeMember failed: no member with id " +
                         std::to_string(memberId));
        return false;
    }

    const bool hasOpenLoan =
        std::any_of(transactions_.begin(), transactions_.end(),
                    [memberId](const Transaction& t) {
                        return t.getMemberId() == memberId && t.isActive();
                    });
    if (hasOpenLoan) {
        logger_->log(Logger::Event::ERROR,
                     "removeMember refused: member " +
                         std::to_string(memberId) + " still has books on loan");
        return false;
    }

    const std::string name = it->getName();
    members_.erase(it);
    logger_->log(Logger::Event::MEMBER_REMOVE,
                 "Member " + std::to_string(memberId) + " removed: " + name);
    notifyDriver("MEMBER_REMOVE:" + std::to_string(memberId));
    saveAll();
    return true;
}

bool Library::updateMember(const Member& member) {
    if (!member.isValid()) {
        logger_->log(Logger::Event::ERROR,
                     "updateMember rejected: record failed validation");
        return false;
    }

    auto it = std::find_if(members_.begin(), members_.end(),
                           [&member](const Member& m) {
                               return m.getMemberId() == member.getMemberId();
                           });
    if (it == members_.end()) {
        logger_->log(Logger::Event::ERROR,
                     "updateMember failed: no member with id " +
                         std::to_string(member.getMemberId()));
        return false;
    }

    *it = member;
    logger_->log(Logger::Event::MEMBER_UPDATE,
                 "Member " + std::to_string(member.getMemberId()) +
                     " updated: " + member.getName());
    notifyDriver("MEMBER_UPDATE:" + std::to_string(member.getMemberId()));
    saveAll();
    return true;
}

const Member* Library::findMember(int memberId) const {
    auto it = std::find_if(members_.begin(), members_.end(),
                           [memberId](const Member& m) {
                               return m.getMemberId() == memberId;
                           });
    return (it == members_.end()) ? nullptr : &(*it);
}

std::vector<Member> Library::searchMembers(const std::string& query) const {
    std::vector<Member> hits;
    for (const Member& m : members_) {
        if (Utils::containsIgnoreCase(m.getName(), query) ||
            Utils::containsIgnoreCase(m.getEmail(), query) ||
            Utils::containsIgnoreCase(m.getPhone(), query) ||
            Utils::containsIgnoreCase(m.getDepartment(), query)) {
            hits.push_back(m);
        }
    }
    return hits;
}

// ---------------------------------------------------------------------------
// Transactions
// ---------------------------------------------------------------------------
bool Library::issueBook(int bookId, int memberId) {
    // Non-const handles: issuing a loan mutates the copy counts.
    Book* book = nullptr;
    auto bit = std::find_if(books_.begin(), books_.end(),
                            [bookId](const Book& b) { return b.getId() == bookId; });
    if (bit != books_.end()) book = &(*bit);

    if (book == nullptr) {
        logger_->log(Logger::Event::ERROR,
                     "issueBook failed: unknown book id " +
                         std::to_string(bookId));
        return false;
    }

    Member* member = nullptr;
    auto mit = std::find_if(members_.begin(), members_.end(),
                            [memberId](const Member& m) {
                                return m.getMemberId() == memberId;
                            });
    if (mit != members_.end()) member = &(*mit);

    if (member == nullptr) {
        logger_->log(Logger::Event::ERROR,
                     "issueBook failed: unknown member id " +
                         std::to_string(memberId));
        return false;
    }

    if (!book->hasAvailableCopies()) {
        logger_->log(Logger::Event::ERROR,
                     "issueBook refused: no copy of book " +
                         std::to_string(bookId) + " available");
        return false;
    }

    if (!member->canBorrow()) {
        logger_->log(Logger::Event::ERROR,
                     "issueBook refused: member " + std::to_string(memberId) +
                         " already holds the maximum of " +
                         std::to_string(Config::MAX_BOOKS_PER_MEMBER) + " books");
        return false;
    }

    const std::string issue = Utils::today();
    const std::string due   = Utils::addDays(issue, Config::DEFAULT_LOAN_DAYS);

    try {
        Transaction txn(nextTransactionId_, bookId, memberId, issue, due);
        transactions_.push_back(txn);
    } catch (const std::exception& e) {
        logger_->log(Logger::Event::ERROR,
                     std::string("issueBook failed: ") + e.what());
        return false;
    }
    ++nextTransactionId_;

    book->setAvailableCopies(book->getAvailableCopies() - 1);
    member->incrementIssued();

    logger_->log(Logger::Event::BOOK_ISSUE,
                 "Book " + std::to_string(bookId) + " ('" + book->getTitle() +
                     "') issued to member " + std::to_string(memberId) +
                     "; due " + due);
    notifyDriver("BOOK_ISSUE:" + std::to_string(bookId) + ":" +
                 std::to_string(memberId));

    saveAll();
    return true;
}

bool Library::returnBook(int transactionId) {
    auto it = std::find_if(
        transactions_.begin(), transactions_.end(),
        [transactionId](const Transaction& t) {
            return t.getTransactionId() == transactionId;
        });

    if (it == transactions_.end()) {
        logger_->log(Logger::Event::ERROR,
                     "returnBook failed: no transaction " +
                         std::to_string(transactionId));
        return false;
    }

    if (it->getStatus() == Transaction::Status::RETURNED) {
        logger_->log(Logger::Event::ERROR,
                     "returnBook refused: transaction " +
                         std::to_string(transactionId) + " is already returned");
        return false;
    }

    const std::string today = Utils::today();
    const int overdueDays   = Utils::daysBetween(today, it->getDueDate());
    const double fee = (overdueDays > 0)
                           ? static_cast<double>(overdueDays) * Config::LATE_FEE_PER_DAY
                           : 0.0;

    it->setReturnDate(today);
    it->setLateFee(fee);
    it->setStatus(Transaction::Status::RETURNED);

    // Give the copy back to the catalogue.
    Book* book = nullptr;
    auto bit = std::find_if(books_.begin(), books_.end(),
                            [this, &it](const Book& b) {
                                return b.getId() == it->getBookId();
                            });
    if (bit != books_.end()) {
        book = &(*bit);
        if (book->getAvailableCopies() < book->getTotalCopies()) {
            book->setAvailableCopies(book->getAvailableCopies() + 1);
        }
    }

    // And decrement the member's running count.
    auto mit = std::find_if(members_.begin(), members_.end(),
                            [this, &it](const Member& m) {
                                return m.getMemberId() == it->getMemberId();
                            });
    if (mit != members_.end()) mit->decrementIssued();

    std::ostringstream msg;
    msg << "Book " << it->getBookId() << " returned by member "
        << it->getMemberId() << " on " << today << "; transaction #"
        << transactionId << "; late fee " << fee;
    logger_->log(Logger::Event::BOOK_RETURN, msg.str());
    notifyDriver("BOOK_RETURN:" + std::to_string(it->getBookId()) + ":" +
                 std::to_string(it->getMemberId()));

    saveAll();
    return true;
}

const Transaction* Library::findTransaction(int transactionId) const {
    auto it = std::find_if(
        transactions_.begin(), transactions_.end(),
        [transactionId](const Transaction& t) {
            return t.getTransactionId() == transactionId;
        });
    return (it == transactions_.end()) ? nullptr : &(*it);
}

std::vector<Transaction> Library::activeTransactions() const {
    std::vector<Transaction> out;
    for (const Transaction& t : transactions_) {
        if (t.isActive()) out.push_back(t);
    }
    return out;
}

std::vector<Transaction> Library::memberHistory(int memberId) const {
    std::vector<Transaction> out;
    for (const Transaction& t : transactions_) {
        if (t.getMemberId() == memberId) out.push_back(t);
    }
    return out;
}

// ---------------------------------------------------------------------------
// refreshOverdueStatus()
// ---------------------------------------------------------------------------
void Library::refreshOverdueStatus() {
    const std::string today = Utils::today();
    bool changed = false;

    for (Transaction& t : transactions_) {
        if (!t.isActive()) continue;

        const int overdue = Utils::daysBetween(today, t.getDueDate());
        const Transaction::Status wanted =
            (overdue > 0) ? Transaction::Status::OVERDUE
                          : Transaction::Status::ISSUED;

        if (t.getStatus() != wanted) {
            t.setStatus(wanted);
            changed = true;
        }

        // Keep the projected fee current so reports are accurate before the
        // book actually comes back.  It applies to overdue loans as well,
        // otherwise the fee would only ever appear after the return.
        const double projected =
            (overdue > 0) ? static_cast<double>(overdue) * Config::LATE_FEE_PER_DAY
                          : 0.0;
        if (t.getLateFee() != projected) {
            t.setLateFee(projected);
            changed = true;
        }
    }

    if (changed) {
        logger_->log(Logger::Event::SYSTEM_START,
                     "Overdue status refreshed for " + today);
        saveAll();
    }
}

// ---------------------------------------------------------------------------
// Persistence
// ---------------------------------------------------------------------------
bool Library::saveAll() {
    bool ok = true;

    try {
        // Truncate-then-write: FileManager gives us a whole-file writer, so
        // opening with out|trunc keeps the file consistent with the vectors.
        fileManager_->open(Config::BOOKS_FILE,
                           std::ios::out | std::ios::binary | std::ios::trunc);
        ok = fileManager_->writeAll(books_) && ok;
        fileManager_->close();

        fileManager_->open(Config::MEMBERS_FILE,
                           std::ios::out | std::ios::binary | std::ios::trunc);
        ok = fileManager_->writeAll(members_) && ok;
        fileManager_->close();

        fileManager_->open(Config::TRANSACTIONS_FILE,
                           std::ios::out | std::ios::binary | std::ios::trunc);
        ok = fileManager_->writeAll(transactions_) && ok;
        fileManager_->close();
    } catch (const FileIOException& e) {
        logger_->log(Logger::Event::ERROR,
                     std::string("saveAll failed: ") + e.what());
        return false;
    }

    return ok;
}

bool Library::loadAll() {
    struct stat st{};

    // A missing file is a normal first-run condition, not an error.
    //   - file absent            -> nothing to load
    //   - file present but empty -> nothing to load
    //   - file present, non-empty, but zero records decoded -> the content is
    //     corrupt, which must be reported rather than silently ignored.
    const auto loadCollection = [this, &st](const char* path,
                                            const char* label,
                                            auto& target) {
        if (::stat(path, &st) != 0) {
            logger_->log(Logger::Event::SYSTEM_START,
                         std::string("No ") + label + " file yet; starting empty");
            return;
        }

        FileManager fm;
        try {
            fm.open(path, std::ios::in | std::ios::binary);
            fm.readAll(target);
            fm.close();
        } catch (const FileIOException& e) {
            logger_->log(Logger::Event::ERROR,
                         std::string("cannot read ") + label + ": " + e.what());
            return;
        }

        if (target.empty() && st.st_size > 0) {
            logger_->log(Logger::Event::ERROR,
                         std::string("data file '") + path +
                             "' contained no valid records and was ignored");
        }
    };

    loadCollection(Config::BOOKS_FILE, "books", books_);
    loadCollection(Config::MEMBERS_FILE, "members", members_);
    loadCollection(Config::TRANSACTIONS_FILE, "transactions", transactions_);

    // Continue the transaction id sequence after a restart, otherwise new
    // loans would reuse ids from the previous run.
    int highest = Config::TRANSACTION_ID_START - 1;
    for (const Transaction& t : transactions_) {
        if (t.getTransactionId() > highest) highest = t.getTransactionId();
    }
    nextTransactionId_ = highest + 1;

    refreshOverdueStatus();

    logger_->log(Logger::Event::SYSTEM_START,
                 "Loaded " + std::to_string(books_.size()) + " book(s), " +
                     std::to_string(members_.size()) + " member(s), " +
                     std::to_string(transactions_.size()) + " transaction(s)");
    return true;
}

// ---------------------------------------------------------------------------
// Reports
// ---------------------------------------------------------------------------
void Library::printSummary() {
    refreshOverdueStatus();
    report_->refresh();
    report_->printSummary(std::cout);
}

void Library::printOverdue() {
    refreshOverdueStatus();
    report_->refresh();
    report_->printOverdue(std::cout);
}

void Library::printRecentlyIssued() {
    report_->refresh();
    report_->printRecentlyIssued(std::cout, Config::RECENT_ISSUED_COUNT);
}

void Library::printMemberHistory(int memberId) {
    report_->refresh();
    report_->printBorrowingHistory(memberId, std::cout);
}

// ---------------------------------------------------------------------------
// notifyDriver()
// ---------------------------------------------------------------------------
void Library::notifyDriver(const std::string& event) {
    if (driver_ && driver_->isAvailable()) {
        if (!driver_->sendEvent(event)) {
            logger_->log(Logger::Event::DRIVER_EVENT,
                         "driver write failed for event '" + event + "'");
        }
    }

    // Same event, this time to the forked monitor process over the pipe.
    if (monitor_ != nullptr && monitor_->isRunning()) {
        monitor_->sendEvent(event);
    }

    // Both channels are optional: never let them break the operation.
}