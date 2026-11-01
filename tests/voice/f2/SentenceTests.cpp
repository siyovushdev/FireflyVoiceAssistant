#include "F2Sentences.h"
using firefly::voice::f2::Sentences;
#include <iostream>
#include <vector>
int main() {
    struct Case {std::string text;std::vector<std::string> expected;};
    std::vector<Case> cases={
        {"Добрый вечер. Я готова помочь.",{"Добрый вечер.","Я готова помочь."}},
        {"<think>SECRET. <think>NESTED.</think> HIDDEN.</think>Ответ.",{"Ответ."}},
        {"&lt;think&gt;SECRET.&lt;/think&gt;Ответ.",{"Ответ."}},
        {"До.\n```kotlin\nSECRET.\n```\nПосле.",{"До.","После."}},
        {"До. ~~~python\nSECRET.\n~~~ После.",{"До.","После."}},
        {"До `SECRET.` после.",{"До после."}},
        {"<think>SECRET.",{}},
        {"```python\nSECRET.",{}},
        {"Цена 3.14 рубля. Всё.",{"Цена 3.14 рубля.","Всё."}},
        {"<laugh>Здравствуйте.<sigh>",{"Здравствуйте."}},
        {"Просто ответ без точки",{"Просто ответ без точки"}},
        {"## Заголовок\n**Ответ.**",{"Заголовок","Ответ."}},
        {"https://example.com Нужный текст.",{"Нужный текст."}},
        {"Сначала закончим обсуждение вопроса, затем продолжим.",{"Сначала закончим обсуждение вопроса,","затем продолжим."}},
    };
    int checks=0;
    for(const auto& c:cases)for(size_t split=0;split<=c.text.size();++split) {
        std::vector<std::string> result;
        Sentences s([&](std::string x){result.push_back(std::move(x));});
        s.push(c.text.substr(0,split));s.push(c.text.substr(split));s.finish();
        if(result!=c.expected){std::cerr<<"Wrong split "<<split<<" for "<<c.text<<"\n";for(auto& x:result)std::cerr<<"["<<x<<"]\n";return 1;}
        ++checks;
    }
    for(const auto& c:cases) {
        std::vector<std::string> result;
        Sentences s([&](std::string x){result.push_back(std::move(x));});
        for(char ch:c.text)s.push(std::string(1,ch));
        s.finish();
        if(result!=c.expected){std::cerr<<"Wrong byte stream: "<<c.text<<'\n';return 1;}++checks;
    }
    bool early=false;
    Sentences s([&](std::string x){early=x=="Первая фраза.";});
    s.push("Первая фраза. Вторая ещё генерируется");
    if(!early)return 1;
    ++checks;
    std::vector<std::string> longChunks;
    Sentences longText([&](std::string x){longChunks.push_back(std::move(x));});
    longText.push("Небо кажется голубым из-за рассеивания солнечного света в атмосфере Земли и сейчас предложение ещё продолжается");
    if(longChunks.empty()||longChunks.front().size()>110)return 1;
    ++checks;
    for(const std::string text:{"Питание: `off` <think>нет</think>.","Значение 3.14. Нет."}) {
        for(size_t split=0;split<=text.size();++split) {
            std::string result;
            Sentences plain([&](std::string chunk){if(!result.empty())result+=' ';result+=chunk;},true);
            plain.push(text.substr(0,split));plain.push(text.substr(split));plain.finish();
            if(result!=text){std::cerr<<"Plain facts changed at split "<<split<<'\n';return 1;}++checks;
        }
    }
    std::cout<<"PASS "<<checks<<" incremental speech checks (all byte/token boundaries)\n";
}
