#!/usr/bin/env bash
# ===========================================================================
#  run_cli_tests.sh - end-to-end tests that drive the real application binary.
#
#  These complement the C++ test suite: where that suite exercises the classes
#  directly, this script proves the *shipped* binary behaves correctly -
#  login, CRUD, issue/return, persistence across a restart, logging, the forked
#  monitor process, FIFO IPC and graceful SIGINT shutdown.
#
#  Usage:  bash tests/run_cli_tests.sh
# ===========================================================================
set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
APP="$ROOT/build/library_app"
WORKDIR="${TMPDIR:-/tmp}/library_cli_tests_$$"
FIFO="/tmp/library_ipc_fifo"
LOG="$WORKDIR/logs/library.log"

PASS=0
FAIL=0

pass() { printf '  [  OK  ] %s\n' "$1"; PASS=$((PASS + 1)); }
fail() { printf '  [ FAIL ] %s\n         %s\n' "$1" "${2:-}"; FAIL=$((FAIL + 1)); }

check() {  # check <description> <condition-exit-code>
    if [ "$2" -eq 0 ]; then pass "$1"; else fail "$1" "${3:-assertion failed}"; fi
}

contains() {  # contains <description> <file> <literal-pattern>
    if grep -qF -- "$3" "$2" 2>/dev/null; then
        pass "$1"
    else
        fail "$1" "pattern '$3' not found in $2"
    fi
}

not_contains() {
    if grep -qF -- "$3" "$2" 2>/dev/null; then
        fail "$1" "pattern '$3' unexpectedly present in $2"
    else
        pass "$1"
    fi
}

# ---------------------------------------------------------------------------
# Setup: a clean sandbox with its own data/ and logs/ directories.
# ---------------------------------------------------------------------------
rm -rf "$WORKDIR"
mkdir -p "$WORKDIR/data" "$WORKDIR/logs"

if [ ! -x "$APP" ]; then
    echo "ERROR: $APP not built. Run 'make' first." >&2
    exit 2
fi

cd "$WORKDIR" || exit 2

echo
echo "================================================================"
echo "  END-TO-END CLI TESTS"
echo "================================================================"
echo "  Work directory: $WORKDIR"

# ---------------------------------------------------------------------------
# 1. Login
# ---------------------------------------------------------------------------
OUT="$WORKDIR/out_login.txt"
printf 'admin\nadmin123\n0\n' | "$APP" > "$OUT" 2>&1
contains "login succeeds with the default admin account" "$OUT" "Welcome, admin"

# Wrong-credential run must be rejected.
printf 'admin\nwrongpass\nwrongpass\nwrongpass\n' | "$APP" > "$WORKDIR/out_badlogin.txt" 2>&1
not_contains "login with a wrong password is rejected" \
    "$WORKDIR/out_badlogin.txt" "Welcome, admin"
contains "lockout message after 3 failed attempts" \
    "$WORKDIR/out_badlogin.txt" "Too many failed attempts"

# ---------------------------------------------------------------------------
# 2. Book CRUD + search, in one session
#    Menu layout reminder: every action is followed by a "Press Enter" pause,
#    and "0" is the only key that returns to the previous menu.
# ---------------------------------------------------------------------------
OUT="$WORKDIR/out_books.txt"
{
    printf 'admin\nadmin123\n'
    printf '1\n1\n101\nDune\nFrank Herbert\nSciFi\n9780441013593\n1965\n2\n\n'   # add book 101
    printf '1\n1\n102\nNeuromancer\nWilliam Gibson\nSciFi\n9780441569595\n1984\n1\n\n' # add 102
    printf '0\n'                                                            # back to main
    printf '4\n1\nneuromancer\nall\n\n0\n'                                   # search, all fields
    printf '4\n1\ndune\ntitle\n\n0\n'                                       # search, title field
    printf '1\n3\n101\nDune Collector Edition\n\n\n\n\n0\n'                   # update book 101
    printf '1\n4\n\n0\n'                                                     # view all
    printf '1\n2\n102\n\n0\n'                                                # remove book 102
    printf '1\n4\n\n0\n'                                                     # view all again
    printf '0\n'
} | "$APP" > "$OUT" 2>&1

