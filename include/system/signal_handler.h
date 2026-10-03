#ifndef LIBRARY_SIGNAL_HANDLER_H
#define LIBRARY_SIGNAL_HANDLER_H

#include <atomic>
#include <functional>

// ---------------------------------------------------------------------------
// SignalHandler - safe, asynchronous-safe signal handling.
//
// Pattern used (the "self-pipe" / atomic-flag approach):
//
//   signal handler (only sets an atomic flag)
//        |
//        v
//   main event loop polls the flag
//        |
//        v
//   performs real cleanup (stop threads, flush logs, close files)
//
// We never do complex work inside the handler itself because async-signal
// safety is very restricted (only reentrant functions allowed).
// ---------------------------------------------------------------------------
class SignalHandler {
public:
    SignalHandler();
    ~SignalHandler();

    SignalHandler(const SignalHandler&)            = delete;
    SignalHandler& operator=(const SignalHandler&) = delete;

    // Install handlers for SIGINT and SIGTERM.
    void install();

    // Returns true when a termination signal has been received.
    bool shouldStop() const noexcept { return stopRequested_.load(); }

    // Reset the flag (used after graceful shutdown).
    void reset() noexcept { stopRequested_.store(false); }

    // ---- optional SIGUSR1 hook -----------------------------------------
    // User-space defined signal; used here to trigger an on-the-fly report.
    void setOnUsr1(std::function<void()> cb) { onUsr1_ = std::move(cb); }
    bool usr1Requested() const noexcept { return usr1Requested_.load(); }
    void clearUsr1() noexcept { usr1Requested_.store(false); }

    // Runs the registered callback from a normal (non-handler) context.
    // Call it from the main loop after checking usr1Requested().
    void runUsr1Callback();

private:
    static void handleIntTerm(int);
    static void handleUsr1(int);

    static SignalHandler* instance_;   // singleton for the C callback

    std::atomic<bool> stopRequested_{false};
    std::atomic<bool> usr1Requested_{false};
    std::function<void()> onUsr1_;
};

#endif // LIBRARY_SIGNAL_HANDLER_H