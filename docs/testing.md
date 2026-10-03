# Testing

Two suites, run by `make test`:

| Suite | What it covers | Runner |
|---|---|---|
| `build/library_tests` | 41 unit/functional tests + 21 Linux system-programming tests | `tests/test_main.cpp` + `tests/unit_tests.cpp` + `tests/system_tests.cpp` |
| `tests/run_cli_tests.sh` | 42 end-to-end tests driving the real binary | bash |

Current status: **62/62 C++ tests pass, 42/42 CLI tests pass.**

```bash
make test          # builds and runs both suites
```

There is no external test framework. `tests/test_harness.h` is about 130 lines:
a registry, a `CHECK` macro, a `CHECK_EQ` that prints both values, and a
`CHECK_THROWS`. This keeps the project dependency-free and keeps a failed
assertion from hiding the remaining tests.

### Test isolation

`Config.h` paths are relative (`data/books.dat`, ...), so `test_main.cpp` simply
`chdir`s into a scratch directory before running anything. The project's real
`data/` folder is never touched by the test suite.

Run a single test while debugging:

```bash
LIBRARY_TEST_FILTER=LibraryCalculatesLateFee ./build/library_tests
```

## 1. Unit and functional tests (41)

### Entity validation
| Test | Verifies |
|---|---|
| `BookRejectsEmptyTitle` | constructor throws `std::invalid_argument` |
| `BookRejectsNegativeCopies` | negative total or available copies are refused |
| `BookRejectsAvailableAboveTotal` | `available > total` is refused |
| `BookSettersValidate` | setters validate, and shrinking the total clamps availability |
| `MemberRejectsBadFields` | id/name rules, and that email/phone validation lives in the setters |
| `MemberBorrowLimitRespectsConfig` | `canBorrow()` honours `MAX_BOOKS_PER_MEMBER`; the counter never goes negative |
| `TransactionRejectsNonPositiveIds` | id and date validation |
| `TransactionStatusRoundTrip` | `Status` ↔ string conversion, with a safe default |

### Binary serialisation
| Test | Verifies |
|---|---|
| `BookSerialisationRoundTrip` | every field survives a write/read cycle |
| `MemberSerialisationRoundTrip` | including the issued-book counter |
| `TransactionSerialisationRoundTrip` | dates, the `double` fee and the status enum |
| `DeserialisationRejectsGarbage` | 16 bytes of `0x7F` are rejected, not stored |

### Utilities
| Test | Verifies |
|---|---|
| `UtilsValidators` | email, phone, ISBN (10/13 digits, `-` ignored), `YYYY-MM-DD` date |
| `UtilsDateArithmetic` | `addDays` across a month end and a leap day (2024-02-28 → 29), `daysBetween` sign |
| `UtilsTrimAndSearch` | trimming, empty handling, case-insensitive substring search |

### File storage
| Test | Verifies |
|---|---|
| `FileManagerWritesAndReadsCollection` | `writeAll` then `readAll` returns the same records, same order |
| `FileManagerCreatesParentDirectories` | `open("nested/dir/x.dat")` creates the tree |
| `FileManagerAtomicWriteReplacesFile` | content is replaced and no `.tmp` file is left behind |
| `FileManagerHandlesMissingFile` | opening a missing file raises `FileIOException`, the stream stays closed, `readAll` returns false |

### Authentication
| Test | Verifies |
|---|---|
| `AuthenticationDefaultAccountWorks` | first run provisions `admin` / `admin123`; wrong password and wrong user both fail |
| `AuthenticationStoresNoPlaintext` | `data/admin.dat` contains no plaintext and does contain `salt:hash` |
| `AuthenticationHashesAreSalted` | hashing the same password twice gives different digests, each 81 chars |
| `AuthenticationChangePassword` | change works, old password stops working, then restores the default |

