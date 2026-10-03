# Requirements traceability matrix

Every requirement from the capstone specification, mapped to the code that
implements it and the test that proves it. Test counts refer to the suites
described in [testing.md](testing.md) (62 C++ tests + 42 CLI tests, all passing).

## Technology and platform

| # | Requirement | Implementation | Verified by |
|---|---|---|---|
| 1 | C++17 only for the main application | all application sources are `.cpp` compiled with `-std=c++17 -Wall -Wextra -pedantic -Wshadow` | `make` (zero warnings), `tests/unit_tests.cpp` |
| 2 | C for the Linux kernel module | `driver/library_driver.c` built by Kbuild | `make -C driver` |
| 3 | Linux / Ubuntu only | POSIX headers throughout; `class_create()` signature handled for Ubuntu | build on Ubuntu 24.04 / kernel 6.8 |
| 4 | Terminal-based application | `src/main.cpp` - all menus read from stdin, print to stdout | 42 CLI tests |
| 5 | File-based persistent storage | `src/FileManager.cpp`, `data/*.dat` | `LibraryDataSurvivesRestart` |
| 6 | GitHub-ready project structure | `Makefile`, `driver/Makefile`, `include/`, `src/`, `src/system/`, `tests/`, `docs/`, `scripts/`, `.gitignore`, `README.md` | - |

## Application features

| # | Requirement | Implementation | Verified by |
|---|---|---|---|
| 7 | Admin authentication | `src/Authentication.cpp` (salted SHA-256, constant-time compare) | `AuthenticationDefaultAccountWorks`, `AuthenticationStoresNoPlaintext`, CLI login tests |
| 8 | Book management - add | `Library::addBook()` + menu 1.1 | `LibraryAddsAndFindsBooks` |
| 9 | Book management - remove | `Library::removeBook()`, refuses if copies are on loan | `LibraryRemovesBook`, `LibraryRefusesToRemoveIssuedBookOrActiveMember` |
| 10 | Book management - update | `Library::updateBook()`, availability derived | `LibraryUpdatesBook` |
| 11 | Search | `Library::searchBooks(query, field)` by title/author/category/isbn/all | `LibrarySearchesBooks`, CLI search tests |
| 12 | Book view | menu 1.4 / 1.5 | CLI "book list" tests |
| 13 | Availability | `Book::availableCopies_`, menu 1.6 availability report | `LibraryIssuesBookAndUpdatesCounts`, CLI availability listing |
| 14 | Member management - add | `Library::addMember()` + menu 2.1 | `LibraryAddsAndSearchesMembers` |
| 15 | Member management - remove | `Library::removeMember()`, refuses if loans are open | `LibraryRefusesToRemoveIssuedBookOrActiveMember` |
| 16 | Member management - update | `Library::updateMember()` | menu 2.3, CLI update flow |
| 17 | Member search | `Library::searchMembers(query)` | `LibraryAddsAndSearchesMembers`, CLI member search |
| 18 | Member view | menu 2.4 / 2.5 | CLI "member count is correct" |
| 19 | Book issue | `Library::issueBook()`, checks availability and the borrow limit | `LibraryIssuesBookAndUpdatesCounts`, `LibraryRefusesIssueWhenUnavailableOrOverLimit`, `LibraryEnforcesBorrowLimit` |
| 20 | Book return | `Library::returnBook()`, restores copy and member count | `LibraryReturnRestoresAvailability` |
| 21 | Due date | `issueDate + Config::DEFAULT_LOAN_DAYS` (14 days) | `LibraryIssuesBookAndUpdatesCounts` |
| 22 | Late fee calculation | `Utils::daysBetween()` x `Config::LATE_FEE_PER_DAY` (5.0/day) | `LibraryCalculatesLateFee`, `UtilsDateArithmetic` |
| 23 | Transaction history | `Library::memberHistory()`, menu 2.6 | `LibraryDataSurvivesRestart`, CLI "loan history survives a restart" |
| 24 | Reports | `src/ReportManager.cpp`: summary, overdue, recently issued, borrowing history | `ReportsMatchLiveData`, `ReportsRenderToStream`, `ReportRefreshPicksUpChanges`, CLI report tests |

## Logging and concurrency

| # | Requirement | Implementation | Verified by |
|---|---|---|---|
| 25 | Logging | `src/Logger.cpp` - async, pid/tid/timestamp stamped | `LoggerWorkerThreadWritesEntries`, `LoggerFormatsEventsAndTimestamps` |
| 26 | Multithreading | logger worker, FIFO reader, `ThreadManager` pool | `ThreadManagerRunsEveryTask` |
| 27 | Mutex synchronisation | `std::mutex` + `condition_variable` in `Logger` and `ThreadManager`; kernel mutex in the driver | `ThreadManagerMutexProtectsSharedState`, `LoggerIsSafeFromManyThreads` |
| 28 | Proper thread shutdown and joining | `shutdown()` drains then joins; RAII destructors | `ThreadManagerStopsPromptly` |

