#include "firefly/voice/WakeAcknowledgement.h"
#include <chrono>
#include <iostream>
#include <thread>
using namespace firefly::voice;
int main(int argc,char** argv) {
    AlsaWakeAcknowledgement beep(argc>1?argv[1]:"plughw:CARD=rockchipes8388,DEV=0");
    beep.start();
    auto status=AcknowledgementStatus::Playing;
    while(status==AcknowledgementStatus::Playing) { status=beep.poll();std::this_thread::sleep_for(std::chrono::milliseconds(5)); }
    beep.cancel();
    if(status==AcknowledgementStatus::Failed) { std::cerr<<beep.error()<<'\n';return 1; }
    std::cout<<"ALSA playback open/write/drain/close passed (audibility not asserted)\n";
}
