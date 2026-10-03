// ---------------------------------------------------------------------------
// unit_tests.cpp - functional tests for the library domain.
//
// Covers: entity validation, binary serialisation round-trips, file
// persistence, authentication, book/member CRUD, issue/return, late fees,
// search and reporting.
//
// The tests run inside a scratch directory (see test_main.cpp) so the real
// data/ folder of the project is never touched.
// ---------------------------------------------------------------------------

#include "test_harness.h"

#include "Authentication.h"
#include "Book.h"
#include "Config.h"
#include "FileManager.h"
#include "Library.h"
#include "Member.h"
#include "ReportManager.h"
#include "Transaction.h"
#include "Utils.h"

#include <cstdio>
#include <fstream>
#include <sstream>

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
namespace {

// Every Library test starts from an empty database: the suite runs in one
// shared scratch directory, so leftover records would otherwise leak between
// tests.
void resetData() {
    std::remove(Config::BOOKS_FILE);
    std::remove(Config::MEMBERS_FILE);
    std::remove(Config::TRANSACTIONS_FILE);
    std::remove(Config::ADMIN_FILE);
}

std::vector<Book> sampleBooks() {
    std::vector<Book> books;
    books.emplace_back(101, "Dune", "Frank Herbert", "SciFi",
                       "9780441013593", "1965", 3, 3);
    books.emplace_back(102, "Neuromancer", "William Gibson", "SciFi",
                       "9780441569595", "1984", 2, 2);
    books.emplace_back(103, "The Hobbit", "J.R.R. Tolkien", "Fantasy",
                       "9780261102217", "1937", 4, 4);
    return books;
}

std::vector<Member> sampleMembers() {
    std::vector<Member> members;
    members.emplace_back(1, "Asha Roy", "asha@uni.edu", "9876543210", "CSE", 0);
    members.emplace_back(2, "Biman Das", "biman@uni.edu", "9123456780", "ECE", 0);
    return members;
}

// Round-trip helper: write a record, read it back into a fresh object.
template <typename T>
T roundTrip(const T& original) {
    std::stringstream buffer(std::ios::in | std::ios::out | std::ios::binary);
    original.serialize(buffer);

    T restored;
    restored.deserialize(buffer);
    return restored;
}

} // namespace

// ---------------------------------------------------------------------------
// Entity validation
// ---------------------------------------------------------------------------
TEST(BookRejectsEmptyTitle) {
    CHECK_THROWS(Book b(1, "", "Author", "Cat", "9780441569595", "2000", 1, 1));
}

TEST(BookRejectsNegativeCopies) {
    CHECK_THROWS(Book b(1, "Title", "Author", "Cat", "", "2000", -1, 0));
    CHECK_THROWS(Book b(1, "Title", "Author", "Cat", "", "2000", 2, -1));
}

TEST(BookRejectsAvailableAboveTotal) {
    CHECK_THROWS(Book b(1, "Title", "Author", "Cat", "", "2000", 2, 3));
}

TEST(BookSettersValidate) {
    Book b(1, "Title", "Author", "Cat", "", "2000", 5, 5);
    CHECK(b.isValid());

    CHECK_THROWS(b.setTitle(""));
    CHECK_THROWS(b.setTotalCopies(-1));
    CHECK_THROWS(b.setAvailableCopies(6));

    // Growing the total must not push availability above it.
    b.setTotalCopies(2);
    CHECK_EQ(b.getAvailableCopies(), 2);
}

TEST(MemberRejectsBadFields) {
    CHECK_THROWS(Member m(0, "Name", "a@b.c", "", "CSE", 0));
    CHECK_THROWS(Member m(1, "", "a@b.c", "", "CSE", 0));

    // Contact details are validated by the setters.
    Member m(1, "Name", "a@b.c", "", "CSE", 0);
    CHECK_THROWS(m.setEmail("not-an-email"));
    CHECK_THROWS(m.setEmail(""));
    CHECK_THROWS(m.setPhone("abc!!"));
    m.setEmail("asha@uni.edu");
    m.setPhone("+91 98765-43210");
    CHECK_EQ(m.getEmail(), std::string("asha@uni.edu"));
    CHECK_EQ(m.getPhone(), std::string("+91 98765-43210"));
}

