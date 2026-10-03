# Library Management System

A terminal-based Library Management System in **C++17**, accompanied by a real
**Linux character-device kernel module** written in C. The application manages
books, members, loans, late fees, search and reports; persists everything to
plain files; logs asynchronously; runs a second process for the audit trail;
accepts commands from other processes over a FIFO; handles `SIGINT`/`SIGTERM`
gracefully; and reports events to the kernel through `/dev/library_driver` when
the module is loaded.

No external libraries, no database, no web anything. Standard library plus POSIX.

---

## Contents

```
.
├── Makefile                 build the app and the tests
├── README.md
├── .gitignore
├── include/                 headers
│   ├── Book.h  Member.h  Transaction.h        domain entities
│   ├── Library.h                               business-logic coordinator
│   ├── ReportManager.h                         reports
│   ├── FileManager.h  Logger.h  Authentication.h  DriverClient.h
│   ├── Config.h  Utils.h                       constants + helpers
│   └── system/                                 system-programming layer
│       ├── ipc.h  thread_manager.h  process_manager.h  signal_handler.h
├── src/                     implementations (mirrors include/)
│   ├── main.cpp             menus, prompt handling, subsystem wiring
│   ├── Library.cpp  ReportManager.cpp  DriverClient.cpp
│   ├── Book.cpp  Member.cpp  Transaction.cpp
│   ├── FileManager.cpp  Logger.cpp  Authentication.cpp
│   └── system/  ipc.cpp  thread_manager.cpp  process_manager.cpp
│               signal_handler.cpp
├── driver/
│   ├── library_driver.c     the kernel module (C)
│   └── Makefile             Kbuild wrapper
├── tests/
│   ├── test_harness.h       tiny dependency-free test framework
│   ├── test_main.cpp        runner (isolated scratch directory)
│   ├── unit_tests.cpp       41 unit / functional tests
│   ├── system_tests.cpp     21 Linux system-programming tests
│   └── run_cli_tests.sh     42 end-to-end tests on the real binary
├── docs/                    architecture, ipc, threading, signals,
│                            device_driver, system_calls, testing,
│                            viva_notes, requirements_traceability
├── scripts/dev_build.sh     per-file compile check during development
├── data/                    books.dat, members.dat, transactions.dat, admin.dat
└── logs/                    library.log
```

## Requirements

* Ubuntu 22.04 or 24.04 (tested on 24.04, kernel 6.8)
* g++ with C++17 support (tested with 13.3.0)
* GNU make
* `linux-headers-$(uname -r)` **only** for building the kernel module

On Debian/Ubuntu:

```bash
sudo apt update
sudo apt install -y build-essential linux-headers-$(uname -r)
```

## Build

```bash
make                 # -> build/library_app
make tests           # -> build/library_tests
make driver          # -> driver/library_driver.ko   (needs kernel headers)
make all             # app + tests
make clean
```

Warnings are never suppressed: the build uses
`-std=c++17 -Wall -Wextra -pedantic -Wshadow -pthread` and compiles cleanly.

> **Note:** the kernel build system (`Kbuild`) does not support paths containing
> spaces. If your checkout lives under a directory with a space in its name,
> build the driver from a symlink, e.g.
> `ln -s "$PWD" ~/library && make -C ~/library/driver`.

## Run

```bash
./build/library_app
```

The first start creates `data/admin.dat` with the default account:

```
Username: admin
Password: admin123
```

Change it from menu option 7 (the current password is required, and the new
password must be at least 6 characters).

### Menus

```
1. Book management          add / remove / update / view all / view one / availability
2. Member management        add / remove / update / view all / view one / history
3. Issue / return           issue, return, list active loans, list transactions
4. Search                   books by field, members by any contact field
5. Reports                  summary, overdue, recently issued, borrowing history
6. System integration status  driver, monitor pid, FIFO, worker threads
7. Change admin password
8. Save data now
9. Logout
0. Exit
```

## The kernel module (optional)

The application is **fully functional without it**; loading it only adds
kernel-side event reporting.