### Library business logic
| Test | Verifies |
|---|---|
| `LibraryAddsAndFindsBooks` | add three books, look them up, unknown id returns null |
| `LibraryRejectsDuplicateAndInvalidBooks` | duplicate id refused, negative id refused, empty title throws |
| `LibraryUpdatesBook` | update applied; unknown id refused |
| `LibraryRemovesBook` | remove then find returns null; removing twice fails |
| `LibrarySearchesBooks` | search by title, author, category, isbn, all-fields, and no-match |
| `LibraryAddsAndSearchesMembers` | add, search by name and by department, duplicate refused |
| `LibraryIssuesBookAndUpdatesCounts` | availability drops, member count rises, due date is today + 14, id starts at 1000 |
| `LibraryRefusesIssueWhenUnavailableOrOverLimit` | no copy left, unknown book, unknown member |
| `LibraryEnforcesBorrowLimit` | exactly 5 loans allowed, the 6th refused |
| `LibraryReturnRestoresAvailability` | availability restored, member count back to 0, double return refused |
| `LibraryCalculatesLateFee` | on-time return costs 0; a loan made 3 days overdue is reloaded as `OVERDUE` with 3 x 5.0 = 15.0 |
| `LibraryRefusesToRemoveIssuedBookOrActiveMember` | referential integrity: an issued book and an active member cannot be deleted |

### Persistence
| Test | Verifies |
|---|---|
| `LibraryDataSurvivesRestart` | a destroyed `Library` is fully recoverable in a new one, and new loans do not reuse transaction ids |
| `LibraryStartsCleanWithNoDataFiles` | no data files at all is a valid first run, and the app is usable afterwards |
| `LibraryIgnoresCorruptedDataFile` | a file of `0x7F` bytes does not crash, produces an error in the log, and a later `saveAll` works |

### Reports
| Test | Verifies |
|---|---|
| `ReportsMatchLiveData` | every `Summary` counter matches the data it was derived from |
| `ReportsRenderToStream` | all four report formats render, including the "nothing to show" paths |
| `ReportRefreshPicksUpChanges` | `refresh()` recomputes from the live vectors |

## 2. Linux system programming tests (21)

### POSIX calls
| Test | Verifies |
|---|---|
| `PosixOpenReadWriteClose` | `O_CREAT`, `write`, `lseek`, `read`, byte count |
| `PosixStatReportsSize` | `st_size == 1024` and `S_ISREG` |
| `PosixChmodAndRename` | `chmod` to `0600` is visible in `st_mode`, `rename` moves the file, `unlink` removes it |
| `PosixGetPidAndGetppid` | both pids are positive |

### Threads, mutex, condition variable
| Test | Verifies |
|---|---|
| `ThreadManagerRunsEveryTask` | 200 tasks across 4 workers all run |
| `ThreadManagerMutexProtectsSharedState` | 8 workers x 2000 plain `++` gives exactly 16000 - a missing lock shows up as a smaller number |
| `ThreadManagerStopsPromptly` | joining an idle pool takes under 500 ms, so a lost notification cannot hang shutdown |
| `LoggerWorkerThreadWritesEntries` | 50 events reach the log file after `shutdown()` |
| `LoggerIsSafeFromManyThreads` | 6 producers x 40 events produce 240 well-formed lines and **0** interleaved ones |
| `LoggerFormatsEventsAndTimestamps` | event names and the `YYYY-MM-DD HH:MM:SS.mmm` shape |
| `LoggerAppendsAcrossInstances` | the log is appended across instances and `SYSTEM_SHUTDOWN` is the last marker |

### fork / pipe / waitpid
| Test | Verifies |
|---|---|
| `PipeAndForkChildWriteParentRead` | child `write`, parent `read`, `waitpid` reports `WIFEXITED` and exit code 0 |
| `ProcessManagerForksMonitorThatReceivesEvents` | the child is a different pid, two events are sent, and the log contains `[MONITOR pid=<child>]`, `BOOK_ISSUE:101:7` and `MEMBER_ADD:7` |
| `ProcessManagerChildIsReapedByWaitpid` | after `stopLoggingProcess()` a further `waitpid()` returns `-1`/`ECHILD`, i.e. the child was really reaped and cannot become a zombie |