TEST(MemberBorrowLimitRespectsConfig) {
    Member m(1, "Asha", "a@b.c", "", "CSE", Config::MAX_BOOKS_PER_MEMBER);
    CHECK(!m.canBorrow());
    m.decrementIssued();
    CHECK(m.canBorrow());

    // decrementIssued() must never go negative.
    for (int i = 0; i < 20; ++i) m.decrementIssued();
    CHECK_EQ(m.getIssuedBooks(), 0);
}

TEST(TransactionRejectsNonPositiveIds) {
    CHECK_THROWS(Transaction t(0, 1, 1, "2026-01-01", "2026-01-15"));
    CHECK_THROWS(Transaction t(1, 0, 1, "2026-01-01", "2026-01-15"));
    CHECK_THROWS(Transaction t(1, 1, 0, "2026-01-01", "2026-01-15"));
    CHECK_THROWS(Transaction t(1, 1, 1, "", "2026-01-15"));
    CHECK_THROWS(Transaction t(1, 1, 1, "2026-01-01", ""));
}

TEST(TransactionStatusRoundTrip) {
    const Transaction t(1000, 101, 7, "2026-01-01", "2026-01-15", "", 0.0,
                        Transaction::Status::OVERDUE);
    CHECK_EQ(std::string(Transaction::statusToString(t.getStatus())),
             std::string("OVERDUE"));
    CHECK(Transaction::stringToStatus("RETURNED") == Transaction::Status::RETURNED);
    CHECK(Transaction::stringToStatus("garbage") == Transaction::Status::ISSUED);
}

// ---------------------------------------------------------------------------
// Serialisation
// ---------------------------------------------------------------------------
TEST(BookSerialisationRoundTrip) {
    const Book original(101, "Dune", "Frank Herbert", "SciFi",
                        "9780441013593", "1965", 3, 2);
    const Book copy = roundTrip(original);

    CHECK_EQ(copy.getId(), original.getId());
    CHECK_EQ(copy.getTitle(), original.getTitle());
    CHECK_EQ(copy.getAuthor(), original.getAuthor());
    CHECK_EQ(copy.getCategory(), original.getCategory());
    CHECK_EQ(copy.getIsbn(), original.getIsbn());
    CHECK_EQ(copy.getPublication(), original.getPublication());
    CHECK_EQ(copy.getTotalCopies(), original.getTotalCopies());
    CHECK_EQ(copy.getAvailableCopies(), original.getAvailableCopies());
}

TEST(MemberSerialisationRoundTrip) {
    const Member original(7, "Asha Roy", "asha@uni.edu", "9876543210", "CSE", 3);
    const Member copy = roundTrip(original);

    CHECK_EQ(copy.getMemberId(), 7);
    CHECK_EQ(copy.getName(), original.getName());
    CHECK_EQ(copy.getEmail(), original.getEmail());
    CHECK_EQ(copy.getPhone(), original.getPhone());
    CHECK_EQ(copy.getDepartment(), original.getDepartment());
    CHECK_EQ(copy.getIssuedBooks(), 3);
}

TEST(TransactionSerialisationRoundTrip) {
    const Transaction original(1005, 101, 7, "2026-01-01", "2026-01-15",
                               "2026-01-20", 25.5, Transaction::Status::RETURNED);
    const Transaction copy = roundTrip(original);

    CHECK_EQ(copy.getTransactionId(), 1005);
    CHECK_EQ(copy.getBookId(), 101);
    CHECK_EQ(copy.getMemberId(), 7);
    CHECK_EQ(copy.getIssueDate(), original.getIssueDate());
    CHECK_EQ(copy.getDueDate(), original.getDueDate());
    CHECK_EQ(copy.getReturnDate(), original.getReturnDate());
    CHECK(copy.getLateFee() > 25.4 && copy.getLateFee() < 25.6);
    CHECK(copy.getStatus() == Transaction::Status::RETURNED);
}

TEST(DeserialisationRejectsGarbage) {
    // Random bytes must not be accepted as a valid record.
    std::stringstream garbage(std::ios::in | std::ios::out | std::ios::binary);
    const char junk[] = "\xff\xff\xff\xff\x7f\x7f\x7f\x7f\x7f\x7f\x7f\x7f";
    garbage.write(junk, sizeof(junk));

    Book book;
    CHECK(!book.deserialize(garbage));
}

