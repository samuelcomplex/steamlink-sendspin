// Receiver volume over HDMI-CEC: turns Music Assistant volume changes into volume up/down
// presses on the AV receiver, sent by the slcec helper that sits next to the player.
//
// CEC can't set or read an absolute volume on most receivers, only step it, so this follows
// how far the slider moved: `steps` presses for the whole 0-100 range. The first volume the
// server sends after the player starts is taken as where the receiver already is.
//
// Licensed under the Apache License, Version 2.0.
#pragma once

#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

namespace sl {

class CecVolume {
public:
    CecVolume(std::string helper, int steps);
    ~CecVolume();
    void set_volume(int volume);  // 0-100, from the server

private:
    void run_();
    std::string helper_;
    int steps_;
    std::mutex m_;
    std::condition_variable cv_;
    std::thread worker_;
    bool stop_{false};
    bool have_baseline_{false};
    int target_{0};  // receiver position wanted, in steps
    int sent_{0};    // receiver position we've stepped to
};

}  // namespace sl
