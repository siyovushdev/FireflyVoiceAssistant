#pragma once
#include <algorithm>
#include <cctype>
#include <functional>
#include <regex>
#include <stdexcept>
#include <string>

// Incremental speech-only view. Original Qwen output is retained separately.
namespace firefly::voice::f2 {
class Sentences {
public:
    explicit Sentences(std::function<void(std::string)> emit, bool plain = false): emit_(std::move(emit)), plain_(plain) {}
    void push(const std::string& bytes) {
        if (input_.size()+bytes.size()>16384) throw std::runtime_error("Speech input limit");
        input_+=bytes; consume(false);
    }
    void finish() { consume(true); if(!code_&&!hidden_) flush(); input_.clear(); }
private:
    std::function<void(std::string)> emit_;
    bool plain_;
    std::string input_,sentence_;
    size_t spoken_=0;
    int code_=0,hidden_=0;
    int chunks_=0;
    char fence_='`';
    static std::string clean(std::string s) {
        s=std::regex_replace(s,std::regex("https?://[^\\s]+")," ");
        s=std::regex_replace(s,std::regex("!\\[[^\\]]*\\]\\([^)]*\\)")," ");
        s=std::regex_replace(s,std::regex("\\[([^\\]]+)\\]\\([^)]*\\)"),"$1");
        for(char& c:s)if(c=='*'||c=='_'||c=='#'||static_cast<unsigned char>(c)<32)c=' ';
        s=std::regex_replace(s,std::regex("[ \\t\\r\\n]+")," ");
        auto first=s.find_first_not_of(' '),last=s.find_last_not_of(' ');
        return first==std::string::npos?"":s.substr(first,last-first+1);
    }
    void flush() {
        auto s=plain_?std::move(sentence_):clean(std::move(sentence_));sentence_.clear();
        if(plain_) { auto first=s.find_first_not_of(" \t\r\n"),last=s.find_last_not_of(" \t\r\n");s=first==std::string::npos?"":s.substr(first,last-first+1); }
        if(!s.empty()&&spoken_<1200){
            size_t end=0;
            for(;end<s.size();++end)if((static_cast<unsigned char>(s[end])&0xc0)!=0x80&&++spoken_>1200)break;
            s.resize(end);
            if(!s.empty()){emit_(std::move(s));++chunks_;}
        }
    }
    void consume(bool final) {
        // Operational replies are plain data assembled from verified facts,
        // never LLM prose. Do not interpret a configured value as markup.
        if(plain_) {
            size_t i=0;
            while(i<input_.size()) {
                const char c=input_[i];
                if((c=='.'||c=='!'||c=='?')&&i+1==input_.size()&&!final)break;
                sentence_+=c;++i;
                const bool decimal=c=='.'&&sentence_.size()>1&&std::isdigit(static_cast<unsigned char>(sentence_[sentence_.size()-2]))&&i<input_.size()&&std::isdigit(static_cast<unsigned char>(input_[i]));
                if(((c=='.'||c=='!'||c=='?')&&!decimal&&(i==input_.size()||std::isspace(static_cast<unsigned char>(input_[i]))))||c=='\n'||(sentence_.size()>=96&&c==' '))flush();
            }
            input_.erase(0,i);return;
        }
        for(const auto& entry: {std::pair<std::string,std::string>{"&lt;","<"},{"&gt;",">"},{"&quot;","\""},{"&apos;","'"},{"&amp;","&"}}) {
            size_t pos=0;
            while((pos=input_.find(entry.first,pos))!=std::string::npos){input_.replace(pos,entry.first.size(),entry.second);pos+=entry.second.size();}
        }
        size_t i=0;
        while(i<input_.size()) {
            char c=input_[i];
            if(c=='&'&&!code_) {
                auto end=input_.find(';',i);
                if(end==std::string::npos&&!final&&input_.size()-i<12)break;
                if(end!=std::string::npos&&end-i<12) {
                    auto e=input_.substr(i,end-i+1);std::string replacement;
                    if(e=="&lt;")replacement="<";else if(e=="&gt;")replacement=">";
                    else if(e=="&quot;")replacement="\"";else if(e=="&apos;")replacement="'";
                    else if(e=="&amp;")replacement="&";
                    if(!replacement.empty()){input_.replace(i,e.size(),replacement);c=input_[i];}
                }
            }
            if(c=='`'||c=='~') {
                size_t end=i;while(end<input_.size()&&input_[end]==c)++end;
                if(end==input_.size()&&!final)break;
                int count=static_cast<int>(end-i);
                if(!code_&&(c=='`'||count>=3)){code_=count;fence_=c;sentence_+=' ';}
                else if(code_&&c==fence_&&count>=code_)code_=0;
                i=end;continue;
            }
            if(code_){++i;continue;}
            if(c=='<') {
                auto end=input_.find('>',i);
                if(end==std::string::npos){if(final)i=input_.size();break;}
                auto tag=input_.substr(i,end-i+1);
                std::transform(tag.begin(),tag.end(),tag.begin(),[](unsigned char x){return std::tolower(x);});
                if(tag=="<think>"||tag=="<analysis>"||tag=="<reasoning>")++hidden_;
                else if(tag=="</think>"||tag=="</analysis>"||tag=="</reasoning>") {
                    if(hidden_)--hidden_;else sentence_.clear();
                }
                i=end+1;continue;
            }
            if(hidden_){++i;continue;}
            if((c=='.'||c=='!'||c=='?')&&i+1==input_.size()&&!final)break;
            sentence_+=c;++i;
            if(c=='.'||c=='!'||c=='?'||c=='\n') {
                // Decimal points are not sentence boundaries.
                bool decimal=c=='.'&&sentence_.size()>1&&std::isdigit(static_cast<unsigned char>(sentence_[sentence_.size()-2]))&&i<input_.size()&&std::isdigit(static_cast<unsigned char>(input_[i]));
                bool boundary=i==input_.size()||std::isspace(static_cast<unsigned char>(input_[i]))||c=='\n';
                if(!decimal&&boundary)flush();
            }
            if((c==','||c==':'||c==';')&&sentence_.size()>=64&&
               !(i<input_.size()&&std::isdigit(static_cast<unsigned char>(input_[i]))))flush();
            // Bound a sentence without punctuation without splitting UTF-8.
            if(sentence_.size()>=72u&&c==' ') {
                // The short first phrase lowers response latency. A comma
                // retains continuation intonation; original model text stays intact.
                if(!sentence_.empty())sentence_.insert(sentence_.size()-1,",");
                flush();
            }
        }
        input_.erase(0,i);
    }
};

}