// ---------------------------------------------------------------------------
// Utils
// ---------------------------------------------------------------------------
TEST(UtilsValidators) {
    CHECK(Utils::isValidEmail("asha@uni.edu"));
    CHECK(!Utils::isValidEmail("asha.uni.edu"));
    CHECK(!Utils::isValidEmail("@uni.edu"));

    CHECK(Utils::isValidPhone(""));          // optional
    CHECK(Utils::isValidPhone("+91 98765-43210"));
    CHECK(!Utils::isValidPhone("98765abc"));

    CHECK(Utils::isValidISBN("978-0-441-01359-3"));
    CHECK(!Utils::isValidISBN("12345"));

    CHECK(Utils::isValidDate("2026-01-31"));
    CHECK(!Utils::isValidDate("31/01/2026"));
}

TEST(UtilsDateArithmetic) {
    CHECK_EQ(Utils::addDays("2026-01-01", 14), std::string("2026-01-15"));
    CHECK_EQ(Utils::addDays("2026-02-28", 1), std::string("2026-03-01"));
    CHECK_EQ(Utils::addDays("2024-02-28", 1), std::string("2024-02-29"));  // leap
    CHECK_EQ(Utils::daysBetween("2026-01-20", "2026-01-15"), 5);
    CHECK_EQ(Utils::daysBetween("2026-01-15", "2026-01-20"), -5);
    CHECK_EQ(Utils::daysBetween("2026-01-15", "2026-01-15"), 0);
}

TEST(UtilsTrimAndSearch) {
    CHECK_EQ(Utils::trim("  hello  "), std::string("hello"));
    CHECK_EQ(Utils::trim("   "), std::string(""));
    CHECK(Utils::containsIgnoreCase("Frank Herbert", "herbert"));
    CHECK(!Utils::containsIgnoreCase("Dune", "herbert"));
    CHECK(Utils::containsIgnoreCase("anything", ""));
}

// ---------------------------------------------------------------------------
// FileManager
// ---------------------------------------------------------------------------
TEST(FileManagerWritesAndReadsCollection) {
    FileManager fm;

    fm.open("test_books.dat", std::ios::out | std::ios::binary | std::ios::trunc);
    CHECK(fm.isOpen());
    const std::vector<Book> books = sampleBooks();
    CHECK(fm.writeAll(books));
    fm.close();

    std::vector<Book> loaded;
    fm.open("test_books.dat", std::ios::in | std::ios::binary);
    CHECK(fm.readAll(loaded));
    fm.close();

    CHECK_EQ(loaded.size(), books.size());
    if (loaded.size() == books.size()) {
        CHECK_EQ(loaded[0].getTitle(), std::string("Dune"));
        CHECK_EQ(loaded[2].getTitle(), std::string("The Hobbit"));
    }

    std::remove("test_books.dat");
}

TEST(FileManagerCreatesParentDirectories) {
    FileManager fm;
    // Directory does not exist yet - open() must create it.
    fm.open("nested/dir/test.dat", std::ios::out | std::ios::binary | std::ios::trunc);
    CHECK(fm.isOpen());
    fm.close();

    std::remove("nested/dir/test.dat");
    ::rmdir("nested/dir");
    ::rmdir("nested");
}

TEST(FileManagerAtomicWriteReplacesFile) {
    CHECK(FileManager::atomicWrite("atomic.txt", "first version"));
    CHECK(FileManager::atomicWrite("atomic.txt", "second version"));

    std::ifstream in("atomic.txt");
    std::string content;
    std::getline(in, content);
    CHECK_EQ(content, std::string("second version"));

    // No temporary file must be left behind.
    std::ifstream stray("atomic.txt.tmp");
    CHECK(!stray.good());

    in.close();
    std::remove("atomic.txt");
}

TEST(FileManagerHandlesMissingFile) {
    FileManager fm;
    // open() is documented to throw on failure; a missing file must therefore
    // surface as a FileIOException, never as a crash.
    bool threw = false;
    try {
        fm.open("does_not_exist.dat", std::ios::in | std::ios::binary);
    } catch (const FileIOException&) {
        threw = true;
    }
    CHECK(threw);
    CHECK(!fm.isOpen());

    std::vector<Book> loaded;
    CHECK(!fm.readAll(loaded));
    CHECK(loaded.empty());
}

