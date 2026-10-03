# Architecture

## 1. What the system is

A terminal-based Library Management System written in C++17, plus a Linux
character-device kernel module written in C. The application manages books,
members, loans, late fees, search and reports; persists everything to plain
files; logs asynchronously; runs a second process for the audit trail; accepts
commands from other processes over a FIFO; handles SIGINT/SIGTERM gracefully;
and reports events to the kernel through `/dev/library_driver` when the module
is loaded.

Nothing in the design is simulated. Every Linux facility listed below is a real
kernel interface, and each one has a job the application actually needs.

## 2. Layers

```
+---------------------------------------------------------------+
|  L3  Presentation        src/main.cpp                          |
|      menus, prompt handling, subsystem wiring, shutdown order   |
+---------------------------------------------------------------+
|  L2  Business logic      src/Library.cpp                       |
|      policy: loan period, borrow limit, late fee, CRUD, search |
|      src/ReportManager.cpp   reports computed from live data    |
+---------------------------------------------------------------+
|  L1  Services             src/FileManager.cpp   binary storage  |
|                          src/Logger.cpp        async logging   |
|                          src/Authentication.cpp credentials    |
|                          src/DriverClient.cpp   /dev access     |
+---------------------------------------------------------------+
|  L0  System layer         src/system/ipc.cpp            FIFO    |
|                          src/system/thread_manager.cpp pool    |
|                          src/system/process_manager.cpp fork   |
|                          src/system/signal_handler.cpp sigact |
+---------------------------------------------------------------+
|  Kernel  driver/library_driver.c   char device /dev/library_driver
+---------------------------------------------------------------+
```

Dependencies only ever point downwards. L2 knows about L1; L1 knows about L0;
L0 knows about the kernel. `main.cpp` is the only place that wires the layers
together.

## 3. Entities

| Class | Responsibility | Persistence |
|---|---|---|
| `Book` | one catalogue title, copy counts, availability | `serialize()` / `deserialize()` |
| `Member` | one registered borrower, running loan count | `serialize()` / `deserialize()` |
| `Transaction` | one issue/return event with dates, fee, status | `serialize()` / `deserialize()` |

All three follow the same shape:

* **Encapsulation** - every field is private with `const`-correct getters.
* **Validation at construction** - `Book` rejects an empty title or a copy
  count that contradicts itself; `Transaction` rejects non-positive ids.
* **Validation on load** - `deserialize()` refuses garbage instead of storing
  half-built state, which is what keeps a corrupt file from crashing the app.

### Binary storage format

Records are stored back-to-back with no header and no separators:

```
uint32_t   string length, then that many raw bytes   (repeated per string)
int        plain integers (4 bytes)
double     plain IEEE-754 (8 bytes)
int        transaction status as an enum value
```

Variable-length strings mean the format is not fixed-size, so each record ends
exactly where the next one begins: `deserialize()` reads one record and the
stream position is naturally correct for the next call. `FileManager::readAll()`
is therefore just "read records until the stream stops yielding them".

`FileManager::atomicWrite()` writes to `path.tmp` and then `rename(2)`s over
the target. `rename(2)` within one directory is atomic, so a reader sees either
the old file or the new file, never a half-written one. This is used for the
credential file, where a truncated write would mean losing the account.

## 4. Control flow of a book issue

`main.cpp` collects the ids, then `Library::issueBook()`:

1. find the book and the member; refuse if either is unknown
2. refuse if `availableCopies == 0`
3. refuse if the member already holds `Config::MAX_BOOKS_PER_MEMBER` books
4. compute `dueDate = today + Config::DEFAULT_LOAN_DAYS`
5. append a `Transaction`, decrement the book's availability, increment the
   member's count
6. log the event through `Logger`
7. broadcast the event to the kernel driver and to the monitor process
8. `saveAll()` to disk

Steps 6-8 are what make the feature demonstrable in a viva: one issue produces
one log line, one driver event and one audit line in the child process.

## 5. Data flow of the asynchronous logger

```
   UI thread / FIFO reader thread
            |  Logger::log()      (mutex, push, notify_one)
            v
   std::queue<Entry>  ---- condition_variable ---->  Logger worker thread
                                                              |
                                                              v
                                                       logs/library.log
```

Producers never touch the file. They take a `std::lock_guard`, push an `Entry`
and return. A single worker thread owns the file, so a line can never be
interleaved with another. Each entry already carries the timestamp, pid and
thread id captured at `log()` time, which means the producer thread is never
blocked on file I/O.

`Logger::shutdown()` sets the stop flag, notifies all, joins the worker, drains
whatever is left and writes the final `SYSTEM_SHUTDOWN` marker itself. That is
why the shutdown marker is always the last line in the file.

## 6. Process architecture

```
   Main process (pid 1000)
        |                       anonymous pipe(2)
        |  BOOK_ISSUE:101:7  ------------------>  Monitor child (pid 1001)
        |                                             |
        |                                             v
        |                                     appends to logs/library.log
        |
        +---- open/write/read/close --> /dev/library_driver  (kernel)
        |
        +---- reader thread <--- /tmp/library_ipc_fifo <--- other processes
```

The monitor runs in its own address space (created with `fork(2)`). Its job is
to write a tamper-evident audit trail with its own pid and its parent's pid. A
separate process is genuinely useful here: the audit trail keeps growing even
if the interactive UI dies, and it cannot corrupt the UI's memory because it
does not share any.

## 7. Error handling policy

| Situation | Behaviour |
|---|---|
| Missing data file | normal first-run condition, start empty, log it |
| Data file present but no valid record | log an error naming the file, continue empty |
| `FileManager::open()` fails | throw `FileIOException`; `Library` catches and logs |
| Entity validation fails | `std::invalid_argument` from the constructor or setter |
| Kernel driver absent | print one warning, run normally, `isAvailable() == false` |
| `fork()`/`pipe()` fails | print a warning, disable only that subsystem |
| SIGINT / SIGTERM | set an atomic flag; the main loop saves, joins and exits |

The guiding rule is that the library system must stay usable. A missing driver
or a dead FIFO degrades one feature; it never takes down the application.

## 8. Why the app is not interrupted by Ctrl+C

`std::getline()` cannot be used for prompts: libstdc++ retries a `read(2)` that
failed with `EINTR`, so a Ctrl+C at a prompt would set the handler's flag but
leave the program blocked forever. `LineReader` in `main.cpp` therefore polls
`STDIN_FILENO` with `poll(2)` and a 250 ms timeout, checking the stop flag
between polls. That is a small amount of extra code for a correct Ctrl+C.