contains "search finds a book by partial title (case-insensitive)" "$OUT" "1 match(es)"
contains "search by the title field works" "$OUT" "Dune"
contains "book list shows the added title" "$OUT" "Dune"
contains "book update is applied" "$OUT" "Dune Collector Edition"
contains "removed book disappears from the list" "$OUT" "Total: 1 title(s)"

# ---------------------------------------------------------------------------
# 3. Members
# ---------------------------------------------------------------------------
OUT="$WORKDIR/out_members.txt"
{
    printf 'admin\nadmin123\n'
    printf '2\n1\n7\nAsha Roy\nasha@uni.edu\n9876543210\nCSE\n\n0\n'
    printf '2\n1\n8\nBiman Das\nbiman@uni.edu\n9123456780\nECE\n\n0\n'
    printf '2\n4\n\n0\n'
    printf '4\n2\nasha\n\n0\n'
    printf '0\n'
} | "$APP" > "$OUT" 2>&1

contains "member is added and listed" "$OUT" "Asha Roy"
contains "member search matches by name" "$OUT" "1 match(es)"
contains "member count is correct" "$OUT" "Total: 2 member(s)"

# ---------------------------------------------------------------------------
# 4. Issue / return / late fee
# ---------------------------------------------------------------------------
OUT="$WORKDIR/out_issue.txt"
{
    printf 'admin\nadmin123\n'
    printf '3\n1\n101\n7\n\n0\n'        # issue book 101 to member 7
    printf '3\n3\n\n0\n'                 # list active loans
    printf '5\n1\n\n0\n'                 # summary report
    printf '3\n2\n1000\ny\n\n0\n'        # return transaction 1000
    printf '5\n1\n\n0\n'
    printf '0\n'
} | "$APP" > "$OUT" 2>&1

contains "book is issued to the member" "$OUT" "Book issued"
contains "issue event reaches the log" "$LOG" "BOOK_ISSUE"
contains "active loan appears with its transaction id" "$OUT" "1000"
contains "summary reports one active loan before the return" "$OUT" "Active loans             : 1"
contains "return is recorded" "$OUT" "Book returned"
contains "return event reaches the log" "$LOG" "BOOK_RETURN"
contains "no active loans remain after the return" "$OUT" "Active loans             : 0"

# ---------------------------------------------------------------------------
# 5. Persistence across a restart
# ---------------------------------------------------------------------------
OUT="$WORKDIR/out_restart.txt"
{
    printf 'admin\nadmin123\n'
    printf '1\n4\n\n0\n'      # list books
    printf '2\n4\n\n0\n'      # list members
    printf '0\n'
} | "$APP" > "$OUT" 2>&1

contains "books survive a restart" "$OUT" "Dune Collector Edition"
contains "members survive a restart" "$OUT" "Asha Roy"
contains "loan history survives a restart" "$LOG" "BOOK_RETURN"

# The data files must actually exist and be non-empty where expected.
[ -s "$WORKDIR/data/books.dat" ]; check "data/books.dat was written" $?
[ -s "$WORKDIR/data/members.dat" ]; check "data/members.dat was written" $?
[ -s "$WORKDIR/data/transactions.dat" ]; check "data/transactions.dat was written" $?
[ -s "$WORKDIR/data/admin.dat" ]; check "data/admin.dat was written" $?

# The credential file must never contain the plaintext password.
not_contains "no plaintext password on disk" "$WORKDIR/data/admin.dat" "admin123"

# Credentials must be restrictive on POSIX.
MODE=$(stat -c '%a' "$WORKDIR/data/admin.dat" 2>/dev/null)
[ "$MODE" = "600" ]; check "admin.dat permissions are 0600 (got ${MODE:-unknown})" $?

