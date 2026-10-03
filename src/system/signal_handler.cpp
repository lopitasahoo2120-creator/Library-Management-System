// ---------------------------------------------------------------------------
// signal_handler.cpp - async-signal-safe SIGINT / SIGTERM handling.
//
// POSIX rules forbid almost everything inside a signal handler (no malloc, no
// stdio, no locks), so the handler does exactly one thing: it stores into a
// std::atomic<bool>.  The main program polls shouldStop() at every safe point
// and performs the real shutdown (save data, stop threads, waitpid the child).
//
// SIGUSR1 is used as an "on-the-fly report" request.  It sets a second flag
// which main.cpp picks up, again without doing work in the handler.
//
// Real syscalls / API used: sigaction(2), kill(2).
// ---------------------------------------------------------------------------

#include "system/signal_handler.h"

#include <csignal>

SignalHandler* SignalHandler::instance_ = nullptr;

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------
SignalHandler::SignalHandler() {
    instance_ = this;
}

SignalHandler::~SignalHandler() {
    // Restore the default disposition so a second Ctrl+C kills us as usual
    // even after shutdown() has run.
    struct sigaction sa{};
    sa.sa_handler = SIG_DFL;
    ::sigemptyset(&sa.sa_mask);
    ::sigaction(SIGINT,  &sa, nullptr);
    ::sigaction(SIGTERM, &sa, nullptr);

    if (instance_ == this) instance_ = nullptr;
}

// ---------------------------------------------------------------------------
// C callbacks - must stay trivial.
// ---------------------------------------------------------------------------
void SignalHandler::handleIntTerm(int) {
    if (instance_) instance_->stopRequested_.store(true);
}

void SignalHandler::handleUsr1(int) {
    if (instance_) instance_->usr1Requested_.store(true);
}

// ---------------------------------------------------------------------------
// install()
// ---------------------------------------------------------------------------
void SignalHandler::install() {
    struct sigaction sa{};
    sa.sa_handler = handleIntTerm;
    ::sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;              // no SA_RESTART: blocking reads should return

    ::sigaction(SIGINT,  &sa, nullptr);
    ::sigaction(SIGTERM, &sa, nullptr);

    struct sigaction us{};
    us.sa_handler = handleUsr1;
    ::sigemptyset(&us.sa_mask);
    us.sa_flags = 0;
    ::sigaction(SIGUSR1, &us, nullptr);

    // A dead peer on the IPC FIFO must not kill the application.
    ::signal(SIGPIPE, SIG_IGN);
}

// ---------------------------------------------------------------------------
// onUsr1 callback - invoked from the *main* loop, never from the handler.
// ---------------------------------------------------------------------------
void SignalHandler::runUsr1Callback() {
    if (usr1Requested_.exchange(false) && onUsr1_) {
        onUsr1_();
    }
}