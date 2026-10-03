# Multithreading and synchronisation

The application uses three threads and one process-level channel. Each thread
exists because the work it does must not block the terminal.

## Thread inventory

| Thread | Created by | Lifetime | Purpose |
|---|---|---|---|
| Logger worker | `Logger::Logger()` | whole program | the only thread that writes `logs/library.log` |
| Main / UI | `main()` | whole program | menus, prompts, business logic |
| FIFO reader | `main.cpp: FifoReader`, submitted to the pool | until shutdown | reads `/tmp/library_ipc_fifo` and forwards commands |
| Pool worker #2 | `ThreadManager(2)` | until shutdown | spare capacity for the pool API |

`ThreadManager` is the generic pool: a mutex-protected
`std::queue<std::function<void()>>` plus N workers, each waiting on a
`std::condition_variable`. It is a real component, not decoration - the FIFO
reader is submitted to it, and it is what `shutdown()` joins.

## The producer/consumer logger

```
  producer (UI thread)          Logger worker thread
  ---------------------          ---------------------
  build Entry {ts,event,
              message,pid,tid}
  lock_guard(mutex_)
  queue_.push(entry)
  unlock                        lock(mutex_)   [holds unique_lock]
  notify_one()                    cv_.wait(lock, stop_ || !queue_.empty())
                                  batch.swap(queue_)      <-- lock still held
                                  unlock
                                  open + write lines
                                  lock
```

Why a queue and a worker rather than writing inline:

* **No interleaving.** Only one thread ever writes the file, so two events can
  never be spliced into one line.
* **Producers do not block on disk.** `log()` returns as soon as the entry is
  queued; the file write happens on the worker.
* **The producing thread is identified at call time.** The pid and
  `std::thread::id` are captured in `log()`, not by the worker, so the log says
  which thread really produced the event.

### The subtlety that the tests prove

`std::queue::swap` is used to hand a batch to the worker while the lock is
still held. After the swap the batch is exclusively the worker's, so the worker
can unlock and write without holding the mutex - producers are not blocked while
the file is being written. `LoggerIsSafeFromManyThreads` runs 6 producer threads
x 40 events and asserts that every one of the 240 lines is well-formed, i.e.
that no two writes interleaved.

## Condition variables: two rules

1. **Always wait in a loop with a predicate**, never a bare `wait()`:
   ```cpp
   cv_.wait(lock, [this] { return stop_.load() || !queue_.empty(); });
   ```
   Without the predicate a spurious wake-up would make the worker read an empty
   queue.
2. **Change the shared state under the same lock that protects it.** `stop_` is
   an `std::atomic<bool>`, so setting it does not need the lock, but the
   `notify_all()` must come *after* the store, otherwise a waiter can wake up,
   see the old value and go back to sleep.

## Graceful shutdown

`ThreadManager::shutdown()`:

1. `stop_` false -> true with `compare_exchange_strong`, so a second caller
   cannot start a second shutdown
2. `cv_.notify_all()`
3. `join()` every worker

Workers **drain** the queue before exiting: the wait predicate returns as soon
as `stop_` is true, but the loop only returns when the queue is *also* empty.
Nothing that was already accepted is lost.

Each task is wrapped in `try/catch`. A task that throws `std::exception` is
reported on `stderr` and the worker continues; an unknown exception type is
caught too. A worker thread must never die from someone else's bad lambda.

`ThreadManagerStopsPromptly` asserts that joining an idle pool takes less than
500 ms, so a shutdown can never hang on a lost notification.

## The main-thread rule

The C++ memory model says touching a non-atomic object from two threads without
synchronisation is undefined behaviour. The project respects that by keeping
all mutation of `books_`, `members_` and `transactions_` on the main thread.
The other threads never touch the domain objects:

* the logger worker only touches its own `Entry` batch
* the FIFO reader only sends strings out (`g_monitor.sendEvent()`,
  `lib.notifyDriver()`, `lib.logger().log()`), all of which are internally
  synchronised

This is why `notifyDriver()` takes a `ProcessManager*` rather than giving the
reader thread direct access to the collections.

## Mutexes in the project

| Mutex | Protects | Lock type |
|---|---|---|
| `Logger::mutex_` | the log `queue_` | `std::lock_guard` in producers, `std::unique_lock` in the worker |
| `ThreadManager::mutex_` | the task `queue_` | `std::lock_guard` to push/pop, `std::unique_lock` to wait |
| `library_driver`'s `event_lock` | `latest_event`, `event_count` | kernel `mutex_lock` |

`ThreadManagerMutexProtectsSharedState` is the honest proof of correctness: 8
workers x 2000 increments of a plain `long long` with no atomic, and the test
requires the result to be exactly 16000. A missing mutex shows up as a smaller
number.

## Why `std::atomic` is not enough by itself

`std::atomic<int>` would make each increment race-free but would not make
`count++, get(), store()` sequences safe, and it gives the reader no way to
sleep. The mutex is needed for the compound operation; the atomic is used only
for the single-flag signal path, where a lock in a signal handler would be
unsafe anyway.