## Linux system programming

| # | Requirement | Implementation | Verified by |
|---|---|---|---|
| 29 | Multiple processes | `ProcessManager::startLoggingProcess()` - `fork(2)` monitor child | `ProcessManagerForksMonitorThatReceivesEvents` |
| 30 | IPC | anonymous `pipe(2)` to the child + `mkfifo(2)` FIFO for external commands | `PipeAndForkChildWriteParentRead`, `FifoCreateAndTransferText`, `FifoSecondProcessConsumesMessages`, CLI FIFO tests |
| 31 | Linux signal handling | `SignalHandler` - `sigaction(2)` for `SIGINT`/`SIGTERM`/`SIGUSR1`, async-signal-safe flag | `SignalHandlerCatchesSigintFromAnotherProcess`, `SignalHandlerCatchesSigusr1`, CLI SIGINT test |
| 32 | Linux/POSIX system calls | `open`, `read`, `write`, `close`, `stat`, `chmod`, `rename`, `fork`, `waitpid`, `pipe`, `kill`, `mkfifo`, `poll` - full table in [system_calls.md](system_calls.md) | `PosixOpenReadWriteClose`, `PosixStatReportsSize`, `PosixChmodAndRename`, `PosixGetPidAndGetppid` |

## Driver

| # | Requirement | Implementation | Verified by |
|---|---|---|---|
| 33 | Real Linux character device | `driver/library_driver.c` - `alloc_chrdev_region`, `cdev_init`, `cdev_add`, `class_create`, `device_create` | `make -C driver` builds `library_driver.ko`; `modinfo` |
| 34 | User-space ↔ kernel-space communication | `write(2)` events in, `read(2)` report out; `copy_from_user` / `copy_to_user` | `DriverClientReportsAvailability` (loaded branch) |
| 35 | `event_count` and `latest_event` | `unsigned long event_count`, `char latest_event[256]`, both under a kernel mutex | `cat /dev/library_driver`, `DriverClientReportsAvailability` |
| 36 | Compatible with the current kernel | modern 5.15+ cdev API; Ubuntu vs upstream `class_create()` both handled | built against Ubuntu 24.04 kernel 6.8 headers |
| 37 | `open` / `read` / `write` / `release` | all four implemented in `struct file_operations` | module loads and answers (see manual steps in [device_driver.md](device_driver.md)) |
| 38 | App must not depend on the driver | `DriverClient` degrades to `false`/`-1`; one warning at start-up | `DriverClientReportsAvailability` (unloaded branch) |

## Testing, build and documentation

| # | Requirement | Implementation | Verified by |
|---|---|---|---|
| 39 | Testing | 62 C++ tests + 42 CLI tests | `make test` |
| 40 | Makefiles | top-level `Makefile`, `driver/Makefile`; `-MMD -MP` header dependency tracking | `make`, `make all`, `make clean`, `make test` |
| 41 | Documentation | `docs/architecture.md`, `system_calls.md`, `ipc.md`, `threading.md`, `signals.md`, `device_driver.md`, `testing.md`, `viva_notes.md`, this file | - |
| 42 | README.md | describes the actual final implementation, build and run instructions | - |

## Additional robustness work (beyond the requirement list)

| Area | Implementation | Verified by |
|---|---|---|
| Missing data files | normal first-run condition | `LibraryStartsCleanWithNoDataFiles` |
| Corrupted data files | rejected and logged, never fatal | `LibraryIgnoresCorruptedDataFile`, CLI corruption test |
| Crash-safe writes | temp file + `rename(2)` | `FileManagerAtomicWriteReplacesFile` |
| File permissions on credentials | `chmod 0600` | CLI "admin.dat permissions are 0600" |
| Referential integrity | cannot delete an issued book or an active member | `LibraryRefusesToRemoveIssuedBookOrActiveMember` |
| Transaction id uniqueness across restarts | `nextTransactionId_` resumes past the highest on disk | `LibraryDataSurvivesRestart` |
| Ctrl+C that actually works | poll-based line reader instead of `getline` | CLI "SIGINT triggers a graceful shutdown message" |
| Zero-warning build | `-Wall -Wextra -pedantic -Wshadow` | `make clean && make` produces no warnings |
