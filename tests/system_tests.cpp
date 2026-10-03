// ---------------------------------------------------------------------------
// system_tests.cpp - tests for the Linux system-programming layer.
//
// Each test proves a real kernel facility is working, not just that a function
// returns true:
//
//   * threads / mutex / condition_variable  -> Logger + ThreadManager
//   * fork() / waitpid() / pipe()           -> ProcessManager
//   * FIFO (mkfifo)                          -> IpcPipe
//   * sigaction() / SIGINT / SIGUSR1        -> SignalHandler
//   * POSIX file calls                       -> open/read/write/close/stat/
//                                             chmod/rename/unlink
//   * character device                       -> DriverClient (must degrade
//                                             gracefully when not loaded)
// ---------------------------------------------------------------------------

#include "test_harness.h"

#include "Config.h"
#include "DriverClient.h"
#include "FileManager.h"
#include "Logger.h"
#include "Utils.h"

#include "system/ipc.h"
#include "system/process_manager.h"
#include "system/signal_handler.h"
#include "system/thread_manager.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

// ---------------------------------------------------------------------------
// 1. POSIX system calls
// ---------------------------------------------------------------------------
SYSTEM_TEST(PosixOpenReadWriteClose) {
    const int fd = ::open("posix_test.bin", O_CREAT | O_RDWR | O_TRUNC, 0644);
    CHECK(fd >= 0);
    if (fd < 0) return;

    const std::string payload = "library system programming";
    const ssize_t written = ::write(fd, payload.data(), payload.size());
    CHECK_EQ(static_cast<long>(written), static_cast<long>(payload.size()));

    ::lseek(fd, 0, SEEK_SET);
    char buffer[64] = {0};
    const ssize_t read = ::read(fd, buffer, sizeof(buffer) - 1);
    CHECK_EQ(static_cast<long>(read), static_cast<long>(payload.size()));
    CHECK_EQ(std::string(buffer), payload);

    ::close(fd);
    std::remove("posix_test.bin");
}

SYSTEM_TEST(PosixStatReportsSize) {
    const int fd = ::open("stat_test.bin", O_CREAT | O_WRONLY | O_TRUNC, 0644);
    CHECK(fd >= 0);
    if (fd >= 0) {
        const std::string data(1024, 'x');
        ::write(fd, data.data(), data.size());
        ::close(fd);
    }

    struct stat st {};
    CHECK(::stat("stat_test.bin", &st) == 0);
    CHECK_EQ(static_cast<long long>(st.st_size), 1024LL);
    CHECK(S_ISREG(st.st_mode));

    std::remove("stat_test.bin");
}

SYSTEM_TEST(PosixChmodAndRename) {
    const int fd = ::open("perm_test.bin", O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd >= 0) ::close(fd);

    // Restrict to owner read/write, exactly what the credential file uses.
    CHECK(::chmod("perm_test.bin", S_IRUSR | S_IWUSR) == 0);

    struct stat st {};
    CHECK(::stat("perm_test.bin", &st) == 0);
    CHECK_EQ(static_cast<long>(st.st_mode & 0777), static_cast<long>(0600));

    // rename(2) moves the file.
    CHECK(::rename("perm_test.bin", "perm_renamed.bin") == 0);
    CHECK(::stat("perm_renamed.bin", &st) == 0);

    // unlink(2) removes it.
    CHECK(::unlink("perm_renamed.bin") == 0);
    CHECK(::stat("perm_renamed.bin", &st) != 0);
}

SYSTEM_TEST(PosixGetPidAndGetppid) {
    CHECK(::getpid() > 0);
    CHECK(::getppid() > 0);

    const std::string record =
        "self=" + std::to_string(::getpid()) +
        " parent=" + std::to_string(::getppid());
    CHECK(record.size() > 10);
}

