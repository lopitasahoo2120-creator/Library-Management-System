#ifndef LIBRARY_UTILS_H
#define LIBRARY_UTILS_H

#include <string>
#include <iostream>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <cctype>
#include <ctime>
#include <limits>

#ifdef _WIN32
#include <windows.h>
#else
#include <termios.h>
#include <unistd.h>
#endif

// ---------------------------------------------------------------------------
// Utils - reusable input-validation helpers.
// Centralised so validation logic is never duplicated across menus.
// ---------------------------------------------------------------------------
class Utils {
public:
    // ---- string helpers -------------------------------------------------
    static std::string trim(const std::string& s) {
        auto wsfront = std::find_if_not(s.begin(), s.end(), [](int c){ return std::isspace(c); });
        auto wsback  = std::find_if_not(s.rbegin(), s.rend(), [](int c){ return std::isspace(c); }).base();
        return (wsback <= wsfront ? std::string() : std::string(wsfront, wsback));
    }

    static std::string toLower(const std::string& s) {
        std::string out = s;
        std::transform(out.begin(), out.end(), out.begin(),
                       [](unsigned char c){ return std::tolower(c); });
        return out;
    }

    static bool containsIgnoreCase(const std::string& haystack,
                                   const std::string& needle) {
        if (needle.empty()) return true;
        auto it = std::search(haystack.begin(), haystack.end(),
                              needle.begin(), needle.end(),
                              [](char ch1, char ch2) {
                                  return std::toupper(static_cast<unsigned char>(ch1))
                                       == std::toupper(static_cast<unsigned char>(ch2));
                              });
        return it != haystack.end();
    }

    // ---- numeric input --------------------------------------------------
    // Reads an integer from cin.  On failure, clears the stream and returns
    // false so the caller can re-prompt without crashing.
    static bool readInt(int& out, const char* prompt = nullptr) {
        if (prompt) std::cout << prompt;
        std::string line;
        if (!std::getline(std::cin, line)) return false;
        line = trim(line);
        if (line.empty()) return false;

        // optional leading sign
        std::size_t pos = 0;
        if (line[0] == '+' || line[0] == '-') pos = 1;
        for (std::size_t i = pos; i < line.size(); ++i) {
            if (!std::isdigit(static_cast<unsigned char>(line[i]))) return false;
        }

        try {
            std::size_t idx = 0;
            out = std::stoi(line, &idx);
            return idx == line.size();
        } catch (...) {
            return false;
        }
    }

    static bool readDouble(double& out, const char* prompt = nullptr) {
        if (prompt) std::cout << prompt;
        std::string line;
        if (!std::getline(std::cin, line)) return false;
        line = trim(line);
        if (line.empty()) return false;
        try {
            std::size_t idx = 0;
            out = std::stod(line, &idx);
            return idx == line.size();
        } catch (...) {
            return false;
        }
    }

    // ---- field validators -----------------------------------------------
    static bool isValidEmail(const std::string& email) {
        // simple educational check: a non-empty local part, '@', then a dot
        // followed by at least one character
        auto at = email.find('@');
        if (at == std::string::npos || at == 0) return false;
        auto dot = email.find('.', at + 1);
        if (dot == std::string::npos) return false;
        return dot > at + 1 && dot + 1 < email.size();
    }

    static bool isValidPhone(const std::string& phone) {
        if (phone.empty()) return true;   // phone is optional
        for (char c : phone) {
            if (c == ' ' || c == '-' || c == '+' || c == '(' || c == ')') continue;
            if (!std::isdigit(static_cast<unsigned char>(c))) return false;
        }
        return true;
    }

    static bool isValidISBN(const std::string& isbn) {
        if (isbn.empty()) return true;    // ISBN is optional
        std::string cleaned;
        for (char c : isbn) {
            if (c == '-' || c == ' ') continue;
            cleaned.push_back(c);
        }
        if (cleaned.size() != 10 && cleaned.size() != 13) return false;
        for (char c : cleaned) {
            if (!std::isdigit(static_cast<unsigned char>(c))) return false;
        }
        return true;
    }

    static bool isValidDate(const std::string& date) {
        // accept YYYY-MM-DD
        if (date.size() != 10) return false;
        if (date[4] != '-' || date[7] != '-') return false;
        for (std::size_t i = 0; i < date.size(); ++i) {
            if (i == 4 || i == 7) continue;
            if (!std::isdigit(static_cast<unsigned char>(date[i]))) return false;
        }
        return true;
    }

