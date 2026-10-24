// Licensed under the Apache License, Version 2.0.
#include "cec_volume.h"

#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>

extern char** environ;

namespace sl {

CecVolume::CecVolume(std::string helper, int steps, int initial_volume)
    : helper_(std::move(helper)), steps_(steps) {
    if (initial_volume < 0) initial_volume = 0;
    if (initial_volume > 100) initial_volume = 100;
    target_ = sent_ = (initial_volume * steps_ + 50) / 100;
    worker_ = std::thread([this] { run_(); });
}

CecVolume::~CecVolume() {
    {
        std::lock_guard<std::mutex> lock(m_);
        stop_ = true;
    }
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
}

void CecVolume::set_volume(int volume) {
    if (volume < 0) volume = 0;
    if (volume > 100) volume = 100;
    int pos = (volume * steps_ + 50) / 100;
    std::lock_guard<std::mutex> lock(m_);
    target_ = pos;
    cv_.notify_all();
}

// Sends the presses in small batches, so turning the slider the other way mid-move takes
// effect quickly; each press takes about a quarter of a second on the wire.
void CecVolume::run_() {
    std::unique_lock<std::mutex> lock(m_);
    for (;;) {
        cv_.wait(lock, [this] { return stop_ || target_ != sent_; });
        if (stop_) return;
        int delta = target_ - sent_;
        int n = delta > 0 ? delta : -delta;
        if (n > 8) n = 8;
        const char* cmd = delta > 0 ? "volup" : "voldown";
        lock.unlock();

        char count[8];
        snprintf(count, sizeof count, "%d", n);
        // vfork-style spawn: a plain fork() would have to duplicate the player's large
        // audio mappings and can fail on the Steam Link's small memory.
        int rc = -1;
        pid_t pid = -1;
        char* args[] = {const_cast<char*>(helper_.c_str()), const_cast<char*>(cmd), count, nullptr};
        posix_spawnattr_t attr;
        posix_spawnattr_init(&attr);
#ifdef POSIX_SPAWN_USEVFORK
        posix_spawnattr_setflags(&attr, POSIX_SPAWN_USEVFORK);
#endif
        int err = posix_spawn(&pid, helper_.c_str(), nullptr, &attr, args, environ);
        posix_spawnattr_destroy(&attr);
        if (err != 0) {
            pid = -1;
            rc = 1000 + err;
        }
        if (pid > 0) {
            int status = 0;
            if (waitpid(pid, &status, 0) == pid && WIFEXITED(status)) rc = WEXITSTATUS(status);
        }

        lock.lock();
        if (rc == 0) {
            sent_ += delta > 0 ? n : -n;
            fprintf(stderr, ">>> receiver volume: %s %d\n", cmd, n);
        } else {
            fprintf(stderr, ">>> receiver volume: %s failed (%d); giving up on this change\n",
                    helper_.c_str(), rc);
            sent_ = target_;
        }
    }
}

}  // namespace sl