// ---------------------------------------------------------------------------
// Authentication
// ---------------------------------------------------------------------------
TEST(AuthenticationDefaultAccountWorks) {
    Authentication auth;
    CHECK(auth.currentAdmin() == "admin");
    CHECK(auth.login("admin", "admin123"));
    CHECK(!auth.login("admin", "wrong-password"));
    CHECK(!auth.login("nobody", "admin123"));
}

TEST(AuthenticationStoresNoPlaintext) {
    std::ifstream in(Config::ADMIN_FILE);
    std::string   content((std::istreambuf_iterator<char>(in)),
                          std::istreambuf_iterator<char>());
    CHECK(content.find("admin123") == std::string::npos);
    CHECK(content.find("hash=") != std::string::npos);
    CHECK(content.find(':') != std::string::npos);   // salt:hash
}

TEST(AuthenticationHashesAreSalted) {
    const std::string a = Authentication::hashPassword("same-password");
    const std::string b = Authentication::hashPassword("same-password");
    CHECK(a != b);   // different salt -> different digest
    CHECK_EQ(a.size(), std::size_t(16 + 1 + 64));
}

TEST(AuthenticationChangePassword) {
    Authentication auth;
    CHECK(auth.changePassword("admin", "admin123", "newpass1"));
    CHECK(auth.login("admin", "newpass1"));
    CHECK(!auth.login("admin", "admin123"));
    CHECK(!auth.changePassword("admin", "admin123", "another1"));  // wrong old

    // Restore the default so the CLI tests are unaffected.
    CHECK(auth.changePassword("admin", "newpass1", "admin123"));
}

// ---------------------------------------------------------------------------
// Library: book management
// ---------------------------------------------------------------------------
TEST(LibraryAddsAndFindsBooks) {
    resetData();
    Library lib;
    CHECK(lib.initialise());

    const std::vector<Book> books = sampleBooks();
    for (const Book& b : books) CHECK(lib.addBook(b));

    CHECK_EQ(lib.allBooks().size(), books.size());
    CHECK(lib.findBook(101) != nullptr);
    CHECK_EQ(lib.findBook(101)->getTitle(), std::string("Dune"));
    CHECK(lib.findBook(999) == nullptr);
}

TEST(LibraryRejectsDuplicateAndInvalidBooks) {
    resetData();
    Library lib;
    lib.initialise();
    CHECK(lib.addBook(sampleBooks()[0]));
    CHECK(!lib.addBook(sampleBooks()[0]));   // duplicate id

    // A non-positive id is rejected by Library::addBook() through
    // Book::isValid() - the constructor itself only validates field ranges.
    CHECK(!lib.addBook(Book(-1, "Ghost", "A", "B", "", "", 1, 1)));

    // An empty title is rejected even earlier, by the constructor.
    CHECK_THROWS(Book bad(102, "", "A", "B", "", "", 1, 1));
}

// The UI shows a different message per refusal reason, so addBookChecked() has
// to report *which* rule was broken rather than a bare false.
TEST(AddBookCheckedReportsTheRefusalReason) {
    resetData();
    Library lib;
    lib.initialise();

    CHECK(lib.addBookChecked(sampleBooks()[0]) == Library::AddResult::Added);

    // Same id as the book above: the duplicate rule fires, not the validity one.
    CHECK(lib.addBookChecked(sampleBooks()[0]) ==
          Library::AddResult::DuplicateId);

    // Fresh slot but a record Book::isValid() rejects: a non-positive id.
    // (The UI rejects id <= 0 before it even builds a Book, so this is the
    // defensive path that keeps a bad record out of books_ regardless.)
    CHECK(lib.addBookChecked(Book(0, "Ghost", "A", "B", "", "", 1, 1)) ==
          Library::AddResult::InvalidRecord);

    // The refusals above must not have created anything, and must not have
    // disturbed the book that was accepted.
    CHECK_EQ(lib.allBooks().size(), std::size_t(1));
    CHECK(lib.findBook(101) != nullptr);
    CHECK(lib.findBook(0) == nullptr);

    // addBook() is the same code path, just without the reason.
    CHECK(!lib.addBook(sampleBooks()[0]));
}