    // ---- date helpers (plain YYYY-MM-DD arithmetic, no library needed) ----
    // Converting a date to a day number lets us do overdue/late-fee maths
    // without pulling in a date library.  All dates used by the system are
    // ISO "YYYY-MM-DD" strings, so a simple civil-date formula is enough.
    static int toDayNumber(const std::string& date) noexcept {
        if (!isValidDate(date)) return 0;
        const int y = std::stoi(date.substr(0, 4));
        const int m = std::stoi(date.substr(5, 2));
        const int d = std::stoi(date.substr(8, 2));

        // Howard Hinnant's days_from_civil algorithm.
        int yy = y;
        yy -= (m <= 2) ? 1 : 0;
        const int era = (yy >= 0 ? yy : yy - 399) / 400;
        const unsigned yoe = static_cast<unsigned>(yy - era * 400);
        const unsigned doy =
            (153u * static_cast<unsigned>(m + (m > 2 ? -3 : 9)) + 2u) / 5u +
            static_cast<unsigned>(d) - 1u;
        const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
        return era * 146097 + static_cast<int>(doe) - 719468;
    }

    // Positive when `a` is later than `b`.
    static int daysBetween(const std::string& a, const std::string& b) noexcept {
        const int da = toDayNumber(a);
        const int db = toDayNumber(b);
        if (da == 0 || db == 0) return 0;
        return da - db;
    }

    static std::string today() {
        std::time_t t = std::time(nullptr);
        std::tm tmv{};
#ifdef _WIN32
        ::localtime_s(&tmv, &t);
#else
        ::localtime_r(&t, &tmv);
#endif
        char buf[16] = {0};
        std::strftime(buf, sizeof(buf), "%Y-%m-%d", &tmv);
        return std::string(buf);
    }

    static std::string addDays(const std::string& date, int days) {
        const int base = toDayNumber(date);
        if (base == 0) return std::string();
        // Round-trip through civil_from_days (inverse of toDayNumber).
        int z = base + days + 719468;
        const int era = (z >= 0 ? z : z - 146096) / 146097;
        const unsigned doe = static_cast<unsigned>(z - era * 146097);
        const unsigned yoe = (doe - doe / 1460u + doe / 36524u - doe / 146096u) / 365u;
        const int yy = static_cast<int>(yoe) + era * 400;
        const unsigned doy = doe - (365u * yoe + yoe / 4u - yoe / 100u);
        const unsigned mp = (5u * doy + 2u) / 153u;
        const unsigned d = doy - (153u * mp + 2u) / 5u + 1u;
        const unsigned m = mp < 10u ? mp + 3u : mp - 9u;
        const int year = yy + (m <= 2u ? 1 : 0);

        std::ostringstream oss;
        oss << std::setw(4) << std::setfill('0') << year << '-'
            << std::setw(2) << std::setfill('0') << m << '-'
            << std::setw(2) << std::setfill('0') << d;
        return oss.str();
    }

    // Number of days `dueDate` is in the past relative to today.
    static int overdueDays(const std::string& dueDate) noexcept {
        return daysBetween(today(), dueDate);
    }

    // ---- pause / clear --------------------------------------------------
    static void pressAnyKey(const std::string& msg = "\nPress Enter to continue...") {
        std::cout << msg;
        std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
    }

    static void clearScreen() {
#ifdef _WIN32
        system("cls");
#else
        system("clear");
#endif
    }

    // ---- password masking ----------------------------------------------
    // Reads a line from /dev/tty without echoing, so the password never
    // appears in the terminal scroll-back or in shell history.
    static std::string readPassword(const std::string& prompt = "Password: ") {
        std::cout << prompt;
        std::string pass;
#ifdef _WIN32
        // Windows fallback - echo off via C runtime
        HANDLE hStdin = GetStdHandle(STD_INPUT_HANDLE);
        DWORD mode = 0;
        GetConsoleMode(hStdin, &mode);
        SetConsoleMode(hStdin, mode & ~ENABLE_ECHO_INPUT);
        std::getline(std::cin, pass);
        SetConsoleMode(hStdin, mode);
#else
        // POSIX termios
        struct termios oldt, newt;
        if (tcgetattr(STDIN_FILENO, &oldt) == 0) {
            newt = oldt;
            newt.c_lflag &= ~ECHO;
            tcsetattr(STDIN_FILENO, TCSANOW, &newt);
            std::getline(std::cin, pass);
            tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
        } else {
            std::getline(std::cin, pass);
        }
#endif
        return pass;
    }
};

#endif // LIBRARY_UTILS_H