// ---------------------------------------------------------------------------
// 2. Threads, mutex and condition_variable
// ---------------------------------------------------------------------------
SYSTEM_TEST(ThreadManagerRunsEveryTask) {
    std::atomic<int> counter{0};

    {
        ThreadManager pool(4);
        for (int i = 0; i < 200; ++i) {
            pool.enqueue([&counter]() { counter.fetch_add(1); });
        }
        pool.shutdown();
    }   // destructor must be safe after an explicit shutdown

    CHECK_EQ(counter.load(), 200);
}

SYSTEM_TEST(ThreadManagerMutexProtectsSharedState) {
    // 8 workers x 2000 increments with a plain counter: only the mutex can
    // keep this exact.
    long long counter = 0;
    std::mutex    guard;

    ThreadManager pool(8);
    for (int i = 0; i < 8; ++i) {
        pool.enqueue([&counter, &guard]() {
            for (int k = 0; k < 2000; ++k) {
                std::lock_guard<std::mutex> lock(guard);
                ++counter;
            }
        });
    }
    pool.shutdown();

    CHECK_EQ(counter, 16000LL);
}

SYSTEM_TEST(ThreadManagerStopsPromptly) {
    ThreadManager pool(1);
    const auto start = std::chrono::steady_clock::now();
    pool.shutdown();
    const auto elapsed = std::chrono::steady_clock::now() - start;

    // Joining an idle worker must be immediate, not a sleep.
    CHECK(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count()
          < 500);
}

SYSTEM_TEST(LoggerWorkerThreadWritesEntries) {
    // Logger always writes to Config::LOG_FILE, so the check is "50 more
    // BOOK_ISSUE lines than before", not an absolute count.
    const auto countEvent = [](const char* event) {
        std::ifstream in(Config::LOG_FILE);
        int           lines = 0;
        std::string   line;
        while (std::getline(in, line)) {
            if (line.find(event) != std::string::npos) ++lines;
        }
        return lines;
    };

    const int before = countEvent("BOOK_ISSUE");

    {
        Logger logger;
        for (int i = 0; i < 50; ++i) {
            logger.log(Logger::Event::BOOK_ISSUE, "concurrent event " + std::to_string(i));
        }
        logger.shutdown();   // joins the worker and flushes
    }                          // destructor must not hang or double-join

    CHECK_EQ(countEvent("BOOK_ISSUE"), before + 50);
}

SYSTEM_TEST(LoggerIsSafeFromManyThreads) {
    // Start from a clean file so the line count is exact.
    std::remove(Config::LOG_FILE);

    {
        Logger logger;
        std::vector<std::thread> producers;
        for (int t = 0; t < 6; ++t) {
            producers.emplace_back([&logger, t]() {
                for (int i = 0; i < 40; ++i) {
                    logger.log(Logger::Event::DRIVER_EVENT,
                               "producer " + std::to_string(t) +
                                   " event " + std::to_string(i));
                }
            });
        }
        for (std::thread& t : producers) t.join();
        logger.shutdown();
    }

    // Count only well-formed lines: every write must be atomic, so there must
    // be no line that got interleaved with another.
    std::ifstream in(Config::LOG_FILE);
    int           good = 0, malformed = 0;
    std::string   line;
    while (std::getline(in, line)) {
        if (line.find("producer ") == std::string::npos) continue;  // lifecycle noise
        if (line.size() > 40 && line.find("[PID:") != std::string::npos &&
            line.find("[TID:") != std::string::npos &&
            line.find("[DRIVER_EVENT]") != std::string::npos) {
            ++good;
        } else {
            ++malformed;   // an interleaved write
        }
    }

    CHECK_EQ(good, 240);
    CHECK_EQ(malformed, 0);
}

SYSTEM_TEST(LoggerFormatsEventsAndTimestamps) {
    CHECK_EQ(std::string(Logger::eventToString(Logger::Event::LOGIN)),
             std::string("LOGIN"));
    CHECK_EQ(std::string(Logger::eventToString(Logger::Event::DRIVER_EVENT)),
             std::string("DRIVER_EVENT"));

    const std::string stamp = Logger::nowTimestamp();
    // YYYY-MM-DD HH:MM:SS.mmm
    CHECK_EQ(stamp.size(), std::size_t(23));
    CHECK(stamp[4] == '-' && stamp[7] == '-' && stamp[10] == ' ');
}

