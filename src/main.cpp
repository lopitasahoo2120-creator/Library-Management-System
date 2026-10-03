// ---------------------------------------------------------------------------
// main.cpp - terminal front-end and system-integration entry point.
//
// Responsibilities beyond drawing menus:
//   * install the SIGINT / SIGTERM handlers and honour them between actions
//   * start the monitor child process and feed it events over a real pipe
//   * create the FIFO and run a reader thread that forwards external audit
//     commands to the monitor process and to the kernel driver
//   * drive the ThreadManager pool used for that forwarding work
//
// Ctrl+C never aborts mid-write: the handler only flips a flag, and this
// file checks it at every safe point so shutdown can save data and join
// threads properly.
// ---------------------------------------------------------------------------

#include "Authentication.h"
#include "Book.h"
#include "Library.h"
#include "Logger.h"
#include "Member.h"
#include "ReportManager.h"
#include "Transaction.h"

#include "Config.h"
#include "Utils.h"

#include "system/ipc.h"
#include "system/process_manager.h"
#include "system/signal_handler.h"
#include "system/thread_manager.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <poll.h>
#include <string>
#include <termios.h>
#include <unistd.h>
#include <vector>

// ---------------------------------------------------------------------------
// Small input helpers (all built on getline so the stream never desyncs)
// ---------------------------------------------------------------------------
namespace {

SignalHandler   g_signals;
ProcessManager  g_monitor;
ThreadManager   g_pool(2);          // one FIFO reader + one spare worker
std::atomic<bool> g_endOfInput{false};   // stdin closed, not a signal

// ---------------------------------------------------------------------------
// LineReader - signal-aware line input.
//
// Why not plain std::getline?  Because libstdc++ retries a read(2) that was
// interrupted by a signal (errno == EINTR), so a Ctrl+C at a prompt would set
// our handler flag but leave the program blocked inside getline() forever.
// Polling stdin with a short timeout lets the main loop notice the flag and
// shut down gracefully, which is exactly what the specification asks for.
//
// In canonical mode the terminal already echoes what the user types, so the
// reader only has to collect bytes until '\n'.
// ---------------------------------------------------------------------------
class LineReader {
public:
    bool readLine(std::string& out) {
        out.clear();

        for (;;) {
            if (!pending_.empty()) {
                if (consumePending(out)) return true;
            }

            // A termination signal must win over waiting for more input.
            if (g_signals.shouldStop()) return false;

            struct pollfd pfd{};
            pfd.fd     = STDIN_FILENO;
            pfd.events = POLLIN;

            const int rc = ::poll(&pfd, 1, 250);
            if (rc < 0) {
                if (errno == EINTR) continue;
                return false;
            }
            if (rc == 0) continue;                     // timeout: re-check stop

            char    chunk[256];
            const ssize_t n = ::read(STDIN_FILENO, chunk, sizeof(chunk));
            if (n < 0) {
                if (errno == EINTR) continue;
                return false;
            }
            if (n == 0) return false;                  // EOF / Ctrl+D

            pending_.append(chunk, static_cast<std::size_t>(n));
        }
    }

private:
    // Splits whatever is buffered; returns true once a full line is in `out`.
    bool consumePending(std::string& out) {
        for (;;) {
            const std::size_t nl = pending_.find('\n');
            if (nl == std::string::npos) {
                out += pending_;
                pending_.clear();
                return false;
            }
            out += pending_.substr(0, nl);
            pending_.erase(0, nl + 1);
            return true;
        }
    }

