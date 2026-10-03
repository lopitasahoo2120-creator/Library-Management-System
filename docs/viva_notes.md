# Viva notes

Prepared answers for the five questions that matter for every Linux concept in
this project: **what is it, why did we use it, where is it used, how does it
work, how do we demonstrate it.**

---

## 1. File descriptors and POSIX file I/O

**What?** An integer index into a per-process table of open files. `open(2)`
returns one; `read(2)`, `write(2)`, `lseek(2)` and `close(2)` all take one.

**Why?** File-based persistence was a requirement, and on Linux "a file" is a
descriptor - the same interface serves regular files, FIFOs, sockets and
character devices. Using it for all of them is why the driver needs no special
API on the user side.

**Where?**
* `FileManager::open()` / `readAll()` / `writeAll()` - the data files
* `FileManager::atomicWrite()` - temp file + `rename(2)`
* `Logger::workerLoop()` - appending log lines
* `ProcessManager::sendEvent()` - the pipe to the child
* `IpcPipe::readData()` / `writeData()` - the FIFO
* `DriverClient` - `/dev/library_driver`

**How?** `open()` asks the kernel for a free descriptor and returns its number.
Every later call passes that number instead of a path, so the kernel knows which
file is meant without another lookup. `O_TRUNC` empties a file on open,
`O_NONBLOCK` stops a FIFO open from waiting, `O_APPEND` forces every write to
the end (which is how two processes can share one log without corrupting it).

**Demonstrate**

```bash
ls -l /proc/self/fd                     # this shell's descriptors
strace -e openat,read,write ./build/library_app   # see every call
echo "BOOK_ISSUE:1:1" > /dev/library_driver && cat /dev/library_driver
```

---

## 2. Threads: `std::thread`, `std::mutex`, `std::condition_variable`

**What?** Threads share one address space; a mutex protects a critical section;
a condition variable lets a thread sleep until work arrives.

**Why?** Two real problems. First, writing a log line from several places at
once would interleave them. Second, the UI must not wait for disk. A dedicated
logger thread fixes both: producers only enqueue, one thread owns the file.

**Where?** `Logger` (queue + worker), `ThreadManager` (generic pool), and the
FIFO reader thread submitted to that pool by `main.cpp`.

**How?** `log()` takes a `std::lock_guard`, pushes an `Entry`, and calls
`notify_one()` - then returns; the UI never touches the disk. The worker holds a
`std::unique_lock`, waits on the predicate `stop_ || !queue_.empty()`, swaps the
queue into a local batch, *unlocks*, then writes. Because `swap` happens under
the lock, the batch is exclusively the worker's, so producers are never blocked
by file I/O.

**Demonstrate**

```bash
grep 'TID:' logs/library.log | sort -u     # several distinct thread ids
grep -c 'TID:' logs/library.log            # every line stamped
LIBRARY_TEST_FILTER=LoggerIsSafeFromManyThreads ./build/library_tests
```

The test is the proof: 6 producer threads x 40 events produce 240 well-formed
lines and zero interleaved ones.

**Likely follow-up: "why not an atomic counter instead of a mutex?"**
An atomic makes each `++` race-free but not a read-modify-write sequence, and it
gives the consumer no way to sleep. `ThreadManagerMutexProtectsSharedState` makes
the difference concrete: 8 threads x 2000 plain `++` must total exactly 16000.

---

## 3. Processes: `fork()`, `waitpid()`, `pipe()`

**What?** `fork(2)` duplicates the current process, giving a child with the same
image but a new pid. `waitpid(2)` blocks until a child exits. `pipe(2)` creates a
one-directional kernel buffer between two file descriptors.

**Why?** The audit trail must survive a crash of the interactive UI and must not
share memory with it. A separate *process* gives isolation for free; a thread
would not.

**Where?** `ProcessManager::startLoggingProcess()` forks the monitor;
`main.cpp: startSubsystems()` creates the pipe; `ProcessManager::sendEvent()`
writes to it; `stopLoggingProcess()` sends `STOP` and `waitpid()`s the child.

**How?** The parent keeps only the write end, the child only the read end - a
pipe with an unwatched end is a classic bug. After `fork(2)` in the child, only
async-signal-safe calls are legal, so the child uses `read(2)`, `fopen`,
`getpid`, `localtime_r` and leaves with `_exit(2)` rather than `exit()`, which
would run C++ destructors and flush buffers duplicated from the parent.
`waitpid()` is what prevents a zombie; the test asserts a second `waitpid()`
returns `ECHILD`.

**Demonstrate**