```bash
make -C driver
sudo insmod driver/library_driver.ko

ls -l /dev/library_driver          # crw------- 1 root root ...
cat /dev/library_driver            # events=0 / latest=(none)
echo "BOOK_ISSUE:101:7" > /dev/library_driver
cat /dev/library_driver            # events=1 / latest=BOOK_ISSUE:101:7

sudo rmmod library_driver
```

Without the module the application prints exactly one warning at start-up:

```
Warning: Library driver unavailable. Continuing without kernel event reporting.
```

Menu option 6 shows the live kernel state either way.

> WSL2 cannot load kernel modules. The module still *compiles* there; `insmod`
> needs a real Ubuntu machine or a VM.

## Seeing the Linux features

```bash
# asynchronous logger - every line stamped with pid and thread id
tail -f logs/library.log

# the forked monitor child process (two processes: parent + child)
./build/library_app &
ps -ef | grep '[l]ibrary_app'
grep 'MONITOR pid=' logs/library.log

# FIFO IPC from another process
echo "SCAN book:101" > /tmp/library_ipc_fifo
grep 'IPC SCAN book:101' logs/library.log

# SIGUSR1 prints a report without leaving the menu
kill -USR1 <pid>

# graceful shutdown on Ctrl+C: data is saved, threads joined, exit code 0
./build/library_app
```

## Tests

```bash
make test
```

Runs both suites:

| Suite | Count | Scope |
|---|---|---|
| `build/library_tests` | 62 | 41 unit/functional + 21 Linux system-programming |
| `tests/run_cli_tests.sh` | 42 | end-to-end on the real binary |

The C++ suite runs in a scratch directory, so the project's own `data/` folder
is never touched. Run one test while debugging:

```bash
LIBRARY_TEST_FILTER=LibraryCalculatesLateFee ./build/library_tests
```

See [docs/testing.md](docs/testing.md) for the full list of what each test
verifies.

## Configuration

Every magic number is in `include/Config.h`:

| Constant | Default | Meaning |
|---|---|---|
| `DEFAULT_LOAN_DAYS` | 14 | loan period |
| `LATE_FEE_PER_DAY` | 5.0 | late fee per overdue day |
| `MAX_BOOKS_PER_MEMBER` | 5 | simultaneous loans per member |
| `TRANSACTION_ID_START` | 1000 | first transaction id |
| `DRIVER_DEVICE` | `/dev/library_driver` | character device path |
| `IPC_FIFO_PATH` | `/tmp/library_ipc_fifo` | named pipe path |
| `MAX_LOGIN_ATTEMPTS` | 3 | before lockout |

## Storage format

Binary, append-only, one record after another. Each string is a `uint32_t` length
followed by the raw bytes; integers are 4 bytes; the late fee is an IEEE-754
double. `deserialize()` validates every record, so a corrupt file yields "no
valid records" in the log instead of a broken catalogue. Whole-file replacement
uses write-temp + `rename(2)`, which is atomic.

## Documentation

| File | Contents |
|---|---|
| [docs/architecture.md](docs/architecture.md) | layers, entities, data flow, error policy |
| [docs/system_calls.md](docs/system_calls.md) | every POSIX call, why, and where |
| [docs/ipc.md](docs/ipc.md) | fork + pipe, and the FIFO protocol |
| [docs/threading.md](docs/threading.md) | the async logger, mutex rules, shutdown |
| [docs/signals.md](docs/signals.md) | `sigaction`, the handler rule, Ctrl+C |
| [docs/device_driver.md](docs/device_driver.md) | the driver, its APIs, and version notes |
| [docs/testing.md](docs/testing.md) | every test and what it proves |
| [docs/viva_notes.md](docs/viva_notes.md) | what/how/where/demonstrate per concept |
| [docs/requirements_traceability.md](docs/requirements_traceability.md) | requirement → code → test matrix |

## Security notes

* Passwords are never written to disk. `data/admin.dat` holds
  `salt:sha256(salt + password)` with a 16-hex-character salt from
  `std::random_device`, compared in constant time.
* `data/admin.dat` is `chmod 0600`.
* This is educational hashing: SHA-256 is fast, so a stolen file could be
  brute forced. A production system needs bcrypt/scrypt/Argon2id. See
  [docs/viva_notes.md](docs/viva_notes.md) section 8.