    std::string pending_;
};

LineReader g_input;

void banner(const std::string& title) {
    std::cout << "\n" << std::string(64, '=') << "\n  " << title
              << "\n" << std::string(64, '=') << "\n";
}

std::string ask(const std::string& prompt) {
    std::cout << prompt;
    std::cout.flush();

    std::string line;
    if (!g_input.readLine(line)) {
        if (!g_signals.shouldStop()) {
            // EOF (piped input finished, or the user pressed Ctrl+D) is a
            // normal way to end a session - it is not a signal.
            g_endOfInput.store(true);
            g_signals.reset();
        }
        return std::string();
    }
    return Utils::trim(line);
}

// Reads a line with terminal echo disabled, so the password never reaches the
// screen or the scroll-back buffer.  Same poll loop, so Ctrl+C still works.
std::string askPassword(const std::string& prompt) {
    std::cout << prompt;
    std::cout.flush();

    std::string pass;

#ifdef _WIN32
    g_input.readLine(pass);
#else
    struct termios saved {};
    bool haveSaved = (::tcgetattr(STDIN_FILENO, &saved) == 0);

    if (haveSaved) {
        struct termios quiet = saved;
        quiet.c_lflag &= ~static_cast<tcflag_t>(ECHO);
        ::tcsetattr(STDIN_FILENO, TCSANOW, &quiet);
    }

    g_input.readLine(pass);

    if (haveSaved) {
        ::tcsetattr(STDIN_FILENO, TCSANOW, &saved);
    }
#endif

    std::cout << "\n";
    return pass;
}

int askInt(const std::string& prompt) {
    while (true) {
        const std::string line = ask(prompt);
        if (line.empty()) return -1;
        try {
            std::size_t idx = 0;
            const int value = std::stoi(line, &idx);
            if (idx == line.size()) return value;
        } catch (...) {
            // not a number - fall through
        }
        std::cout << "  Please enter a whole number.\n";
    }
}

// Every menu reads with the poll-based reader, so the pause must use it too.
void pause() {
    std::string ignored;
    std::cout << "\n  Press Enter to continue...";
    std::cout.flush();
    g_input.readLine(ignored);
}

// A refused action must look exactly like a completed one: message, then the
// usual pause.
//
// This exists because the pause used to sit only at the bottom of the menu
// loop, so every `continue` on a validation failure skipped it.  The menu then
// redrew immediately with no "Press Enter to continue", the user's next
// keystroke was consumed by the redrawn "Select:" prompt, and the rest of the
// menu session shifted by one prompt - which looked exactly like the book
// having been lost.  Every early-exit path in the menus goes through here.
void reject(const std::string& message) {
    std::cout << message;
    pause();
}

// True when the user asked to leave the current screen.
bool wantsBack(const std::string& answer) {
    return answer == "0" || Utils::toLower(answer) == "back" ||
           Utils::toLower(answer) == "q";
}

// ---------------------------------------------------------------------------
// Display helpers
// ---------------------------------------------------------------------------
void printBookRow(const Book& b) {
    std::cout << "  ID " << std::setw(4) << b.getId() << " | "
              << std::left << std::setw(28) << b.getTitle()
              << std::setw(18) << b.getAuthor()
              << std::setw(12) << b.getCategory()
              << "ISBN " << b.getIsbn()
              << " | " << b.getAvailableCopies() << "/" << b.getTotalCopies()
              << " available\n";
}

void printMemberRow(const Member& m) {
    std::cout << "  ID " << std::setw(4) << m.getMemberId() << " | "
              << std::left << std::setw(24) << m.getName()
              << std::setw(26) << m.getEmail()
              << std::setw(14) << m.getDepartment()
              << " | books " << m.getIssuedBooks() << "/"
              << Config::MAX_BOOKS_PER_MEMBER << "\n";
}

void printTransactionRow(const Transaction& t) {
    std::cout << "  Tx #" << std::setw(5) << t.getTransactionId() << " | "
              << std::left << std::setw(4) << t.getBookId()
              << std::setw(4) << t.getMemberId()
              << "issued " << std::setw(10) << t.getIssueDate()
              << "due " << std::setw(10) << t.getDueDate()
              << "ret " << std::setw(10)
              << (t.getReturnDate().empty() ? "-" : t.getReturnDate())
              << " | " << std::setw(8)
              << Transaction::statusToString(t.getStatus())
              << " | fee " << std::fixed << std::setprecision(2)
              << t.getLateFee() << "\n";
}

// ---------------------------------------------------------------------------
// Integration status line
// ---------------------------------------------------------------------------
void printIntegrationStatus(Library& lib) {
    banner("SYSTEM INTEGRATION STATUS");

    std::cout << "  Kernel driver   : ";
    if (lib.driverAvailable()) {
        std::string report;
        std::cout << lib.driver().devicePath() << " (loaded)";
        if (lib.driver().readReport(report)) {
            std::cout << "\n  Driver report   : " << report;
        }
    } else {
        std::cout << Config::DRIVER_DEVICE
                  << " not available - running without kernel events\n"
                  << "                   (build and insmod driver/library_driver.ko)";
    }

    std::cout << "  Monitor process : ";
    if (g_monitor.isRunning()) {
        std::cout << "running, child pid " << g_monitor.childPid()
                  << " (parent pid " << ::getpid() << ")\n";
    } else {
        std::cout << "not running\n";
    }

    std::cout << "  IPC FIFO        : " << Config::IPC_FIFO_PATH << "\n"
              << "  Worker threads  : " << g_pool.pendingTasks()
              << " task(s) pending\n"
              << "  Application pid : " << ::getpid() << "\n"
              << "  Data files      : " << Config::BOOKS_FILE << ", "
              << Config::MEMBERS_FILE << ", " << Config::TRANSACTIONS_FILE
              << "\n  Log file        : " << Config::LOG_FILE << "\n";
}

// ---------------------------------------------------------------------------
// FIFO reader thread
//
// Runs for the lifetime of the program.  poll(2) with a short timeout lets it
// exit promptly on shutdown instead of blocking forever in read(2).
// ---------------------------------------------------------------------------
class FifoReader {
public:
    FifoReader(Library& lib, IpcPipe& fifo, const std::atomic<bool>& stop)
        : lib_(lib), fifo_(fifo), stop_(stop) {}