```bash
./build/library_app &
ps -ef | grep '[l]ibrary_app'        # two pids
grep 'MONITOR pid=' logs/library.log # the child's own pid and ppid
```

---

## 4. IPC: named FIFO

**What?** A FIFO is a file that the kernel presents as a byte stream: anything
written to one end appears at the other, with no reader or writer required at
creation time.

**Why?** The running application must accept commands from *other* processes
(audit tool, second instance, shell script) without restarting.

**Where?** `IpcPipe` in `src/system/ipc.cpp`; `FifoReader` in `src/main.cpp`
runs `poll(2)` on `/tmp/library_ipc_fifo` and forwards each line to the monitor
child and to the driver.

**How?** `mkfifo(2)` creates the node; `lstat(2)` confirms it really is a FIFO;
the kernel routes bytes between the two open descriptors.

Three details worth volunteering, because they are what examiners ask about:

1. **Opening write-only blocks** until a reader attaches. `O_NONBLOCK` avoids
   that but then returns `ENXIO` if no reader exists - so the reader must be
   opened first.
2. **`SIGPIPE`**: writing with no reader raises `SIGPIPE`, whose default action
   kills the process. The program sets `SIGPIPE` to `SIG_IGN` so the write just
   fails with `EPIPE`.
3. **Read until `'\n'`**, not one `read()` per message: a pipe may split a
   message across reads and may also return several messages in one read.

**Demonstrate**

```bash
ls -l /tmp/library_ipc_fifo                  # p = named pipe
echo "SCAN book:101" > /tmp/library_ipc_fifo
grep 'IPC SCAN book:101' logs/library.log
```

---

## 5. Signals: `sigaction()`, `SIGINT`, `SIGTERM`, `SIGPIPE`, `SIGUSR1`

**What?** An asynchronous event delivered to a process. `sigaction(2)` installs
the disposition.

**Why?** Ctrl+C must shut the program down *cleanly* - saving data and joining
threads - rather than killing it mid-write.

**Where?** `SignalHandler` (`src/system/signal_handler.cpp`), installed at the
top of `main()`; polled by every menu loop.

**How?** Between delivery and process death only async-signal-safe operations
are legal: no `malloc`, no `printf`, no mutex, no `std::string`. So the handler
does exactly one thing - `stopRequested_.store(true)`, a single atomic store.
The main loop notices and performs the real shutdown. This is the atomic-flag
form of the self-pipe pattern.

**The hard-won detail.** `std::getline()` cannot be used for the prompts:
libstdc++ retries a `read(2)` that failed with `EINTR`, so Ctrl+C at a prompt
sets the flag and then leaves the process blocked in `getline()` forever - the
graceful shutdown never runs. `LineReader` in `main.cpp` therefore uses
`poll(2)` with a 250 ms timeout and re-checks the flag between polls. Extra code,
but the requirement ("Ctrl+C should result in graceful shutdown") is only
genuinely met with it.

**Demonstrate**

```bash
./build/library_app &
kill -INT $!        # wait: data saved, "Goodbye.", exit code 0
kill -USR1 $!       # prints a report without leaving the menu
dmesg | tail       # nothing from the handler itself - it only sets a flag
```

---

## 6. Character device driver

**What?** A kernel module that registers a `struct cdev` on a major number and a
device node, so user space can `open()`/`read()`/`write()` it.

**Why?** It demonstrates real user-space ↔ kernel-space communication, which is
the point of the requirement. It also gives the application a kernel-side audit
counter it cannot tamper with from user space.

**Where?** `driver/library_driver.c` (C, kernel APIs);
`src/DriverClient.cpp` (user space); `Library::notifyDriver()` broadcasts events
to it.

**How?**

```
open()    -> cdev lookup by major number, filp->f_pos = 0
write()   -> copy_from_user() into a kernel buffer, trim '\n',
             reject non-printable bytes, store, event_count++  (under mutex)
read()    -> scnprintf "events=N\nlatest=<text>\n", copy_to_user()
```

**Three answers to have ready:**

* *"Why not `init_module()` / `register_chrdev()`?"* Those are pre-2.6 APIs.
  `alloc_chrdev_region()` allocates a major number dynamically, so the module
  cannot collide with an existing device.

* *"How is your module version-portable?"* The Ubuntu kernel keeps the
  single-argument `class_create()` for out-of-tree compatibility while upstream
  5.15+ takes an owner argument, so the source supports both and defaults to the
  Ubuntu signature (`-DLIBRARY_CLASS_CREATE_TAKES_OWNER` switches).

* *"Why `copy_to_user` instead of `memcpy`?"* The user pointer is untrusted.
  `copy_to_user` validates it and returns the number of bytes it could not copy,
  so the driver reports `-EFAULT` instead of crashing the kernel.