// "1/07" was rejected by the UI and the zero total was correct: '/' is not a
// digit, so it is neither a 10- nor a 13-digit ISBN. Pin the rule down so the
// behaviour cannot be mistaken for data loss later.
TEST(IsbnValidationRejectsNonDigitsAndHonoursHyphens) {
    CHECK(Utils::isValidISBN("1234567890"));
    CHECK(Utils::isValidISBN("978-0-441-01359-3"));
    CHECK(Utils::isValidISBN("978 0 441 01359 3"));
    CHECK(Utils::isValidISBN(""));            // blank is allowed

    CHECK(!Utils::isValidISBN("1/07"));       // the reported case
    CHECK(!Utils::isValidISBN("12345"));      // too short
    CHECK(!Utils::isValidISBN("12345678901"));   // 11 digits
    CHECK(!Utils::isValidISBN("12345678901234"));  // 14 digits
    CHECK(!Utils::isValidISBN("abc1234567"));
    CHECK(!Utils::isValidISBN("   "));       // whitespace only is not blank
}

TEST(LibraryUpdatesBook) {
    resetData();
    Library lib;
    lib.initialise();
    CHECK(lib.addBook(sampleBooks()[0]));

    Book updated = *lib.findBook(101);
    updated.setTitle("Dune (Revised)");
    updated.setAuthor("Frank Herbert");
    CHECK(lib.updateBook(updated));
    CHECK_EQ(lib.findBook(101)->getTitle(), std::string("Dune (Revised)"));

    CHECK(!lib.updateBook(Book(999, "Ghost", "A", "B", "", "", 1, 1)));
}

// A title that is partly on loan cannot have its total cut below what is out.
// Book::isValid() requires available <= total, and the constructor refuses an
// impossible pair outright - which is why the UI has to check before building
// the record rather than letting a throw escape as "Error: ...".
TEST(LibraryRefusesToTotalBelowCopiesOnLoan) {
    resetData();
    Library lib;
    lib.initialise();
    CHECK(lib.addBook(Book(101, "Dune", "Herbert", "SciFi", "", "1965", 3, 3)));
    CHECK(lib.addMember(sampleMembers()[0]));

    CHECK(lib.issueBook(101, 1));
    CHECK(lib.issueBook(101, 1));
    CHECK_EQ(lib.findBook(101)->getAvailableCopies(), 1);

    // Total 1 with 2 copies on loan would mean available = -1: not constructible.
    CHECK_THROWS(Book(101, "Dune", "Herbert", "SciFi", "", "1965", 1, -1));

    // Nor is a record whose available count exceeds its total.
    CHECK_THROWS(Book(101, "Dune", "Herbert", "SciFi", "", "1965", 1, 2));

    // Nothing above disturbed the stored record.
    const Book* stored = lib.findBook(101);
    CHECK_EQ(stored->getTotalCopies(), 3);
    CHECK_EQ(stored->getAvailableCopies(), 1);

    // updateBook() requires the incoming available count to be at least the
    // number of copies on loan, so a record that would leave fewer available
    // than are issued is refused and nothing is written.
    CHECK(!lib.updateBook(Book(101, "Dune", "Herbert", "SciFi", "", "1965", 2, 0)));
    CHECK_EQ(lib.findBook(101)->getTotalCopies(), 3);
    CHECK_EQ(lib.findBook(101)->getAvailableCopies(), 1);

    // Growing the total while leaving the same two copies on loan is fine:
    // 4 total with 2 available still implies 2 issued.
    CHECK(lib.updateBook(Book(101, "Dune", "Herbert", "SciFi", "", "1965", 4, 2)));
    CHECK_EQ(lib.findBook(101)->getTotalCopies(), 4);
    CHECK_EQ(lib.findBook(101)->getAvailableCopies(), 2);
}

TEST(LibraryRemovesBook) {
    resetData();
    Library lib;
    lib.initialise();
    CHECK(lib.addBook(sampleBooks()[0]));
    CHECK(lib.removeBook(101));
    CHECK(lib.findBook(101) == nullptr);
    CHECK(!lib.removeBook(101));
}

