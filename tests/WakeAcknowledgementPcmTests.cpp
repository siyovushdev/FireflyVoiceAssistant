#include "firefly/voice/WakeAcknowledgement.h"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>

using namespace firefly::voice;
namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
void tone(int volume) {
    const auto pcm = makeWakeAcknowledgementPcm({true, volume});
    require(pcm.size() == 16000 * 400 / 1000, "Tone must last 400 ms at 16 kHz");
    require(pcm.front() == 0 && pcm.back() == 0, "Tone endpoints must be silent");
    const double peak = 32767.0 * volume / 100;
    int maximum = 0, positiveCrossings = 0;
    double energy = 0;
    for (size_t i = 0; i < pcm.size(); ++i) {
        maximum = std::max(maximum, std::abs(int(pcm[i])));
        require(std::abs(int(pcm[i])) <= peak, "PCM exceeds selected amplitude/clips");
        if (i < 160) require(std::abs(int(pcm[i])) <= peak * i / 160, "Missing fade in");
        if (i >= pcm.size() - 160)
            require(std::abs(int(pcm[i])) <= peak * (pcm.size() - 1 - i) / 160, "Missing fade out");
        if (i && pcm[i - 1] <= 0 && pcm[i] > 0) ++positiveCrossings;
        if (i >= 160 && i < pcm.size() - 160) energy += double(pcm[i]) * pcm[i];
    }
    require(maximum >= peak - 2, "Selected volume not reached");
    require(positiveCrossings == 352, "Tone frequency must be 880 Hz");
    const double rms = std::sqrt(energy / (pcm.size() - 320));
    require(std::abs(rms - peak / std::sqrt(2.0)) < 2, "Tone plateau energy incorrect");
}
}
int main() {
    try {
        WakeAcknowledgementConfig config;
        require(config.enabled && config.volumePercent == 30, "Wrong audible defaults");
        for (int volume : {1, 12, 30}) tone(volume);
        require(makeWakeAcknowledgementPcm({false, 30}).empty(), "Disabled beep generated PCM");
        for (int volume : {-1, 0, 31, 100}) {
            for (bool enabled : {false, true}) {
                bool rejected = false;
                try { makeWakeAcknowledgementPcm({enabled, volume}); }
                catch (const std::invalid_argument&) { rejected = true; }
                require(rejected, "Invalid volume accepted");
            }
        }
        std::cout << "13 PCM/settings cases passed: duration, frequency, envelope, amplitude and rejection\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