**Demonstrate**

```bash
make -C driver && sudo insmod driver/library_driver.ko
ls -l /dev/library_driver                 # crw-------
cat /dev/library_driver                   # events=0 latest=(none)
echo "BOOK_ISSUE:101:7" > /dev/library_driver
cat /dev/library_driver                   # events=1, latest=BOOK_ISSUE:101:7
sudo rmmod library_driver
```

**Graceful degradation (explicitly required):** without the module the
application prints one warning, `isAvailable()` is false, every driver call
returns false, and nothing else changes. `DriverClientReportsAvailability`
asserts both branches.

---

## 7. File persistence without a database

**What?** Records serialised to a binary stream and loaded back at start-up.

**Why?** File-based storage was required and no external dependency may be used.

**Where?** `Book::serialize()`, `Member::serialize()`,
`Transaction::serialize()`, driven by `FileManager::writeAll()` / `readAll()`.

**How?** Each string is written as `uint32_t` length followed by raw bytes; each
integer as 4 bytes. Records are variable-length but self-delimiting, so reading
one leaves the stream exactly at the start of the next.

Three design decisions to defend:

* **Validation on load, not only on construction.** `deserialize()` refuses a
  record that violates its invariants, so a corrupt file yields "no valid
  records" instead of a nonsensical in-memory catalogue
  (`LibraryIgnoresCorruptedDataFile`).
* **Atomic replacement.** Write to `path.tmp`, then `rename(2)`. `rename` within
  a directory is atomic, so a reader sees the old file or the new file.
* **Transaction ids never restart.** `Library::loadAll()` sets
  `nextTransactionId_` to one past the highest id on disk, so ids stay unique
  across restarts.

---

## 8. Authentication without storing passwords

**What?** Salted SHA-256, implemented from scratch so there is no crypto
dependency.

**Why?** Credentials must not be readable from `data/admin.dat`.

**How?** `Authentication::generateSalt()` makes 16 hex characters from
`std::random_device`; `hashPassword()` returns `salt:sha256(salt + password)`;
`login()` recomputes and compares with a constant-time `secureEquals()` that
accumulates XOR differences instead of returning early.

**Be honest about the limitation** - it earns more credit than hiding it:
SHA-256 is fast, so a stolen `admin.dat` can be brute forced. A production system
needs a slow KDF (bcrypt, scrypt, Argon2id). The value here is that the
plaintext password is never written to disk, and `AuthenticationStoresNoPlaintext`
proves it.

---

## 9. OOP design

* **Encapsulation** - every entity field is private with `const`-correct getters.
* **Abstraction** - callers use `display()`, `serialize()`, `isValid()`,
  `canBorrow()` and never touch fields.
* **Single responsibility** - `FileManager` does I/O and nothing else,
  `ReportManager` computes and prints numbers and stores nothing,
  `Logger` writes text and knows nothing about books.
* **Composition over inheritance** - `Library` *has* a `Logger`, a `FileManager`,
  an `Authentication`, a `ReportManager` and a `DriverClient`, all held by
  `std::unique_ptr` so ownership is unambiguous.
* **RAII** - `IpcPipe::~IpcPipe()` closes the descriptor, `ThreadManager` and
  `Logger` destructors join their threads, so no path through the program leaks
  a descriptor or leaves a thread running.
* **Rule of zero / destructor that cannot throw** - `~Library()` wraps
  `shutdown()` in `try/catch` because a destructor must not propagate.

---

## 10. Debugging tools actually used

```bash
strace -f -e trace=process,desc ./build/library_app   # fork/pipe/read/write
ltrace ./build/library_app                            # library calls
gdb -p $(pgrep library_app) -ex 'thread apply all bt' # inspect the threads
gdb --args ./build/library_app                        # break in issueBook
dmesg | tail                                          # the driver's pr_info
ls -l /proc/$(pgrep library_app)/fd                   # the open descriptors
```

---

## 11. Honest limitations

1. No kernel module can be loaded under WSL2 - the driver is compiled against
   real headers, but `insmod` needs a real Ubuntu machine or a VM.
2. The build tree must not contain spaces: the kernel's Kbuild passes `M=`
   unquoted, so `M=/path/with space/driver` breaks. This is a Kbuild limitation,
   not a bug in the project.
3. The binary format has no version field, so files written by an older build
   with different fields would not load. `deserialize()` rejects them rather
   than misreading them.
4. SHA-256 password hashing is educational only - see section 8.
5. `ReportManager` caches its counters, so `refresh()` must be called after a
   mutation. `Library` does this before printing any report.
