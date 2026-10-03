# Signal handling

## Signals used

| Signal | Installed by | Handler does | Effect |
|---|---|---|---|
| `SIGINT` (2) | `SignalHandler::install()` | `stopRequested_.store(true)` | graceful shutdown |
| `SIGTERM` (15) | `SignalHandler::install()` | `stopRequested_.store(true)` | graceful shutdown |
| `SIGUSR1` (10) | `SignalHandler::install()` | `usr1Requested_.store(true)` | print a report without leaving the menu |
| `SIGPIPE` (13) | `SignalHandler::install()` | ignored | a write to a FIFO with no reader must not kill the app |

`sigaction(2)` is used rather than `signal(2)` because `sigaction` fully specifies
the behaviour (mask, flags, whether the syscall is restarted) instead of leaving
it implementation-defined.

## The rule: the handler does almost nothing

Between the moment a signal is delivered and the moment the process is killed,
only **async-signal-safe** operations are legal. That rules out almost
everything useful: no `malloc`, no `printf`, no `std::string`, no mutex, no
throwing.

So the handler is one line:

```cpp
void SignalHandler::handleIntTerm(int) {
    if (instance_) instance_->stopRequested_.store(true);
}
```

`std::atomic<bool>::store` compiles to a single instruction on x86 and is
async-signal-safe. The heavy work happens in the main loop, which is the "atomic
flag" pattern also known as the *self-pipe* approach in its simplest form.

## Graceful shutdown sequence

```
Ctrl+C
  |
  v
handler: stopRequested_ = true          (only this happens in signal context)
  |
  v
main loop notices (at the next prompt or at the top of the menu loop)
  |
  +--> saveAll()                  all three data files written
  +--> stopSubsystems()
  |      +--> g_fifoStop = true; g_pool.shutdown()   joins the FIFO reader
  |      +--> g_fifo.close(); unlink(FIFO)
  |      +--> stopLoggingProcess()  STOP -> waitpid -> child reaped
  +--> lib.shutdown()             flush + join the logger, final log marker
  v
"Goodbye."   exit code 0
```

The data is saved **before** the threads are stopped, so the logger thread is
still alive to record the shutdown itself.

## Why the prompts are not plain `std::getline`

This is the part that is easy to get wrong. `std::getline()` ends in
`read(2)`, and libstdc++'s `basic_filebuf::underflow` retries whenever `errno ==
EINTR`. A `Ctrl+C` at a prompt therefore sets the flag and then the process
stays blocked inside `getline()` forever - the graceful shutdown never runs.

The fix is `LineReader` in `src/main.cpp`:

```cpp
struct pollfd pfd { STDIN_FILENO, POLLIN, 0 };
const int rc = ::poll(&pfd, 1, 250);
if (rc == 0) continue;                    // timeout: loop back
if (g_signals.shouldStop()) return false;  // checked at the top of the loop
```

Polling with a 250 ms timeout means the main loop wakes up four times a second,
sees the flag and returns to the caller, which unwinds the menus and shuts down.
The same class also fixes a second problem: the FIFO reader thread needs a
non-blocking wait so it can be joined, and `poll` provides it.

## `SIGPIPE`

`write(2)` to a FIFO (or a socket) whose reader has closed raises `SIGPIPE`.
Its default disposition terminates the process, which would mean a stray
`echo x > /tmp/library_ipc_fifo` at the wrong moment could kill the library
system. `SignalHandler::install()` sets it to `SIG_IGN`, so the `write` simply
returns `-1` with `EPIPE` and the caller logs and continues.

## `SIGUSR1` - an interactive demonstration

```bash
./build/library_app &
kill -USR1 <pid>        # the menu is idle, nothing is being edited
```

The handler sets `usr1Requested_`. At the top of `mainMenu()` the loop calls
`runUsr1Callback()`, which runs the registered callback **in normal program
context** and clears the flag. Because the callback executes outside signal
context, it is free to print a full report.

## `SignalHandler` lifecycle

```cpp
SignalHandler handler;      // registers as the C-callback singleton
handler.install();          // sigaction() x3, SIGPIPE ignored
handler.shouldStop();       // read from the main loop
handler.reset();            // clear the flag after a graceful shutdown
                            // destructor restores SIG_DFL/SIG_DFL so a second
                            // Ctrl+C kills the process as the user expects
```

`tests/system_tests.cpp` verifies all of this:

* `SignalHandlerCatchesSigusr1` - `raise(SIGUSR1)`, flag set, callback runs,
  flag cleared
* `SignalHandlerCatchesSigintFromAnotherProcess` - a forked child really sends
  `SIGINT`, which is what the terminal does, rather than testing `raise()`
* `SignalHandlerInstallsTermAndIgnoresSigpipe` - after `install()` the
  dispositions are the handlers, not `SIG_DFL`; `SIGPIPE` is `SIG_IGN`; after
  destruction `SIGINT` is back to `SIG_DFL`
