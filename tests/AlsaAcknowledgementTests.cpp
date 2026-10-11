// Real AlsaWakeAcknowledgement.cpp, fake C API. No libasound or hardware access.
#include "firefly/voice/WakeAcknowledgement.h"
#include <alsa/asoundlib.h>
#include <algorithm>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace firefly::voice;
namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
struct FakePcm {
    int opens = 0, closes = 0, drops = 0, writes = 0, starts = 0, drains = 0;
    bool automaticStart = false;
    std::string failure;
    std::vector<int16_t> received;
} fake;
int token;
}
extern "C" {
int snd_pcm_open(snd_pcm_t** pcm, const char*, snd_pcm_stream_t stream, int mode) {
    ++fake.opens;
    require(stream == SND_PCM_STREAM_PLAYBACK && mode == SND_PCM_NONBLOCK, "Blocking/non-playback ALSA open");
    if (fake.failure == "open") return -ENODEV;
    *pcm = reinterpret_cast<snd_pcm_t*>(&token);
    return 0;
}
int snd_pcm_set_params(snd_pcm_t*, snd_pcm_format_t format, snd_pcm_access_t access,
    unsigned channels, unsigned rate, int resample, unsigned latency) {
    require(format == SND_PCM_FORMAT_S16_LE && access == SND_PCM_ACCESS_RW_INTERLEAVED &&
        channels == 1 && rate == 16000 && resample == 1 && latency == 100000, "ALSA format changed");
    return fake.failure == "setup" ? -EINVAL : 0;
}
int snd_pcm_drop(snd_pcm_t*) { ++fake.drops; return 0; }
int snd_pcm_close(snd_pcm_t*) { ++fake.closes; return 0; }
const char* snd_strerror(int) { return "injected ALSA error"; }
snd_pcm_state_t snd_pcm_state(snd_pcm_t*) {
    return fake.automaticStart ? SND_PCM_STATE_RUNNING : SND_PCM_STATE_PREPARED;
}
int snd_pcm_start(snd_pcm_t*) { ++fake.starts; return fake.failure == "start" ? -EIO : 0; }
snd_pcm_sframes_t snd_pcm_writei(snd_pcm_t*, const void* data, snd_pcm_uframes_t frames) {
    ++fake.writes;
    if (fake.failure == "write") return -EPIPE;
    if (fake.failure == "stalled" || fake.writes == 1) return -EAGAIN;
    const auto count = std::min<snd_pcm_uframes_t>(frames, 1024);
    const auto* pcm = static_cast<const int16_t*>(data);
    fake.received.insert(fake.received.end(), pcm, pcm + count);
    return count;
}
int snd_pcm_drain(snd_pcm_t*) {
    ++fake.drains;
    if (fake.failure == "drain") return -EIO;
    return fake.drains < 3 ? -EAGAIN : 0;
}
}
int main() {
    try {
        for (bool automaticStart : {false, true}) {
            fake = {}; fake.automaticStart = automaticStart;
            AlsaWakeAcknowledgement beep("fixture");
            beep.start();
            require(beep.poll() == AcknowledgementStatus::Playing && fake.received.empty(), "EAGAIN mishandled");
            auto status = AcknowledgementStatus::Playing;
            for (int i = 0; i < 20 && status == AcknowledgementStatus::Playing; ++i) status = beep.poll();
            require(status == AcknowledgementStatus::Complete && beep.error().empty(), "Playback did not finish");
            require(fake.received == makeWakeAcknowledgementPcm({}), "Partial writes lost/duplicated PCM");
            require(fake.starts == (automaticStart ? 0 : 1) && fake.drains == 3, "Start/drain contract failed");
            require(fake.closes == 1 && fake.drops == 1, "Playback leaked PCM handle");
            beep.cancel();
            require(fake.closes == 1, "Cancel closed handle twice");
            require(beep.poll() == AcknowledgementStatus::Complete, "Completed playback replayed");
        }
        for (const char* failure : {"open", "setup", "write", "start", "drain"}) {
            fake = {}; fake.failure = failure;
            AlsaWakeAcknowledgement beep("fixture");
            beep.start();
            auto status = AcknowledgementStatus::Playing;
            for (int i = 0; i < 20 && status == AcknowledgementStatus::Playing; ++i) status = beep.poll();
            require(status == AcknowledgementStatus::Failed && !beep.error().empty(), "ALSA failure hidden");
            const int writes = fake.writes;
            require(beep.poll() == AcknowledgementStatus::Failed && fake.writes == writes, "Failed playback replayed");
            require(fake.closes == (fake.failure == "open" ? 0 : 1), "Failure leaked/double closed handle");
            fake.failure.clear(); fake.writes = fake.drains = 0; fake.received.clear();
            beep.start();
            require(beep.error().empty(), "Retry retained stale error");
            beep.cancel();
        }
        for (bool stalled : {false, true}) {
            fake = {}; if (stalled) fake.failure = "stalled";
            AlsaWakeAcknowledgement beep("fixture");
            beep.start();
            if (stalled) require(beep.poll() == AcknowledgementStatus::Playing, "Stalled playback not exercised");
            beep.cancel(); beep.cancel();
            require(fake.drops == 1 && fake.closes == 1 && beep.poll() == AcknowledgementStatus::Complete,
                "Immediate/stalled cancellation did not release playback");
        }
        fake = {};
        {
            AlsaWakeAcknowledgement beep("fixture");
            beep.start();
        }
        require(fake.closes == 1, "Destructor leaked playback");
        fake = {}; fake.failure = "stalled";
        {
            AlsaWakeAcknowledgement beep("fixture");
            beep.start();
            require(beep.poll() == AcknowledgementStatus::Playing, "Deadline case not playing");
            std::this_thread::sleep_for(std::chrono::milliseconds(1010));
            require(beep.poll() == AcknowledgementStatus::Failed &&
                beep.error() == "ALSA acknowledgement deadline exceeded" && fake.closes == 1,
                "Playback deadline did not fail/close");
        }
        fake = {};
        AlsaWakeAcknowledgement disabled("fixture", {false, 30});
        disabled.start();
        require(disabled.poll() == AcknowledgementStatus::Complete && fake.opens == 0, "Disabled beep opened ALSA");
        std::cout << "12 fake ALSA cases passed: nonblocking partial writes/drain, failures, retry, cancel, deadline and RAII\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