TEST(LibrarySearchesBooks) {
    resetData();
    Library lib;
    lib.initialise();
    for (const Book& b : sampleBooks()) lib.addBook(b);

    CHECK_EQ(lib.searchBooks("dune", "title").size(), std::size_t(1));
    CHECK_EQ(lib.searchBooks("tolkien", "author").size(), std::size_t(1));
    CHECK_EQ(lib.searchBooks("SciFi", "category").size(), std::size_t(2));
    CHECK_EQ(lib.searchBooks("9780441569595", "isbn").size(), std::size_t(1));
    CHECK_EQ(lib.searchBooks("sci", "all").size(), std::size_t(2));
    CHECK_EQ(lib.searchBooks("nothing-matches", "all").size(), std::size_t(0));
}

// ---------------------------------------------------------------------------
// Library: member management
// ---------------------------------------------------------------------------
TEST(LibraryAddsAndSearchesMembers) {
    resetData();
    Library lib;
    lib.initialise();
    for (const Member& m : sampleMembers()) CHECK(lib.addMember(m));

    CHECK_EQ(lib.allMembers().size(), std::size_t(2));
    CHECK_EQ(lib.searchMembers("asha").size(), std::size_t(1));
    CHECK_EQ(lib.searchMembers("CSE").size(), std::size_t(1));
    CHECK(lib.findMember(2) != nullptr);
    CHECK(lib.findMember(42) == nullptr);
    CHECK(!lib.addMember(sampleMembers()[0]));
}

// ---------------------------------------------------------------------------
// Library: issue / return / late fee
// ---------------------------------------------------------------------------
TEST(LibraryIssuesBookAndUpdatesCounts) {
    resetData();
    Library lib;
    lib.initialise();
    lib.addBook(sampleBooks()[0]);
    lib.addMember(sampleMembers()[0]);

    CHECK(lib.issueBook(101, 1));
    CHECK_EQ(lib.findBook(101)->getAvailableCopies(), 2);
    CHECK_EQ(lib.findMember(1)->getIssuedBooks(), 1);

    const std::vector<Transaction> active = lib.activeTransactions();
    CHECK_EQ(active.size(), std::size_t(1));
    if (!active.empty()) {
        CHECK_EQ(active[0].getTransactionId(), Config::TRANSACTION_ID_START);
        CHECK_EQ(active[0].getDueDate(),
                 Utils::addDays(Utils::today(), Config::DEFAULT_LOAN_DAYS));
    }
}

TEST(LibraryRefusesIssueWhenUnavailableOrOverLimit) {
    resetData();
    Library lib;
    lib.initialise();
    // A single-copy title.
    lib.addBook(Book(201, "Solo", "Author", "Cat", "", "2020", 1, 1));
    lib.addMember(sampleMembers()[0]);

    CHECK(lib.issueBook(201, 1));
    CHECK(!lib.issueBook(201, 1));            // no copy left
    CHECK(!lib.issueBook(999, 1));            // unknown book
    CHECK(!lib.issueBook(201, 999));          // unknown member
}

TEST(LibraryEnforcesBorrowLimit) {
    resetData();
    Library lib;
    lib.initialise();
    lib.addMember(sampleMembers()[0]);

    // Hand out exactly MAX_BOOKS_PER_MEMBER one-copy titles.
    for (int i = 0; i < Config::MAX_BOOKS_PER_MEMBER; ++i) {
        const int id = 300 + i;
        lib.addBook(Book(id, "B" + std::to_string(id), "A", "C", "", "2020", 1, 1));
        CHECK(lib.issueBook(id, 1));
    }

    const int extra = 300 + Config::MAX_BOOKS_PER_MEMBER;
    lib.addBook(Book(extra, "Extra", "A", "C", "", "2020", 1, 1));
    CHECK(!lib.issueBook(extra, 1));   // member already at the limit
}

