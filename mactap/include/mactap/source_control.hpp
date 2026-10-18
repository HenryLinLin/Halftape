#pragma once
#include <string>

namespace ws {
void pauseSourceApp(const std::string& bundle);
// command: 0 previous track, 1 play/pause, 2 next track
void transportSourceApp(const std::string& bundle, int command);
}