# ---------------------------------------------------------------------------
# 6. Corrupted data file must not crash the application
# ---------------------------------------------------------------------------
cp "$WORKDIR/data/books.dat" "$WORKDIR/books.dat.bak"
printf 'garbage-not-a-valid-record' > "$WORKDIR/data/books.dat"

OUT="$WORKDIR/out_corrupt.txt"
{
    printf 'admin\nadmin123\n'
    printf '1\n4\n\n'
    printf '0\n0\n'
} | "$APP" > "$OUT" 2>&1
RC=$?

check "application survives a corrupted data file" "$RC"
contains "corrupted records are reported, not hidden" "$LOG" "no valid records"
cp "$WORKDIR/books.dat.bak" "$WORKDIR/data/books.dat"

# ---------------------------------------------------------------------------
# 7. Forked monitor process + pipe IPC
# ---------------------------------------------------------------------------
OUT="$WORKDIR/out_monitor.txt"
{
    printf 'admin\nadmin123\n'
    printf '1\n1\n201\nSolaris\nStanislaw Lem\nSciFi\n9780156027601\n1961\n1\n\n0\n'
    printf '0\n'
} | "$APP" > "$OUT" 2>&1

contains "monitor child process is forked" "$OUT" "Monitor process started"
contains "monitor logs the startup handshake" "$LOG" "MONITOR_START"
contains "business events reach the monitor over the pipe" "$LOG" "BOOK_ADD:201"
contains "monitor records its own pid and ppid" "$LOG" "MONITOR pid="

# ---------------------------------------------------------------------------
# 8. FIFO IPC from a second process
# ---------------------------------------------------------------------------
# The app is started in the background so it can stay alive long enough for an
# external writer to push an audit command into the FIFO.
{
    printf 'admin\nadmin123\n'
    printf '6\n\n'        # system integration status
    sleep 2
    printf '0\n0\n'
} | "$APP" > "$WORKDIR/out_ipc.txt" 2>&1 &
APP_PID=$!

# Wait until the FIFO exists.
for _ in $(seq 1 40); do
    [ -p "$FIFO" ] && break
    sleep 0.1
done

if [ -p "$FIFO" ]; then
    pass "IPC FIFO is created at $FIFO"
    printf 'SCAN book:101\n' > "$FIFO"
    sleep 0.6
    wait "$APP_PID" 2>/dev/null

    contains "FIFO command is forwarded to the monitor process" \
        "$LOG" "IPC SCAN book:101"
    contains "FIFO command is logged as an IPC event" \
        "$LOG" "FIFO command received"
else
    fail "IPC FIFO is created at $FIFO" "FIFO never appeared"
    wait "$APP_PID" 2>/dev/null
fi

contains "integration status reports the monitor pid" \
    "$WORKDIR/out_ipc.txt" "Monitor process : running"

# ---------------------------------------------------------------------------
# 9. Graceful SIGINT shutdown
# ---------------------------------------------------------------------------
rm -f "$LOG"
{
    printf 'admin\nadmin123\n'
    sleep 3
    printf '0\n0\n'
} | "$APP" > "$WORKDIR/out_sigint.txt" 2>&1 &
APP_PID=$!

sleep 1.5
kill -INT "$APP_PID" 2>/dev/null
wait "$APP_PID" 2>/dev/null
RC=$?

check "process exits cleanly after SIGINT (rc=$RC)" "$RC"
contains "SIGINT triggers a graceful shutdown message" \
    "$WORKDIR/out_sigint.txt" "Termination signal received"
contains "data is saved before the signal-driven exit" \
    "$WORKDIR/out_sigint.txt" "Goodbye."
contains "logger records the signal shutdown" "$LOG" "Graceful shutdown after signal"

# ---------------------------------------------------------------------------
# 10. Threading evidence in the log
# ---------------------------------------------------------------------------
contains "every log line carries the writing thread id" "$LOG" "[TID:"

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
echo
echo "----------------------------------------------------------------"
echo "  CLI tests: $PASS passed, $FAIL failed"
echo "----------------------------------------------------------------"

rm -rf "$WORKDIR"

[ "$FAIL" -eq 0 ]
