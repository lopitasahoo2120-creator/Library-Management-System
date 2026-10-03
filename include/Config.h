#ifndef LIBRARY_CONFIG_H
#define LIBRARY_CONFIG_H

// Library Management System - Configuration Constants
// All magic numbers are centralized here for easy modification.

namespace Config {

    // ---- Loan / Late-fee policy ----
    constexpr int    DEFAULT_LOAN_DAYS     = 14;   // how long a member may keep a book
    constexpr double LATE_FEE_PER_DAY     = 5.0;  // rupees/dollars per overdue day
    constexpr int    MAX_BOOKS_PER_MEMBER  = 5;    // cap on simultaneous loans

    // ---- File storage paths ----
    constexpr const char* DATA_DIR        = "data";
    constexpr const char* BOOKS_FILE      = "data/books.dat";
    constexpr const char* MEMBERS_FILE    = "data/members.dat";
    constexpr const char* TRANSACTIONS_FILE = "data/transactions.dat";
    constexpr const char* ADMIN_FILE      = "data/admin.dat";
    constexpr const char* LOG_FILE        = "logs/library.log";

    // ---- Driver ----
    constexpr const char* DRIVER_DEVICE   = "/dev/library_driver";

    // ---- IPC ----
    constexpr const char* IPC_FIFO_PATH   = "/tmp/library_ipc_fifo";
    constexpr int         IPC_MAX_MESSAGE  = 256;

    // ---- Logging queue ----
    constexpr size_t LOG_QUEUE_SIZE       = 256;

    // ---- Authentication ----
    constexpr int MAX_LOGIN_ATTEMPTS      = 3;

    // ---- Transaction ID start ----
    constexpr int TRANSACTION_ID_START    = 1000;

    // ---- Report pagination ----
    constexpr int RECENT_ISSUED_COUNT     = 10;

} // namespace Config

#endif // LIBRARY_CONFIG_H