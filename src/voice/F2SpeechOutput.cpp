#include "firefly/voice/F2SpeechOutput.h"
#include "F2Worker.h"
#include "F2StockData.h"
#include "../config/Platform.h"
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace firefly::voice {
namespace {
// Exact files used by the accepted F2 pilot. Large weights remain external;
// a different voice/model cannot be selected through user configuration.
void verifyAssets(const std::string& directory) {
    struct Pin { const char* file; std::size_t bytes; const char* sha; };
    const Pin pins[] = {
        {"duration_predictor.int8.onnx",3700147,"c3eb91414d5ff8a7a239b7fe9e34e7e2bf8a8140d8375ffb14718b1c639325db"},
        {"text_encoder.int8.onnx",36416150,"c7befd5ea8c3119769e8a6c1486c4edc6a3bc8365c67621c881bbb774b9902ff"},
        {"vector_estimator.int8.onnx",78400833,"20cd86fa5c6effedfda0e7cffe5b0569ca401c440a0c3a1d72bf39286c0db3fd"},
        {"vocoder.int8.onnx",25991073,"e923d60f53f95eb1ce235f1dc33ec56d9c057823c96fa6f8acf98f32b0da6152"},
        {"tts.json",8253,"42078d3aef1cd43ab43021f3c54f47d2d75ceb4e75f627f118890128b06a0d09"},
        {"unicode_indexer.bin",262144,"8402ca48e5189a8950138580b0fff64db6f072f24ac07cd54ba8b2fbb9883b30"},
        {"voice.bin",517168,"67d5209b0ee8ce6c74105ffbe12fe6a7628aea3b4ba2fcb308a4a67938a93ce8"}
    };
    const std::filesystem::path root(directory);
    if (!root.is_absolute() || std::filesystem::canonical(root) != root || !std::filesystem::is_directory(root))
        throw std::runtime_error("tts_asset_directory_invalid");
    struct stat directoryInfo{};
    if (::stat(root.c_str(), &directoryInfo) != 0 || (directoryInfo.st_mode & 0022) ||
        (directoryInfo.st_uid != 0 && directoryInfo.st_uid != ::geteuid()))
        throw std::runtime_error("tts_asset_directory_unsafe");
    for (const auto& pin : pins) {
        config::detail::FileDescriptor fd(::open((root/pin.file).c_str(), O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK));
        struct stat info{};
        if(fd.get()<0||::fstat(fd.get(),&info)!=0||!S_ISREG(info.st_mode)||info.st_size!=static_cast<off_t>(pin.bytes)||
           (info.st_mode&0022)||(info.st_uid!=0&&info.st_uid!=::geteuid()))throw std::runtime_error("tts_asset_unavailable_or_unsafe");
        std::string bytes(pin.bytes,'\0');std::size_t offset=0;
        while(offset<bytes.size()) {
            auto n=::read(fd.get(),bytes.data()+offset,bytes.size()-offset);
            if(n<0&&errno==EINTR)continue;
            if(n<=0)throw std::runtime_error("tts_asset_read_failed");
            offset+=static_cast<std::size_t>(n);
        }
        if(config::detail::sha256(bytes)!=pin.sha)throw std::runtime_error("tts_asset_sha256_mismatch");
    }
}
}
struct F2SpeechOutput::Impl {
    f2::Events events;
    f2::Speaker speaker;
    int turn=0;
    Impl(const std::string& model,const std::string& playback,Events sink,const std::string& wav,LoadGuard guard)
        : events(std::move(sink)),speaker(events,model,wav,playback,!wav.empty(),true,f2::stockAudio,
            [model,guard=std::move(guard)](const std::function<void()>& load) {
                const auto verifiedLoad=[&]{verifyAssets(model);load();};
                if(guard)guard(verifiedLoad);else verifiedLoad();
            }){}
};
F2SpeechOutput::F2SpeechOutput(const std::string& model,const std::string& playback,Events events,const std::string& wav,LoadGuard guard) {
    impl_=std::make_unique<Impl>(model,playback,std::move(events),wav,std::move(guard));
}
F2SpeechOutput::~F2SpeechOutput()=default;
void F2SpeechOutput::cancel(){impl_->speaker.cancel();}
SpeechOutcome F2SpeechOutput::speak(const std::string& text,const std::function<bool()>& interrupted) {
    if(text.empty()||text.size()>4096)return {false,false,"speech_text_limit"};
    if(interrupted&&interrupted())return {false,true,{}};
    impl_->speaker.begin(++impl_->turn);
    impl_->speaker.push(text);
    const bool ok=impl_->speaker.finish(true,interrupted);
    const auto error=impl_->speaker.error();
    return {ok,impl_->speaker.cancelled()&&error.empty(),error};
}
}
