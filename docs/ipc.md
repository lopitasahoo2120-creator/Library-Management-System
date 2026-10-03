# IPC and multi-process architecture

The project uses two distinct IPC mechanisms because they solve two different
problems. Neither is simulated: both are real kernel interfaces.

## 1. Anonymous pipe + fork: the monitor child process

### Problem

The audit trail must be written even if the interactive UI dies, and a bug in
the UI must not be able to corrupt the audit records. A thread would share the
same address space and could not survive a crash in that space.

### Solution

`ProcessManager` (`include/system/process_manager.h`,
`src/system/process_manager.cpp`) creates an anonymous pipe and forks.

```
main process                                        child process
-----------                                         -------------
pipe(fds)  -> fds[0]=read  fds[1]=write

fork()
  |                                                monitorLoop(fds[0]):
  |                                                  setsid()
  |                                                  read() in a loop
  |                                                  split on '\n'
  |                                                  append audit lines
  |                                                  exit on "STOP" or EOF
  |
  +-- close(fds[0])
  +-- keep fds[1]
  |
  +-- sendEvent("BOOK_ISSUE:101:7")  --write(2)-->  [MONITOR pid=… ppid=…]
```

### Events sent to the monitor

`Library::notifyDriver()` broadcasts every significant business event to both
subscribers:

| Event | Emitted by |
|---|---|
| `BOOK_ADD:<id>` | `Library::addBook()` |
| `BOOK_UPDATE:<id>` | `Library::updateBook()` |
| `BOOK_REMOVE:<id>` | `Library::removeBook()` |
| `MEMBER_ADD:<id>` | `Library::addMember()` |
| `MEMBER_UPDATE:<id>` | `Library::updateMember()` |
| `MEMBER_REMOVE:<id>` | `Library::removeMember()` |
| `BOOK_ISSUE:<bookId>:<memberId>` | `Library::issueBook()` |
| `BOOK_RETURN:<bookId>:<memberId>` | `Library::returnBook()` |

plus `MONITOR_START parent=<pid>` at start-up and `IPC <command>` for anything
that arrives through the FIFO.

### Shutdown protocol

1. the parent `write(2)`s the line `STOP\n`
2. the child sees it, closes its read end and returns 0 from `monitorLoop()`
3. the child calls `_exit(0)` (never `exit()`, so no inherited C++ destructor
   or stdio buffer is touched after `fork()`)
4. the parent `waitpid(2)`s - blocking, with an `EINTR` retry loop
5. if the child is still alive after ~5 s of retries, the parent escalates to
   `SIGTERM`, waits 100 ms, then `SIGKILL`

### Why the child never sees the parent's `std::cout`

`fork()` duplicates the file descriptors, so a child that used `printf` would
interleave output with the parent's. The child therefore uses nothing except
`read(2)`, `open`/`fopen` on its own log, `getpid`, `localtime_r` and
`_exit(2)`. `tests/system_tests.cpp`
(`ProcessManagerChildIsReapedByWaitpid`) proves the reap actually happens by
checking that a later `waitpid()` returns `-1` with `ECHILD`.

## 2. Named FIFO: commands from other processes

### Problem

A stock script or a second copy of the application may need to push a note into
the running system - for example an audit tool marking a shelf check. The
running instance must accept it without restarting.

### Solution

`IpcPipe` (`include/system/ipc.h`, `src/system/ipc.cpp`) wraps a FIFO at
`/tmp/library_ipc_fifo` (`Config::IPC_FIFO_PATH`).

```
   any process                      main process
   -----------                       ------------
   mkfifo(...)? already there
   echo "SCAN book:101" > fifo  --write-->  FifoReader thread (poll 200 ms)
                                             buffer partial reads
                                             on '\n': trim
                                               -> monitor child via pipe
                                               -> kernel driver via write()
                                               -> Logger::IPC_EVENT
```

### The two blocking rules that matter

1. **Opening a FIFO write-only blocks until a reader attaches.** That is what
   makes a FIFO usable between two processes, but it also means the *reader*
   must be opened first when using `O_NONBLOCK` - a non-blocking write-only
   open with no reader fails with `ENXIO`. `IpcPipe::open()` therefore offers
   both modes and the callers choose: the application opens its reader
   non-blocking (so startup never hangs), the tests demonstrate the blocking
   behaviour explicitly.
2. **`SIGPIPE`.** If every reader goes away, a `write(2)` raises `SIGPIPE`,
   whose default action is to kill the process. `SignalHandler::install()` sets
   it to `SIG_IGN`, so a write simply fails with `EPIPE`.

### Message protocol

One UTF-8/ASCII text line per message, terminated by `\n`, at most
`Config::IPC_MAX_MESSAGE` (256) bytes.

```
SCAN book:101          a stock-take command for one title
CHECK member:7        an audit note about one member
# a comment           ignored (leading '#')
```

`FifoReader` reads with `poll(2)` on a 200 ms timeout rather than a blocking
`read(2)`. That is deliberate: a blocking read cannot be interrupted, so the
reader thread could never be joined at shutdown.

## 3. How to demonstrate IPC in a viva

```bash
# Terminal 1 - run the application and log in
./build/library_app

# Terminal 2 - the FIFO appears as soon as the app starts
ls -l /tmp/library_ipc_fifo          # srwxrwxrwx ... (a named pipe)
echo "SCAN book:101" > /tmp/library_ipc_fifo

# Terminal 1 shows:  [IPC] forwarded external command: SCAN book:101
# and the child process appended it to logs/library.log
```

Further checks:

```bash
ps -ef | grep library_app            # parent and monitor child
ls -l /dev/library_driver           # character device after insmod
cat /dev/library_driver             # the kernel-side event report
```