### FIFO
| Test | Verifies |
|---|---|
| `FifoCreateAndTransferText` | `S_ISFIFO` on the created path, and a line written through the FIFO is read back intact |
| `FifoSecondProcessConsumesMessages` | a forked child consumes three lines the parent writes and exits 0 - the real two-process pattern |

### Signals
| Test | Verifies |
|---|---|
| `SignalHandlerCatchesSigusr1` | `raise(SIGUSR1)` sets the flag, the callback runs outside handler context, the flag clears |
| `SignalHandlerCatchesSigintFromAnotherProcess` | a forked child sends `SIGINT` (as a terminal would); the flag is observed within a second |
| `SignalHandlerInstallsTermAndIgnoresSigpipe` | dispositions are handlers not `SIG_DFL`, `SIGPIPE` is `SIG_IGN`, and the destructor restores `SIG_DFL` |

### Character device
| Test | Verifies |
|---|---|
| `DriverClientReportsAvailability` | with the module loaded: the event counter increments by one and the report contains `latest=TEST_EVENT:1:1`. Without it: every call degrades to `false`/`-1` and the app keeps working |
| `DriverClientRejectsOversizedEvents` | events over 256 bytes and empty events are refused before reaching the kernel |

## 3. End-to-end CLI tests (42)

`tests/run_cli_tests.sh` drives the shipped binary through a pipe inside a
sandbox directory, so it tests what a user actually gets.

| Area | Checks |
|---|---|
| Login | default account accepted; wrong password rejected; lockout after 3 attempts |
| Book CRUD | add 2 books, search by partial title and by field, update, view all, remove, verify the removed title is gone |
| Member CRUD | add 2 members, list, search by name |
| Issue / return | issue, transaction id 1000 appears, summary shows 1 active loan, return confirmed, summary shows 0 |
| Logging | `BOOK_ISSUE`, `BOOK_RETURN` and every line carrying `[TID:` |
| Persistence | books and members visible after a restart; all four data files non-empty; `admin.dat` permissions `0600`; the plaintext password is absent from disk |
| Corruption | garbage in `books.dat` - the app still starts and the log says `no valid records` |
| Multi-process | the monitor child is forked, `MONITOR_START`, `BOOK_ADD:201` and `MONITOR pid=` all appear |
| FIFO IPC | the FIFO appears, an external `echo "SCAN book:101" > fifo` is forwarded to the monitor and logged, and the integration screen shows `Monitor process : running` |
| Signals | `kill -INT` on the running binary leads to a clean exit, `Termination signal received`, `Graceful shutdown after signal` and `Goodbye.` |

## 4. Verifying by hand

```bash
# Persistence
./build/library_app          # add a book, exit
./build/library_app          # it is still there

# Logging with pid and thread id
tail -f logs/library.log

# Threads
grep -c 'TID:' logs/library.log

# Monitor child process
ps -ef | grep -c "[l]ibrary_app"     # 2: parent + monitor

# IPC
echo "SCAN book:101" > /tmp/library_ipc_fifo
grep 'IPC SCAN' logs/library.log

# Signals - start, then press Ctrl+C: data is saved and the process exits 0
./build/library_app

# Driver, if loaded
echo "BOOK_ISSUE:101:7" > /dev/library_driver
cat /dev/library_driver
```

## 5. What is not covered

* No test loads a kernel module: WSL2 cannot `insmod`, and CI has no privileged
  kernel. The driver path is covered by `DriverClientReportsAvailability` in
  both the loaded and unloaded states, and by building the module against real
  kernel headers.
* No load/performance testing - out of scope for this project.
