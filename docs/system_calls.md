# Linux system calls used

Every POSIX call the project relies on, why it is needed, and where it is
called from. Nothing here is included just to satisfy a requirement - if a call
is listed, the program breaks without it.

## File and directory calls

| Call | Why the project needs it | Where |
|---|---|---|
| `open(2)` / `close(2)` | every file read and write in the application | `FileManager::open()`, `FileManager::close()`, `Logger` worker, `DriverClient` |
| `read(2)` / `write(2)` | binary record I/O, log lines, driver events, pipe traffic | `FileManager`, `Logger`, `ProcessManager::sendEvent()`, `IpcPipe`, `DriverClient` |
| `stat(2)` | detect whether a data file exists before loading it, and whether a path is a character device | `Library::loadAll()`, `DriverClient::open()` |
| `lstat(2)` | confirm a path really is a FIFO | `IpcPipe::create()` |
| `mkdir(2)` | create `data/` and `logs/` on first run (mkdir -p semantics) | `FileManager::ensureParentDirectory()`, `Logger::ensureLogDirectory()`, `ProcessManager::ensureDirFor()`, `Library::ensureDataDirectory()` |
| `chmod(2)` | lock the credential file to `0600` | `FileManager::open(strictAdminFile)`, `Authentication::restrictPermissions()`, `Logger` after each write |
| `rename(2)` | atomic whole-file replacement, so a crash cannot truncate data | `FileManager::atomicWrite()`, used by `Authentication::saveAdmin()` |
| `unlink(2)` | remove the FIFO and failed temporary files | `IpcPipe::remove()`, `FileManager::atomicWrite()` |
| `rmdir(2)` | test cleanup | `tests/unit_tests.cpp` |

## Process calls

| Call | Why | Where |
|---|---|---|
| `fork(2)` | start the monitor child process | `ProcessManager::startLoggingProcess()` |
| `waitpid(2)` | reap the child on shutdown; escalate to `SIGTERM`/`SIGKILL` if it hangs | `ProcessManager::stopLoggingProcess()` |
| `pipe(2)` | parent-to-child event channel | `main.cpp: startSubsystems()` |
| `_exit(2)` | leave the child without running C++ destructors or flushing inherited stdio buffers | `ProcessManager::monitorLoop()`, tests |
| `exec`-family | not used: the child is a copy of the parent image, which is exactly what makes the fork cheap | - |
| `setsid()` | detach the monitor from the controlling terminal so Ctrl+C does not kill it | `ProcessManager::monitorLoop()` |
| `getpid(2)` | stamped on every log line, printed in the banner and the integration screen | `Logger::log()`, `main.cpp` |
| `getppid(2)` | recorded by the monitor so the audit trail names both processes | `ProcessManager::monitorLoop()` |

## Signals

| Call | Why | Where |
|---|---|---|
| `sigaction(2)` | install handlers for `SIGINT`, `SIGTERM`, `SIGUSR1`; used instead of the older `signal(2)` so the behaviour is fully specified | `SignalHandler::install()` |
| `kill(2)` | escalate to `SIGTERM` then `SIGKILL` if the child ignores `STOP` | `ProcessManager::stopLoggingProcess()` |
| `raise(3)` | used by the tests to deliver `SIGUSR1` to the test process itself | `tests/system_tests.cpp` |
| `signal(SIGPIPE, SIG_IGN)` | writing to a FIFO whose reader has died must not kill the process | `SignalHandler::install()` |

## Thread-related

| Facility | Why | Where |
|---|---|---|
| `std::thread` | dedicated log writer, FIFO reader, worker pool | `Logger`, `main.cpp`, `ThreadManager` |
| `std::mutex` | protects the log queue, the task queue and shared counters | `Logger`, `ThreadManager`, tests |
| `std::condition_variable` | a consumer must sleep until there is work, not spin | `Logger::workerLoop()`, `ThreadManager::workerLoop()` |
| `std::atomic<bool>` | the signal flag, written from a signal handler, read from the main loop | `SignalHandler`, `main.cpp` |

## Other

| Call | Why | Where |
|---|---|---|
| `poll(2)` | read the FIFO without blocking, and read stdin in a way Ctrl+C can interrupt | `main.cpp: FifoReader::run()`, `LineReader::readLine()` |
| `mkfifo(2)` | create the named pipe used for commands from other processes | `IpcPipe::create()` |
| `localtime_r(2)` | thread-safe timestamp formatting for log lines | `Logger::nowTimestamp()` |
| `strftime` / `put_time` | render timestamps and dates | `Logger`, `Utils::today()` |

## Not used, and why

* **`ioctl(2)`** - the driver contract is deliberately "write one text line,
  read one small report". Adding `ioctl` would need a shared header and a
  userspace/kernel struct layout, which adds risk without adding function.
* **`mmap(2)`** - the data files are small and read sequentially; `read(2)` is
  simpler and sufficient.
* **`select(2)`** - `poll(2)` is used instead because its `revents` output makes
  "hung up" distinguishable from "no data".
* **`pthread_*` C API** - the C++ standard library wrappers are used, which is
  what a C++17 project should do. Both end up in the same futex-based
  implementation underneath.