// ---------------------------------------------------------------------------
// 3. fork() / pipe() / waitpid()
// ---------------------------------------------------------------------------
SYSTEM_TEST(PipeAndForkChildWriteParentRead) {
    int fds[2] = {-1, -1};
    CHECK(::pipe(fds) == 0);
    if (fds[0] < 0 || fds[1] < 0) return;

    const pid_t pid = ::fork();
    CHECK(pid >= 0);

    if (pid == 0) {
        // ---- child -----------------------------------------------------
        ::close(fds[0]);
        const std::string msg = "hello from child";
        ssize_t written = 0;
        do {
            written = ::write(fds[1], msg.data(), msg.size());
        } while (written < 0 && errno == EINTR);
        ::close(fds[1]);
        _exit(0);
    }

    // ---- parent --------------------------------------------------------
    ::close(fds[1]);

    char buffer[64] = {0};
    const ssize_t bytesRead = ::read(fds[0], buffer, sizeof(buffer) - 1);
    ::close(fds[0]);

    CHECK_EQ(static_cast<long>(bytesRead),
             static_cast<long>(std::string("hello from child").size()));
    CHECK_EQ(std::string(buffer), std::string("hello from child"));

    int status = 0;
    const pid_t done = ::waitpid(pid, &status, 0);
    CHECK_EQ(done, pid);
    CHECK(WIFEXITED(status));
    CHECK_EQ(WEXITSTATUS(status), 0);
}

SYSTEM_TEST(ProcessManagerForksMonitorThatReceivesEvents) {
    const std::string logPath = "logs/library.log";

    int fds[2] = {-1, -1};
    CHECK(::pipe(fds) == 0);
    if (fds[0] < 0 || fds[1] < 0) return;

    ProcessManager pm;
    const pid_t child = pm.startLoggingProcess(fds[0], fds[1]);
    CHECK(child > 0);
    CHECK(child != ::getpid());
    CHECK(pm.isRunning());
    CHECK_EQ(pm.childPid(), child);

    // Give the child a moment to reach its read loop.
    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    CHECK(pm.sendEvent("BOOK_ISSUE:101:7"));
    CHECK(pm.sendEvent("MEMBER_ADD:7"));
    std::this_thread::sleep_for(std::chrono::milliseconds(250));

    pm.stopLoggingProcess();
    CHECK(!pm.isRunning());

    // The child must have written both events into the monitor log.
    std::ifstream in(logPath);
    std::string   content((std::istreambuf_iterator<char>(in)),
                          std::istreambuf_iterator<char>());

    CHECK(content.find("MONITOR pid=" + std::to_string(child)) != std::string::npos);
    CHECK(content.find("BOOK_ISSUE:101:7") != std::string::npos);
    CHECK(content.find("MEMBER_ADD:7") != std::string::npos);
}

SYSTEM_TEST(ProcessManagerChildIsReapedByWaitpid) {
    int fds[2] = {-1, -1};
    CHECK(::pipe(fds) == 0);
    if (fds[0] < 0 || fds[1] < 0) return;

    ProcessManager pm;
    const pid_t child = pm.startLoggingProcess(fds[0], fds[1]);
    CHECK(child > 0);
    if (child <= 0) return;

    pm.stopLoggingProcess();

    // After stopLoggingProcess() the child must already be reaped, so
    // waitpid() reports "no such child" rather than blocking forever.
    int   status = 0;
    pid_t result = ::waitpid(child, &status, WNOHANG);
    CHECK(result == -1);
    CHECK_EQ(errno, ECHILD);
}