    void run() {
        std::string buffer;
        char        chunk[512];

        while (!stop_.load()) {
            struct pollfd pfd{};
            pfd.fd     = fifo_.fd();
            pfd.events = POLLIN;

            const int rc = ::poll(&pfd, 1, 200);
            if (rc < 0) {
                if (errno == EINTR) continue;
                break;
            }
            if (rc == 0) continue;               // timeout - re-check stop
            if (!(pfd.revents & (POLLIN | POLLHUP))) continue;

            const ssize_t n = fifo_.readData(chunk, sizeof(chunk));
            if (n <= 0) {
                if (n < 0 && errno == EINTR) continue;
                // n == 0 with POLLHUP: no writer yet, just wait again.
                continue;
            }

            buffer.append(chunk, static_cast<std::size_t>(n));

            std::size_t nl;
            while ((nl = buffer.find('\n')) != std::string::npos) {
                const std::string line = buffer.substr(0, nl);
                buffer.erase(0, nl + 1);

                const std::string command = Utils::trim(line);
                if (command.empty() || command[0] == '#') continue;   // '#' = comment

                // External audit command, e.g. "SCAN member:1".
                g_monitor.sendEvent("IPC " + command);
                lib_.logger().log(Logger::Event::IPC_EVENT,
                                  "FIFO command received: " + command);

                // Mirror it into the kernel driver too.
                lib_.notifyDriver(command);

                std::cout << "\n[IPC] forwarded external command: " << command
                          << "\n";
            }
        }
    }

private:
    Library&                lib_;
    IpcPipe&                fifo_;
    const std::atomic<bool>& stop_;
};

// ---------------------------------------------------------------------------
// Login
// ---------------------------------------------------------------------------
bool loginFlow(Library& lib) {
    banner("ADMIN LOGIN");

    for (int attempt = 1; attempt <= Config::MAX_LOGIN_ATTEMPTS; ++attempt) {
        std::cout << "  Attempt " << attempt << " of "
                  << Config::MAX_LOGIN_ATTEMPTS << "\n";

        const std::string user = ask("  Username: ");
        const std::string pass = askPassword("  Password: ");
        std::cout << "\n";

        if (user.empty() || pass.empty()) {
            std::cout << "  Username and password are both required.\n";
            continue;
        }

        if (lib.authenticate(user, pass)) {
            std::cout << "\n  Welcome, " << user << ".\n";
            return true;
        }

        std::cout << "  Invalid credentials.\n";
    }

    std::cout << "\n  Too many failed attempts. Locked out for this session.\n";
    lib.logger().log(Logger::Event::ERROR,
                     "Login lockout after " +
                         std::to_string(Config::MAX_LOGIN_ATTEMPTS) +
                         " failed attempts");
    return false;
}

// ---------------------------------------------------------------------------
// Book menu
// ---------------------------------------------------------------------------
void bookMenu(Library& lib) {
    while (!g_signals.shouldStop()) {
        banner("BOOK MANAGEMENT");
        std::cout << "  1. Add book\n"
                  << "  2. Remove book\n"
                  << "  3. Update book\n"
                  << "  4. View all books\n"
                  << "  5. View one book\n"
                  << "  6. Availability report\n"
                  << "  0. Back\n";

        const std::string choice = ask("  Select: ");

        if (wantsBack(choice)) return;

        try {
            if (choice == "1") {
                const int id = askInt("  Book id       : ");
                if (id <= 0) {
                    reject("  Book id must be a positive whole number. "
                           "Nothing was added.\n");
                    continue;
                }

                // Title is mandatory: Book's constructor throws on an empty
                // one, so it is checked here to keep the message readable.
                const std::string title = ask("  Title         : ");
                if (title.empty()) {
                    reject("  Title cannot be empty. Nothing was added.\n");
                    continue;
                }

                const std::string author = ask("  Author        : ");
                const std::string cat    = ask("  Category      : ");
                const std::string isbn   = ask("  ISBN (10/13)  : ");

                // Validated here, at the field that is actually wrong, so the
                // error lands on the ISBN prompt instead of after the user has
                // already typed the remaining fields.
                if (!Utils::isValidISBN(isbn)) {
                    reject("  Invalid ISBN: it must be exactly 10 or 13 digits\n"
                           "  (hyphens and spaces are ignored). Nothing was added.\n");
                    continue;
                }

                const std::string pub    = ask("  Publication   : ");
                const int copies = askInt("  Total copies  : ");

                if (copies <= 0) {
                    reject("  Total copies must be at least 1. "
                           "Nothing was added.\n");
                    continue;
                }

                // Built only after every field passed, so no partially valid
                // Book object is ever constructed or inserted.
                const Book b(id, title, author, cat, isbn, pub, copies, copies);

                switch (lib.addBookChecked(b)) {
                    case Library::AddResult::Added:
                        std::cout << "  Book added with id " << id
                                  << " (" << copies << " cop"
                                  << (copies == 1 ? "y" : "ies") << ").\n";
                        break;
                    case Library::AddResult::DuplicateId:
                        reject("  A book with id " + std::to_string(id) +
                               " already exists. Use Update book to change it.\n");
                        continue;
                    case Library::AddResult::InvalidRecord:
                    default:
                        reject("  The book details were rejected as invalid. "
                               "Nothing was added.\n");
                        continue;
                }

            } else if (choice == "2") {
                const int id = askInt("  Book id to remove: ");
                if (id < 0) {
                    reject("  Book id must be a positive whole number.\n");
                    continue;
                }
                std::cout << (lib.removeBook(id)
                                  ? "  Book removed.\n"
                                  : "  Could not remove book (issued or unknown).\n");

            } else if (choice == "3") {
                const int id = askInt("  Book id to update: ");
                if (id < 0) {
                    reject("  Book id must be a positive whole number.\n");
                    continue;
                }
                const Book* existing = lib.findBook(id);
                if (existing == nullptr) {
                    reject("  No book with id " + std::to_string(id) +
                           " exists.\n");
                    continue;
                }

                std::cout << "  Press Enter to keep the current value.\n";
                const std::string title =
                    ask("  Title         [" + existing->getTitle() + "]: ");
                const std::string author =
                    ask("  Author        [" + existing->getAuthor() + "]: ");
                const std::string cat =
                    ask("  Category      [" + existing->getCategory() + "]: ");
                const std::string pub =
                    ask("  Publication   [" + existing->getPublication() + "]: ");
                const std::string copiesLine =
                    ask("  Total copies  [" + std::to_string(existing->getTotalCopies()) + "]: ");

                int copies = existing->getTotalCopies();
                if (!copiesLine.empty()) {
                    std::size_t idx = 0;
                    try {
                        copies = std::stoi(copiesLine, &idx);
                    } catch (...) {
                        copies = existing->getTotalCopies();
                    }
                }

                // Availability is derived, never typed by hand.
                const int outstanding = existing->getTotalCopies() -
                                        existing->getAvailableCopies();
                const int available = copies - outstanding;

                if (copies < 1) {
                    reject("  Total copies must be at least 1. "
                           "Nothing was changed.\n");
                    continue;
                }
                if (available < 0) {
                    reject("  " + std::to_string(outstanding) +
                           (outstanding == 1 ? " copy is" : " copies are") +
                           " still on loan, so the total cannot be lower than " +
                           std::to_string(outstanding) +
                           ". Nothing was changed.\n");
                    continue;
                }

                Book updated(id,
                             title.empty() ? existing->getTitle() : title,
                             author.empty() ? existing->getAuthor() : author,
                             cat.empty() ? existing->getCategory() : cat,
                             existing->getIsbn(),
                             pub.empty() ? existing->getPublication() : pub,
                             copies, available);
                std::cout << (lib.updateBook(updated) ? "  Book updated.\n"
                                                      : "  Update rejected (see log).\n");

            } else if (choice == "4") {
                const auto& all = lib.allBooks();
                std::cout << "\n";
                for (const Book& b : all) printBookRow(b);
                std::cout << "  Total: " << all.size() << " title(s)\n";

            } else if (choice == "5") {
                const int id = askInt("  Book id: ");
                if (id < 0) {
                    reject("  Book id must be a positive whole number.\n");
                    continue;
                }
                const Book* b = lib.findBook(id);
                if (b == nullptr) {
                    reject("  No book with id " + std::to_string(id) +
                           " exists.\n");
                } else {
                    std::cout << "\n";
                    b->display(std::cout);
                }

            } else if (choice == "6") {
                std::cout << "\n  TITLE                          AVAIL / TOTAL\n";
                for (const Book& b : lib.allBooks()) {
                    std::cout << "  " << std::left << std::setw(30) << b.getTitle()
                              << b.getAvailableCopies() << " / "
                              << b.getTotalCopies();
                    if (b.getAvailableCopies() == 0) std::cout << "   (none)";
                    std::cout << "\n";
                }
            }
        } catch (const std::exception& e) {
            std::cout << "  Error: " << e.what() << "\n";
            lib.logger().log(Logger::Event::ERROR,
                             std::string("Book menu error: ") + e.what());
        }

        if (!choice.empty()) pause();
    }
}

// ---------------------------------------------------------------------------
// Member menu
// ---------------------------------------------------------------------------
void memberMenu(Library& lib) {
    while (!g_signals.shouldStop()) {
        banner("MEMBER MANAGEMENT");
        std::cout << "  1. Add member\n"
                  << "  2. Remove member\n"
                  << "  3. Update member\n"
                  << "  4. View all members\n"
                  << "  5. View one member\n"
                  << "  6. Borrowing history\n"
                  << "  0. Back\n";

        const std::string choice = ask("  Select: ");
        if (wantsBack(choice)) return;

        try {
            if (choice == "1") {
                const int id   = askInt("  Member id      : ");
                if (id <= 0) {
                    reject("  Member id must be a positive whole number. "
                           "Nothing was added.\n");
                    continue;
                }
                const std::string name = ask("  Name           : ");
                const std::string mail = ask("  Email          : ");
                const std::string tel  = ask("  Phone          : ");
                const std::string dept = ask("  Department     : ");

                if (name.empty()) {
                    reject("  Name cannot be empty. Nothing was added.\n");
                    continue;
                }
                if (!Utils::isValidEmail(mail)) {
                    reject("  Invalid email address. Nothing was added.\n");
                    continue;
                }
                if (!Utils::isValidPhone(tel)) {
                    reject("  Invalid phone number. Nothing was added.\n");
                    continue;
                }

                const Member m(id, name, mail, tel, dept, 0);
                std::cout << (lib.addMember(m) ? "  Member added.\n"
                                               : "  Could not add member (see log).\n");

            } else if (choice == "2") {
                const int id = askInt("  Member id to remove: ");
                if (id < 0) {
                    reject("  Member id must be a positive whole number.\n");
                    continue;
                }
                std::cout << (lib.removeMember(id)
                                  ? "  Member removed.\n"
                                  : "  Could not remove member (has loans or unknown).\n");

            } else if (choice == "3") {
                const int id = askInt("  Member id to update: ");
                if (id < 0) {
                    reject("  Member id must be a positive whole number.\n");
                    continue;
                }
                const Member* existing = lib.findMember(id);
                if (existing == nullptr) {
                    reject("  No member with id " + std::to_string(id) +
                           " exists.\n");
                    continue;
                }

                const std::string name =
                    ask("  Name       [" + existing->getName() + "]: ");
                const std::string mail =
                    ask("  Email      [" + existing->getEmail() + "]: ");
                const std::string tel =
                    ask("  Phone      [" + existing->getPhone() + "]: ");
                const std::string dept =
                    ask("  Department [" + existing->getDepartment() + "]: ");

                if (!mail.empty() && !Utils::isValidEmail(mail)) {
                    reject("  Invalid email address. Nothing was changed.\n");
                    continue;
                }

                // The issued count is state, not user input.
                Member updated(id,
                               name.empty() ? existing->getName() : name,
                               mail.empty() ? existing->getEmail() : mail,
                               tel.empty() ? existing->getPhone() : tel,
                               dept.empty() ? existing->getDepartment() : dept,
                               existing->getIssuedBooks());
                std::cout << (lib.updateMember(updated) ? "  Member updated.\n"
                                                       : "  Update rejected (see log).\n");

            } else if (choice == "4") {
                const auto& all = lib.allMembers();
                std::cout << "\n";
                for (const Member& m : all) printMemberRow(m);
                std::cout << "  Total: " << all.size() << " member(s)\n";

            } else if (choice == "5") {
                const int id = askInt("  Member id: ");
                if (id < 0) {
                    reject("  Member id must be a positive whole number.\n");
                    continue;
                }
                const Member* m = lib.findMember(id);
                if (m == nullptr) {
                    reject("  No member with id " + std::to_string(id) +
                           " exists.\n");
                } else {
                    std::cout << "\n";
                    m->display(std::cout);
                }

            } else if (choice == "6") {
                const int id = askInt("  Member id: ");
                if (id < 0) continue;
                lib.printMemberHistory(id);
            }
        } catch (const std::exception& e) {
            std::cout << "  Error: " << e.what() << "\n";
            lib.logger().log(Logger::Event::ERROR,
                             std::string("Member menu error: ") + e.what());
        }

        if (!choice.empty()) pause();
    }
}

// ---------------------------------------------------------------------------
// Issue / return menu
// ---------------------------------------------------------------------------
void transactionMenu(Library& lib) {
    while (!g_signals.shouldStop()) {
        banner("ISSUE / RETURN");
        std::cout << "  1. Issue a book\n"
                  << "  2. Return a book\n"
                  << "  3. List active loans\n"
                  << "  4. List all transactions\n"
                  << "  0. Back\n";

        const std::string choice = ask("  Select: ");
        if (wantsBack(choice)) return;

        if (choice == "1") {
            const int bookId   = askInt("  Book id   : ");
            const int memberId = askInt("  Member id : ");
            if (bookId < 0 || memberId < 0) {
                reject("  Book id and member id must be positive whole "
                       "numbers. Nothing was issued.\n");
                continue;
            }

            std::cout << (lib.issueBook(bookId, memberId)
                              ? "  Book issued. Due date is 14 days from today.\n"
                              : "  Issue refused (see log for the reason).\n");

        } else if (choice == "2") {
            const int txnId = askInt("  Transaction id: ");
            if (txnId < 0) {
                reject("  Transaction id must be a positive whole number.\n");
                continue;
            }

            const Transaction* t = lib.findTransaction(txnId);
            if (t == nullptr) {
                reject("  No transaction with id " + std::to_string(txnId) +
                       " exists.\n");
            } else {
                std::cout << "\n";
                printTransactionRow(*t);
                const std::string confirm =
                    ask("  Confirm return (y/n): ");
                if (Utils::toLower(confirm) != "y") {
                    std::cout << "  Cancelled.\n";
                } else {
                    std::cout << (lib.returnBook(txnId)
                                      ? "  Book returned; late fee applied if overdue.\n"
                                      : "  Return refused (see log).\n");
                }
            }

        } else if (choice == "3") {
            const auto active = lib.activeTransactions();
            std::cout << "\n";
            for (const Transaction& t : active) printTransactionRow(t);
            std::cout << "  Active loans: " << active.size() << "\n";

        } else if (choice == "4") {
            std::cout << "\n";
            for (const Transaction& t : lib.activeTransactions()) {
                printTransactionRow(t);
            }
            std::cout << "  (active loans only; use a member id in Member "
                         "Management -> Borrowing history for the rest)\n";
        }

        if (!choice.empty()) pause();
    }
}

// ---------------------------------------------------------------------------
// Search menu
// ---------------------------------------------------------------------------
void searchMenu(Library& lib) {
    while (!g_signals.shouldStop()) {
        banner("SEARCH");
        std::cout << "  1. Search books\n"
                  << "  2. Search members\n"
                  << "  0. Back\n";

        const std::string choice = ask("  Select: ");
        if (wantsBack(choice)) return;

        if (choice == "1") {
            const std::string query = ask("  Search text : ");
            const std::string field = ask("  Field (title/author/category/isbn/all): ");
            const auto hits = lib.searchBooks(query, field);

            std::cout << "\n";
            for (const Book& b : hits) printBookRow(b);
            std::cout << "  " << hits.size() << " match(es)\n";

        } else if (choice == "2") {
            const std::string query = ask("  Search text (name/email/phone/department): ");
            const auto hits = lib.searchMembers(query);

            std::cout << "\n";
            for (const Member& m : hits) printMemberRow(m);
            std::cout << "  " << hits.size() << " match(es)\n";
        }

        if (!choice.empty()) pause();
    }
}

// ---------------------------------------------------------------------------
// Reports menu
// ---------------------------------------------------------------------------
void reportMenu(Library& lib) {
    while (!g_signals.shouldStop()) {
        banner("REPORTS");
        std::cout << "  1. Summary\n"
                  << "  2. Overdue books\n"
                  << "  3. Recently issued\n"
                  << "  4. Borrowing history (member id)\n"
                  << "  0. Back\n";

        const std::string choice = ask("  Select: ");
        if (wantsBack(choice)) return;

        if (choice == "1") {
            lib.printSummary();
        } else if (choice == "2") {
            lib.printOverdue();
        } else if (choice == "3") {
            lib.printRecentlyIssued();
        } else if (choice == "4") {
            const int id = askInt("  Member id: ");
            if (id >= 0) lib.printMemberHistory(id);
        }

        if (!choice.empty()) pause();
    }
}

// ---------------------------------------------------------------------------
// Password change
// ---------------------------------------------------------------------------
void passwordMenu(Library& lib) {
    banner("CHANGE ADMIN PASSWORD");
    const std::string user = ask("  Username     : ");
    const std::string oldP = askPassword("  Current pass : ");
    const std::string newP = askPassword("  New pass     : ");
    const std::string cnf = askPassword("  Confirm pass : ");
    std::cout << "\n";

    if (newP != cnf) {
        std::cout << "  New passwords do not match.\n";
        return;
    }
    std::cout << (lib.changePassword(user, oldP, newP) ? "  Password changed.\n"
                                                       : "  Password change failed.\n");
}

// ---------------------------------------------------------------------------
// Main menu
// ---------------------------------------------------------------------------
void mainMenu(Library& lib) {
    while (!g_signals.shouldStop()) {
        // A SIGUSR1 ("kill -USR1 <pid>") prints a report without disturbing
        // the current screen state.
        g_signals.runUsr1Callback();

        banner("LIBRARY MANAGEMENT SYSTEM");
        std::cout << "  1. Book management\n"
                  << "  2. Member management\n"
                  << "  3. Issue / return\n"
                  << "  4. Search\n"
                  << "  5. Reports\n"
                  << "  6. System integration status\n"
                  << "  7. Change admin password\n"
                  << "  8. Save data now\n"
                  << "  9. Logout\n"
                  << "  0. Exit\n";

        const std::string choice = ask("  Select: ");
        if (choice.empty() || wantsBack(choice)) return;

        if (choice == "1")      bookMenu(lib);
        else if (choice == "2") memberMenu(lib);
        else if (choice == "3") transactionMenu(lib);
        else if (choice == "4") searchMenu(lib);
        else if (choice == "5") reportMenu(lib);
        else if (choice == "6") printIntegrationStatus(lib);
        else if (choice == "7") passwordMenu(lib);
        else if (choice == "8") {
            std::cout << (lib.saveAll() ? "  Data saved to disk.\n"
                                        : "  Save failed (see log).\n");
        } else if (choice == "9") {
            lib.shutdown();
            std::cout << "\n  Logged out. Data saved.\n";
            return;
        }
    }
}

// ---------------------------------------------------------------------------
// startSubsystems() - pipe + monitor child + FIFO + reader thread
// ---------------------------------------------------------------------------
std::atomic<bool> g_fifoStop{false};
IpcPipe           g_fifo;

void startSubsystems(Library& lib) {
    // ---- 1. anonymous pipe -> monitor child process ---------------------
    int fds[2] = {-1, -1};
    if (::pipe(fds) == 0) {
        if (g_monitor.startLoggingProcess(fds[0], fds[1]) > 0) {
            std::cout << "  Monitor process started (child pid "
                      << g_monitor.childPid() << ").\n";
            // Business events now also reach the child process.
            lib.attachMonitor(&g_monitor);
            g_monitor.sendEvent("MONITOR_START parent=" +
                                std::to_string(::getpid()));
        } else {
            ::close(fds[0]);
            ::close(fds[1]);
            std::cout << "  Warning: could not fork the monitor process.\n";
        }
    } else {
        std::cout << "  Warning: pipe() failed (" << std::strerror(errno)
                  << "); monitor process disabled.\n";
    }

    // ---- 2. named FIFO for external audit commands ----------------------
    if (IpcPipe::create(Config::IPC_FIFO_PATH, 0666) &&
        g_fifo.open(Config::IPC_FIFO_PATH, IpcPipe::Mode::READ, true)) {
        g_pool.enqueue([&lib]() { FifoReader(lib, g_fifo, g_fifoStop).run(); });
        std::cout << "  IPC FIFO ready at " << Config::IPC_FIFO_PATH << "\n";
    } else {
        std::cout << "  Warning: IPC FIFO unavailable; external audit commands "
                     "disabled.\n";
    }
}

void stopSubsystems() {
    g_fifoStop.store(true);
    g_pool.shutdown();       // joins the FIFO reader thread
    g_fifo.close();
    g_monitor.stopLoggingProcess();   // sends STOP, waitpid()s the child
    IpcPipe::remove(Config::IPC_FIFO_PATH);
}

} // namespace

// ---------------------------------------------------------------------------
// main()
// ---------------------------------------------------------------------------
int main() {
    g_signals.install();

    std::cout << "\n"
              << "============================================================\n"
              << "  LIBRARY MANAGEMENT SYSTEM\n"
              << "  Linux system programming capstone\n"
              << "  pid " << ::getpid() << ", ppid " << ::getppid() << "\n"
              << "============================================================\n";

    Library lib;
    lib.initialise();

    if (!lib.driverAvailable()) {
        std::cout << "\n  Warning: Library driver unavailable. "
                     "Continuing without kernel event reporting.\n";
    }

    startSubsystems(lib);

    if (loginFlow(lib)) {
        mainMenu(lib);
    }

    std::cout << "\n  Shutting down...\n";
    stopSubsystems();

    if (g_signals.shouldStop() && !g_endOfInput.load()) {
        std::cout << "  Termination signal received - data saved before exit.\n";
        lib.logger().log(Logger::Event::SYSTEM_SHUTDOWN,
                         "Graceful shutdown after signal");
    } else if (g_endOfInput.load()) {
        std::cout << "  End of input received - data saved before exit.\n";
    }

    lib.saveAll();
    lib.shutdown();
    std::cout << "  Goodbye.\n";
    return 0;
}