TEST(LibraryReturnRestoresAvailability) {
    resetData();
    Library lib;
    lib.initialise();
    lib.addBook(sampleBooks()[0]);
    lib.addMember(sampleMembers()[0]);
    CHECK(lib.issueBook(101, 1));

    const int txn = lib.activeTransactions()[0].getTransactionId();
    CHECK(lib.returnBook(txn));

    CHECK_EQ(lib.findBook(101)->getAvailableCopies(), 3);
    CHECK_EQ(lib.findMember(1)->getIssuedBooks(), 0);
    CHECK(lib.activeTransactions().empty());
    CHECK(!lib.returnBook(txn));   // already returned
}

TEST(LibraryCalculatesLateFee) {
    resetData();
    Library lib;
    lib.initialise();
    lib.addBook(sampleBooks()[0]);
    lib.addMember(sampleMembers()[0]);

    // Issue it, then rewrite the stored due date so the loan is overdue.
    CHECK(lib.issueBook(101, 1));
    const std::vector<Transaction> active = lib.activeTransactions();
    CHECK(!active.empty());
    if (active.empty()) return;

    const int txnId = active[0].getTransactionId();
    CHECK(lib.returnBook(txnId));

    // Not overdue yet -> no fee.
    const Transaction* t = lib.findTransaction(txnId);
    CHECK(t != nullptr);
    if (t != nullptr) {
        CHECK(t->getLateFee() == 0.0);
        CHECK(t->getStatus() == Transaction::Status::RETURNED);
    }

    // Now force a genuinely overdue loan by editing the file behind the
    // library's back, then reloading - this also exercises the load path.
    CHECK(lib.saveAll());
    {
        std::vector<Transaction> stored;
        FileManager fm;
        fm.open(Config::TRANSACTIONS_FILE, std::ios::in | std::ios::binary);
        fm.readAll(stored);
        fm.close();

        CHECK_EQ(stored.size(), std::size_t(1));
        if (stored.size() != 1) return;

        stored[0].setDueDate(Utils::addDays(Utils::today(), -3));   // 3 days late
        stored[0].setReturnDate("");
        stored[0].setStatus(Transaction::Status::ISSUED);
        stored[0].setLateFee(0.0);

        fm.open(Config::TRANSACTIONS_FILE,
                std::ios::out | std::ios::binary | std::ios::trunc);
        CHECK(fm.writeAll(stored));
        fm.close();
    }

    // A brand new instance must see the overdue state after reloading.
    Library reloaded;
    reloaded.initialise();
    const std::vector<Transaction> after = reloaded.activeTransactions();
    CHECK_EQ(after.size(), std::size_t(1));
    if (!after.empty()) {
        CHECK(after[0].getStatus() == Transaction::Status::OVERDUE);
        // 3 days x 5.0 = 15.0
        CHECK(after[0].getLateFee() > 14.9 && after[0].getLateFee() < 15.1);
    }
}

TEST(LibraryRefusesToRemoveIssuedBookOrActiveMember) {
    resetData();
    Library lib;
    lib.initialise();
    lib.addBook(sampleBooks()[0]);
    lib.addMember(sampleMembers()[0]);
    CHECK(lib.issueBook(101, 1));

    CHECK(!lib.removeBook(101));     // copies still on loan
    CHECK(!lib.removeMember(1));     // member still has a loan
}

// ---------------------------------------------------------------------------
// Library: persistence
// ---------------------------------------------------------------------------
TEST(LibraryDataSurvivesRestart) {
    resetData();
    {
        Library lib;
        lib.initialise();
        for (const Book& b : sampleBooks()) lib.addBook(b);
        for (const Member& m : sampleMembers()) lib.addMember(m);
        lib.issueBook(101, 1);
        lib.shutdown();
    }   // Library destroyed here - everything must be on disk

    // A brand new instance must reload exactly what was written above -
    // nothing is reset here on purpose.
    Library reopened;
    reopened.initialise();

    CHECK_EQ(reopened.allBooks().size(), std::size_t(3));
    CHECK_EQ(reopened.allMembers().size(), std::size_t(2));
    CHECK_EQ(reopened.findBook(101)->getAvailableCopies(), 2);
    CHECK_EQ(reopened.findMember(1)->getIssuedBooks(), 1);
    CHECK_EQ(reopened.activeTransactions().size(), std::size_t(1));

    // New loans must not reuse ids from the previous run.
    CHECK(reopened.issueBook(102, 2));
    const std::vector<Transaction> active = reopened.activeTransactions();
    bool idsUnique = true;
    for (std::size_t i = 1; i < active.size(); ++i) {
        if (active[i].getTransactionId() == active[i - 1].getTransactionId()) {
            idsUnique = false;
        }
    }
    CHECK(idsUnique);
}

