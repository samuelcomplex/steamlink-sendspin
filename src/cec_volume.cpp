// Licensed under the Apache License, Version 2.0.
#include "cec_volume.h"

#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>

namespace sl {

CecVolume::CecVolume(std::string helper, int steps) : helper_(std::move(helper)), steps_(steps) {
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
    if (!have_baseline_) {
        sent_ = pos;
        have_baseline_ = true;
        fprintf(stderr, ">>> receiver volume: following Music Assistant from %d%%\n", volume);
        return;
    }
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
        int rc = -1;
        pid_t pid = fork();
        if (pid == 0) {
            execl(helper_.c_str(), helper_.c_str(), cmd, count, (char*)nullptr);
            _exit(127);
        }
        if (pid > 0) {
            int status = 0;
            if (waitpid(pid, &status, 0) == pid && WIFEXITED(status)) rc = WEXITSTATUS(status);
        }

        lock.lock();
        if (rc == 0) {
            sent_ += delta > 0 ? n : -n;
        } else {
            fprintf(stderr, ">>> receiver volume: %s failed (%d); giving up on this change\n",
                    helper_.c_str(), rc);
            sent_ = target_;
        }
    }
}

}  // namespace sl