// ---------------------------------------------------------------------------
// 4. FIFO (named pipe)
// ---------------------------------------------------------------------------
SYSTEM_TEST(FifoCreateAndTransferText) {
    const std::string path = "/tmp/library_test_fifo";

    ::unlink(path.c_str());
    CHECK(IpcPipe::create(path, 0666));

    struct stat st {};
    CHECK(::lstat(path.c_str(), &st) == 0);
    CHECK(S_ISFIFO(st.st_mode));      // it really is a named pipe

    // The reader must be opened first: a non-blocking write-only open of a
    // FIFO with no reader fails with ENXIO.
    IpcPipe reader;
    CHECK(reader.open(path, IpcPipe::Mode::READ, true));

    IpcPipe writer;
    CHECK(writer.open(path, IpcPipe::Mode::WRITE, true));

    CHECK(writer.writeLine("BOOK_ISSUE:101:7"));

    std::string received;
    bool got = false;
    for (int attempt = 0; attempt < 50 && !got; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        got = reader.readLine(received);
    }

    CHECK(got);
    CHECK_EQ(received, std::string("BOOK_ISSUE:101:7"));

    writer.close();
    reader.close();
    IpcPipe::remove(path);
}

SYSTEM_TEST(FifoSecondProcessConsumesMessages) {
    // The real IPC test: a forked child reads the FIFO and counts lines the
    // parent writes.  This is the same pattern the application uses.
    const std::string path = "/tmp/library_test_fifo_ipc";
    ::unlink(path.c_str());
    CHECK(IpcPipe::create(path, 0666));
    if (!IpcPipe::create(path, 0666)) return;

    const pid_t pid = ::fork();
    CHECK(pid >= 0);
    if (pid < 0) return;

    if (pid == 0) {
        IpcPipe childReader;
        if (!childReader.open(path, IpcPipe::Mode::READ, true)) _exit(1);

        int lines = 0;
        std::string line;
        // Bounded so a bug can never hang the test suite.
        for (int attempt = 0; attempt < 300 && lines < 3; ++attempt) {
            if (childReader.readLine(line)) {
                ++lines;
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }
        childReader.close();
        _exit(lines == 3 ? 0 : 1);
    }

    // Opening write-only BLOCKS until the child's reader is attached - this is
    // exactly the behaviour that makes a FIFO usable between two processes.
    IpcPipe parentWriter;
    CHECK(parentWriter.open(path, IpcPipe::Mode::WRITE, false));
    if (!parentWriter.isOpen()) {
        ::kill(pid, SIGKILL);
        ::waitpid(pid, nullptr, 0);
        IpcPipe::remove(path);
        return;
    }

    for (int i = 0; i < 3; ++i) {
        parentWriter.writeLine("SCAN book:" + std::to_string(100 + i));
    }
    parentWriter.close();

    int status = 0;
    CHECK(::waitpid(pid, &status, 0) == pid);
    CHECK(WIFEXITED(status));
    CHECK_EQ(WEXITSTATUS(status), 0);

    IpcPipe::remove(path);
}

// ---------------------------------------------------------------------------
// 5. Signals
// ---------------------------------------------------------------------------
namespace {

std::atomic<bool> g_usr1Fired{false};

} // namespace

SYSTEM_TEST(SignalHandlerCatchesSigusr1) {
    SignalHandler handler;
    handler.install();
    handler.setOnUsr1([]() { g_usr1Fired.store(true); });

    CHECK(!handler.usr1Requested());

    // Raise the signal from this very process: sigaction() delivers it to us.
    CHECK(::raise(SIGUSR1) == 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    CHECK(handler.usr1Requested());

    // The real work happens outside the handler, in the main loop.
    handler.runUsr1Callback();
    CHECK(g_usr1Fired.load());
    CHECK(!handler.usr1Requested());
}

SYSTEM_TEST(SignalHandlerCatchesSigintFromAnotherProcess) {
    SignalHandler handler;
    handler.install();

    CHECK(!handler.shouldStop());

    // Simulate Ctrl+C arriving from the terminal: a separate process sends it.
    const pid_t killer = ::fork();
    CHECK(killer >= 0);
    if (killer == 0) {
        ::usleep(50000);
        ::kill(::getppid(), SIGINT);
        _exit(0);
    }

    // Wait for the handler to flip the flag.
    for (int i = 0; i < 100 && !handler.shouldStop(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    int status = 0;
    ::waitpid(killer, &status, 0);

    CHECK(handler.shouldStop());

    handler.reset();
    CHECK(!handler.shouldStop());
}

SYSTEM_TEST(SignalHandlerInstallsTermAndIgnoresSigpipe) {
    SignalHandler handler;
    handler.install();

    struct sigaction current {};

    // SIGINT and SIGTERM must be handled, not defaulted.
    CHECK(::sigaction(SIGINT, nullptr, &current) == 0);
    CHECK(current.sa_handler != SIG_DFL);

    CHECK(::sigaction(SIGTERM, nullptr, &current) == 0);
    CHECK(current.sa_handler != SIG_DFL);

    // A dead FIFO reader must not kill the process.
    CHECK(::sigaction(SIGPIPE, nullptr, &current) == 0);
    CHECK(current.sa_handler == SIG_IGN);

    // The destructor must put the default disposition back.
    {
        SignalHandler temporary;
        temporary.install();
    }
    CHECK(::sigaction(SIGINT, nullptr, &current) == 0);
    CHECK(current.sa_handler == SIG_DFL);
}

// ---------------------------------------------------------------------------
// 6. Character device driver (must degrade gracefully)
// ---------------------------------------------------------------------------
SYSTEM_TEST(DriverClientReportsAvailability) {
    DriverClient client;

    if (client.isAvailable()) {
        // Module is loaded: exercise the real write / read path.
        const long long before = client.eventCount();
        CHECK(before >= 0);

        CHECK(client.sendEvent("TEST_EVENT:1:1"));
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        const long long after = client.eventCount();
        CHECK_EQ(after, before + 1);

        std::string report;
        CHECK(client.readReport(report));
        CHECK(report.find("events=") != std::string::npos);
        CHECK(report.find("latest=TEST_EVENT:1:1") != std::string::npos);
    } else {
        // Module not loaded: the application must still work.
        CHECK(client.eventCount() == -1);
        CHECK(!client.sendEvent("TEST_EVENT:1:1"));
        std::string report;
        CHECK(!client.readReport(report));
    }

    // Close / reopen must be safe in both cases.
    client.close();
    CHECK(!client.isAvailable());
    client.open();
}

SYSTEM_TEST(DriverClientRejectsOversizedEvents) {
    DriverClient client;
    if (!client.isAvailable()) return;

    const std::string huge(1000, 'A');
    CHECK(!client.sendEvent(huge));
    CHECK(!client.sendEvent(""));
    CHECK(client.sendEvent("BOOK_RETURN:102:7"));
}

// ---------------------------------------------------------------------------
// 7. Logging destination
// ---------------------------------------------------------------------------
SYSTEM_TEST(LoggerAppendsAcrossInstances) {
    const std::string path = "logs/library.log";

    {
        Logger a;
        a.log(Logger::Event::SYSTEM_START, "instance A");
        a.shutdown();
    }
    {
        Logger b;
        b.log(Logger::Event::SYSTEM_SHUTDOWN, "instance B");
        b.shutdown();
    }

    std::ifstream in(path);
    const std::string content((std::istreambuf_iterator<char>(in)),
                              std::istreambuf_iterator<char>());
    CHECK(content.find("instance A") != std::string::npos);
    CHECK(content.find("instance B") != std::string::npos);

    // The shutdown marker must be the last line written.
    const std::size_t lastShutdown = content.rfind("SYSTEM_SHUTDOWN");
    const std::size_t lastStart = content.rfind("SYSTEM_START");
    CHECK(lastShutdown != std::string::npos);
    CHECK(lastShutdown > lastStart);
}