TEST(LibraryStartsCleanWithNoDataFiles) {
    // Removing every file must not break start-up.
    std::remove(Config::BOOKS_FILE);
    std::remove(Config::MEMBERS_FILE);
    std::remove(Config::TRANSACTIONS_FILE);

    resetData();
    Library lib;
    CHECK(lib.initialise());
    CHECK(lib.allBooks().empty());
    CHECK(lib.allMembers().empty());
    CHECK(lib.activeTransactions().empty());
    CHECK(lib.addBook(sampleBooks()[0]));   // still usable afterwards
}

TEST(LibraryIgnoresCorruptedDataFile) {
    {
        // Write something that is definitely not a book record.
        std::ofstream junk(Config::BOOKS_FILE, std::ios::binary | std::ios::trunc);
        for (int i = 0; i < 64; ++i) junk.put(static_cast<char>(0x7F));
    }

    resetData();
    Library lib;
    CHECK(lib.initialise());          // must not crash or throw
    CHECK(lib.allBooks().empty());

    // The application must still be able to write afterwards.
    CHECK(lib.addBook(sampleBooks()[0]));
    CHECK(lib.saveAll());
}

// ---------------------------------------------------------------------------
// Reports
// ---------------------------------------------------------------------------
TEST(ReportsMatchLiveData) {
    std::vector<Book>        books = sampleBooks();
    std::vector<Member>      members = sampleMembers();
    std::vector<Transaction> transactions;

    books[0].setAvailableCopies(2);   // 1 copy on loan
    members[0].setIssuedBooks(1);

    Transaction issued(Config::TRANSACTION_ID_START, 101, 1,
                       Utils::addDays(Utils::today(), -20),
                       Utils::addDays(Utils::today(), -5));
    issued.setStatus(Transaction::Status::OVERDUE);
    issued.setLateFee(25.0);
    transactions.push_back(issued);

    ReportManager rm(books, members, transactions);
    const ReportManager::Summary& s = rm.summary();

    CHECK_EQ(s.totalBooks, std::size_t(3));
    CHECK_EQ(s.totalCopies, std::size_t(9));
    CHECK_EQ(s.availableCopies, std::size_t(8));
    CHECK_EQ(s.issuedCopies, std::size_t(1));
    CHECK_EQ(s.totalMembers, std::size_t(2));
    CHECK_EQ(s.activeLoans, std::size_t(1));
    CHECK_EQ(s.overdueLoans, std::size_t(1));
    CHECK(s.totalLateFees > 24.9 && s.totalLateFees < 25.1);
}

TEST(ReportsRenderToStream) {
    std::vector<Book>        books = sampleBooks();
    std::vector<Member>      members = sampleMembers();
    std::vector<Transaction> transactions;

    ReportManager rm(books, members, transactions);
    std::ostringstream out;

    rm.printSummary(out);
    rm.printOverdue(out);
    rm.printRecentlyIssued(out, 5);
    rm.printBorrowingHistory(1, out);

    const std::string text = out.str();
    CHECK(text.find("LIBRARY SUMMARY REPORT") != std::string::npos);
    CHECK(text.find("OVERDUE BOOKS") != std::string::npos);
    CHECK(text.find("RECENTLY ISSUED BOOKS") != std::string::npos);
    CHECK(text.find("BORROWING HISTORY") != std::string::npos);
    CHECK(text.find("(no overdue loans)") != std::string::npos);
}

TEST(ReportRefreshPicksUpChanges) {
    std::vector<Book>        books = sampleBooks();
    std::vector<Member>      members = sampleMembers();
    std::vector<Transaction> transactions;

    ReportManager rm(books, members, transactions);
    CHECK_EQ(rm.summary().totalBooks, std::size_t(3));

    books.emplace_back(104, "New Arrival", "Someone", "Cat", "", "2026", 1, 1);
    rm.refresh();
    CHECK_EQ(rm.summary().totalBooks, std::size_t(4));
}