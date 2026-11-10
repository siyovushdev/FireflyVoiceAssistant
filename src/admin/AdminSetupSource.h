#pragma once
#include "Json.h"
#include "../Text.h"
#include <algorithm>
#include <cctype>
#include <map>
#include <limits>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace firefly::admin::setup_source {
// This layer extracts explicit source relationships, not proposed AI values.
// Unknown/ambiguous relationships remain an error; a literal elsewhere in the
// document is never evidence for another device or the opposite power action.
inline std::string trim(std::string value) {
    const auto first=value.find_first_not_of(" \t\r\n:,-");
    if(first==std::string::npos)return {};
    value=value.substr(first);const auto last=value.find_last_not_of(" \t\r\n:,-");
    value.resize(last+1);return value;
}
// Numeric/source values may lose whitespace, never signs or punctuation.
inline std::string trimWhitespace(std::string value) {
    const auto first=value.find_first_not_of(" \t\r\n");
    if(first==std::string::npos)return {};
    const auto last=value.find_last_not_of(" \t\r\n");
    return value.substr(first,last-first+1);
}
inline std::string labels(std::string value) {
    for(const std::string quote:{"«","»","“","”","„"})
        for(std::size_t at=0;(at=value.find(quote,at))!=std::string::npos;at+=quote.size())
            value.replace(at,quote.size(),quote.size(),' ');
    return detail::lowercase(value);
}
inline std::string stem(std::string word) {
    for(const std::string suffix:{"иями","ями","ами","ого","ему","ому","ыми","ими","иях","ах","ях","ой","ей","ий","ый","ая","яя","ую","юю","ое","ее","ом","ем","ам","ям","ов","ев","ы","и","а","я","у","ю","е","о"})
        if(word.size()>suffix.size()+3&&word.compare(word.size()-suffix.size(),suffix.size(),suffix)==0) {
            word.resize(word.size()-suffix.size());break;
        }
    return word;
}
inline std::vector<std::string> words(std::string_view text) {
    auto result=detail::words(labels(std::string(text)));
    for(auto& word:result)word=stem(std::move(word));
    return result;
}
inline bool same(std::string_view a,std::string_view b) {return words(a)==words(b);}
inline bool mentions(std::string_view text,std::string_view name) {
    const auto a=words(text),b=words(name);
    return !b.empty()&&std::search(a.begin(),a.end(),b.begin(),b.end())!=a.end();
}
inline std::set<std::string> matches(std::string_view text,const std::vector<std::string>& names) {
    const auto a=words(text);struct Span {std::size_t first,last;std::string name;};std::vector<Span> spans;
    for(const auto& name:names) {
        const auto b=words(name);if(b.empty())continue;
        for(std::size_t i=0;i+b.size()<=a.size();++i)
            if(std::equal(b.begin(),b.end(),a.begin()+i))spans.push_back({i,i+b.size(),name});
    }
    std::set<std::string> result;
    for(const auto& s:spans)if(std::none_of(spans.begin(),spans.end(),[&](const Span& x) {
        return x.first<=s.first&&x.last>=s.last&&x.last-x.first>s.last-s.first;
    }))result.insert(s.name);
    return result;
}
// Delimit only outside quoted strings and JSON. Newlines and list bullets are
// optional formatting; sentences/semicolons preserve the same source scope.
inline std::vector<std::string> units(std::string_view input) {
    std::vector<std::string> result;std::size_t begin=0;int nesting=0;std::string close;
    static const std::vector<std::pair<std::string,std::string>> quotes={{"«","»"},{"“","”"},{"„","“"}};
    for(std::size_t i=0;i<input.size();++i) {
        if(!close.empty()) {
            if(input[i]=='\\') {if(i+1<input.size())++i;continue;}
            if(input.substr(i,close.size())==close) {i+=close.size()-1;close.clear();}continue;
        }
        if(input[i]=='"'||input[i]=='\'') {close=std::string(1,input[i]);continue;}
        bool quoted=false;
        for(const auto& pair:quotes)
            if(input.substr(i,pair.first.size())==pair.first) {close=pair.second;i+=pair.first.size()-1;quoted=true;break;}
        if(quoted)continue;
        if(input[i]=='{'||input[i]=='[')++nesting;
        if(input[i]=='}'||input[i]==']')--nesting;
        if(nesting)continue;
        const bool period=input[i]=='.'&&(i+1==input.size()||std::isspace(static_cast<unsigned char>(input[i+1])));
        const bool bullet=input[i]=='-'&&i>0&&std::isspace(static_cast<unsigned char>(input[i-1]))&&
            i+2<input.size()&&input[i+1]==' '&&!std::isdigit(static_cast<unsigned char>(input[i+2]));
        // A field value may be on the next line. Keep that original relation,
        // and preserve an empty ':' below so it cannot turn into a default.
        if(input[i]=='\n') {
            const auto before=input.substr(begin,i-begin);const auto last=before.find_last_not_of(" \t\r");
            const auto next=input.find_first_not_of(" \t\r\n",i+1);
            if(last!=std::string_view::npos&&(before[last]==':'||before[last]=='=')&&next!=std::string_view::npos&&
               (std::isdigit(static_cast<unsigned char>(input[next]))||input[next]=='-'||input[next]=='+'))continue;
        }
        if(input[i]=='\n'||input[i]==';'||input[i]=='!'||input[i]=='?'||period||bullet) {
            auto part=std::string(input.substr(begin,i-begin));
            const bool emptyField=part.find_last_not_of(" \t\r\n")==part.find_last_of(":=");
            part=trim(part);if(emptyField&&!part.empty())part+=":";if(!part.empty())result.push_back(std::move(part));begin=i+1;
        }
    }
    auto part=std::string(input.substr(begin));
    const bool emptyField=part.find_last_not_of(" \t\r\n")==part.find_last_of(":=");
    part=trim(part);if(emptyField&&!part.empty())part+=":";if(!part.empty())result.push_back(std::move(part));return result;
}
struct Target {
    std::string zone,name;
    std::string key() const {return Json::array({detail::lowercase(zone),detail::lowercase(name)}).dump();}
};
inline std::vector<bool> protectedValues(std::string_view text,bool jsonOnly=false);
inline bool equal(const Target& a,const Target& b) {return same(a.zone,b.zone)&&same(a.name,b.name);}
inline std::vector<std::string> quotedNames(std::string_view text) {
    std::vector<std::string> result;
    static const std::vector<std::pair<std::string,std::string>> quotes={{"«","»"},{"“","”"},{"\"","\""}};
    for(std::size_t i=0;i<text.size();++i)for(const auto& pair:quotes)
        if(text.substr(i,pair.first.size())==pair.first) {
            const auto end=text.find(pair.second,i+pair.first.size());if(end==std::string_view::npos)return result;
            result.push_back(trim(std::string(text.substr(i+pair.first.size(),end-i-pair.first.size()))));i=end+pair.second.size()-1;break;
        }
    return result;
}
struct Document {
    std::vector<Target> targets,declarations;
    std::map<std::string,std::string> facts;
    int statedTotal=-1;
    Document(std::string_view input,std::vector<Target> requested,std::vector<std::string> zones={}) : targets(std::move(requested)) {
        std::vector<std::string> names;for(const auto& t:targets) {names.push_back(t.name);if(!t.zone.empty())zones.push_back(t.zone);}
        std::string zone,current;
        for(const auto& unit:units(input)) {
            auto scopeText=unit;const auto jsonMask=protectedValues(unit,true);
            for(std::size_t i=0;i<scopeText.size();++i)if(jsonMask[i])scopeText[i]=' ';
            const auto lower=labels(scopeText);auto z=matches(scopeText,zones);
            // Explicit zone+device introductions are independent of inventory:
            // a missing second identically named device cannot disappear.
            const auto inZone=lower.find("в зоне ");
            if(inZone!=std::string::npos) {
                const auto add=lower.find("добав",inZone+12);
                if(add!=std::string::npos) {
                    auto explicitZone=trim(unit.substr(inZone+std::string("в зоне ").size(),add-inZone-std::string("в зоне ").size()));
                    const auto quoted=quotedNames(explicitZone);if(quoted.size()==1)explicitZone=quoted.front();
                    if(!explicitZone.empty()) {z.clear();z.insert(explicitZone);}
                }
            }
            if(z.size()==1) {
                if(!same(zone,*z.begin()))current.clear();
                zone=*z.begin();
            } else if(z.size()>1) {zone.clear();current.clear();}
            const auto n=matches(scopeText,names);
            if(n.size()==1) {
                std::vector<Target> candidates;
                for(const auto& t:targets)if(same(t.name,*n.begin())&&(zone.empty()||same(t.zone,zone)))candidates.push_back(t);
                if(candidates.size()==1)current=candidates.front().key();else current.clear();
            } else if(n.size()>1)current.clear();
            if(!current.empty())facts[current]+=unit+";\n";

            const std::regex create(R"((?:^|[ ,])(?:добавь|добавьте|создай|создайте)[ \t]+)");
            for(auto i=std::sregex_iterator(lower.begin(),lower.end(),create);i!=std::sregex_iterator();++i) {
                auto rest=unit.substr(static_cast<std::size_t>(i->position()+i->length()));auto fold=labels(rest);
                const auto object=fold.find("устройств");
                if(object==std::string::npos) {
                    if(fold.find("зон")==0||fold.find("комнат")==0||fold.find("подключ")==0||fold.find("действ")==0||fold.find("фраз")==0)continue;
                } else {
                    const auto end=fold.find_first_of(" :\t",object);if(end==std::string::npos)continue;
                    rest=trim(rest.substr(end));fold=labels(rest);
                }
                // Device names belong to the introduction, never to a later
                // JSON body or quoted payload in the same sentence.
                auto header=rest;std::smatch capability;
                const auto headerFold=labels(header);
                if(std::regex_search(headerFold,capability,std::regex(R"((?:^|[ ,])(?:включать|выключать|включение|выключение|управляется|управлять|подключение|с подключением|без управления|без действий|без зоны|mqtt|http|modbus|которое|который|оно)[ ,:]?)")))
                    header.resize(static_cast<std::size_t>(capability.position()));
                auto declared=quotedNames(header);
                if(declared.empty()) {
                    std::string list=header;
                    for(std::size_t at=0;(at=labels(list).find(" и ",at))!=std::string::npos;)list.replace(at,std::string(" и ").size(),",");
                    std::istringstream parts(list);std::string bare;
                    while(std::getline(parts,bare,',')) {
                        bare=trim(bare);
                        for(const std::string prefix:{"туда ","там ","новый ","новую ","новое "})if(labels(bare).find(prefix)==0)bare=trim(bare.substr(prefix.size()));
                        for(const auto& known:zones)if(labels(bare).find(labels(known)+" ")==0)bare=trim(bare.substr(known.size()));
                        for(const std::string preposition:{" в "," на "}) {
                            const auto at=labels(bare).find(preposition);
                            if(at!=std::string::npos&&matches(bare.substr(at+preposition.size()),zones).size()==1)bare=trim(bare.substr(0,at));
                        }
                        if(!bare.empty())declared.push_back(bare);
                    }
                }
                for(auto name:declared) {
                    if(name.empty()||std::any_of(zones.begin(),zones.end(),[&](const std::string& v){return same(v,name);}))continue;
                    Target declaration{zone,std::move(name)};
                    if(std::none_of(declarations.begin(),declarations.end(),[&](const Target& t){return equal(t,declaration);}))declarations.push_back(std::move(declaration));
                }
            }
            if(lower.find("итог")!=std::string::npos||lower.find("всего")!=std::string::npos) {
                const std::regex count(R"((одно|один|два|две|три|четыре|пять|шесть|семь|восемь|девять|десять|[0-9]+)\s+(?:новых?\s+)?устройств)");
                const std::map<std::string,int> numbers={{"одно",1},{"один",1},{"два",2},{"две",2},{"три",3},{"четыре",4},{"пять",5},{"шесть",6},{"семь",7},{"восемь",8},{"девять",9},{"десять",10}};
                std::smatch match;if(std::regex_search(lower,match,count))statedTotal=numbers.count(match.str(1))?numbers.at(match.str(1)):std::stoi(match.str(1));
            }
        }
    }
    std::string forTarget(const Target& target) const {
        const auto i=facts.find(target.key());return i==facts.end()?std::string():i->second;
    }
    void requireInventory(const std::vector<Target>& proposed) const {
        for(const auto& d:declarations) {
            const bool covered=std::any_of(proposed.begin(),proposed.end(),[&](const Target& t) {
                return same(t.name,d.name)&&(d.zone.empty()||same(t.zone,d.zone));
            });
            if(!covered)throw std::invalid_argument("incomplete_setup_document: пропущено устройство «"+d.name+"»; уточните полный список устройств и зон.");
        }
        if(statedTotal>=0&&static_cast<int>(proposed.size())!=statedTotal)
            throw std::invalid_argument("incomplete_setup_document: число устройств не соответствует явному итогу запроса.");
    }
};

struct RolePart {bool on;std::string text;};
inline std::vector<bool> protectedValues(std::string_view text,bool jsonOnly) {
    std::vector<bool> mask(text.size(),false);std::string close;int depth=0;
    static const std::vector<std::pair<std::string,std::string>> quotes={{"«","»"},{"“","”"},{"\"","\""},{"'","'"}};
    for(std::size_t i=0;i<text.size();++i) {
        if(!close.empty()) {
            mask[i]=!jsonOnly||depth>0;
            if(text[i]=='\\'&&i+1<text.size()) {mask[++i]=!jsonOnly||depth>0;continue;}
            if(text.substr(i,close.size())==close) {for(std::size_t n=1;n<close.size();++n)mask[i+n]=!jsonOnly||depth>0;i+=close.size()-1;close.clear();}
            continue;
        }
        bool opened=false;
        for(const auto& quote:quotes)if(text.substr(i,quote.first.size())==quote.first) {
            close=quote.second;for(std::size_t n=0;n<quote.first.size();++n)mask[i+n]=!jsonOnly||depth>0;i+=quote.first.size()-1;opened=true;break;
        }
        if(opened)continue;
        if(text[i]=='{'||text[i]=='[')++depth;
        if(depth)mask[i]=true;
        if((text[i]=='}'||text[i]==']')&&depth)--depth;
    }
    return mask;
}
// Preserve offsets while excluding quoted/JSON data from instruction syntax.
inline std::string instructionText(std::string_view source) {
    auto text=labels(std::string(source));const auto mask=protectedValues(source);
    for(std::size_t i=0;i<text.size();++i)if(mask[i])text[i]=' ';
    return text;
}
inline std::vector<RolePart> powerParts(std::string_view input) {
    std::vector<RolePart> result;
    for(const auto& unit:units(input)) {
        const auto lower=labels(unit);const auto protectedText=protectedValues(unit);
        struct Marker {std::size_t at;bool on;};std::vector<Marker> markers;
        const std::regex token(R"([^ \t\r\n,;:.!?()\[\]{}"']+)");
        std::string previous;
        for(auto i=std::sregex_iterator(lower.begin(),lower.end(),token);i!=std::sregex_iterator();++i) {
            if(protectedText[static_cast<std::size_t>(i->position())])continue;
            const auto word=i->str();int role=-1;
            if(word.find("выключ")==0||word.find("отключ")==0)role=0;
            else if(word.find("включ")==0)role=1;
            else if((word=="on"||word=="off")&&(i->position()==0||
                (previous!="payload"&&previous!="text"&&previous!="текст"&&previous!="value"&&previous!="—"&&previous!="-"&&previous!="="))) {
                const auto end=static_cast<std::size_t>(i->position()+i->length());
                const auto tail=trim(lower.substr(end));
                if(tail.find("function")==0||tail.find("fc")==0||tail.find("value")==0||tail.find("payload")==0||
                   tail.find("включ")==0||tail.find("выключ")==0||tail.find("означает")==0)role=word=="on"?1:0;
            }
            if(role>=0&&previous!="не"&&previous!="без") {
                if(markers.empty()||markers.back().on!=(role==1))markers.push_back({static_cast<std::size_t>(i->position()),role==1});
            }
            previous=word;
        }
        for(std::size_t i=0;i<markers.size();++i) {
            const auto end=i+1<markers.size()?markers[i+1].at:unit.size();
            result.push_back({markers[i].on,unit.substr(markers[i].at,end-markers[i].at)});
        }
    }
    return result;
}
inline std::string actionFacts(std::string_view source,bool on) {
    std::string result;bool reading=false;
    for(auto unit:units(source)) {
        auto s=labels(unit);const auto protectedText=protectedValues(unit);
        for(std::size_t i=0;i<s.size();++i)if(protectedText[i])s[i]=' ';
        const bool command=s.find("включение")!=std::string::npos||s.find("выключение")!=std::string::npos||
            s.find("для включения")!=std::string::npos||s.find("для выключения")!=std::string::npos||s.find("запись")!=std::string::npos||s.find("топик команд")!=std::string::npos;
        if(command)reading=false;
        std::size_t readAt=std::string::npos;
        for(const std::string word:{"состояни","чтен","означает","true_payload","false_payload"})readAt=std::min(readAt,s.find(word));
        if(readAt!=std::string::npos) {
            if(!reading&&readAt>0)unit.resize(readAt);else unit.clear();reading=true;
        } else if(reading)unit.clear();
        for(const auto& p:powerParts(unit))if(p.on==on)result+=p.text+";\n";
    }
    return result;
}
inline std::string escapedRegex(std::string_view value) {
    std::string out;for(const char c:value) {if(std::string_view("\\.^$|()[]{}*+?").find(c)!=std::string_view::npos)out+='\\';out+=c;}return out;
}
inline std::size_t quotedEnd(std::string_view source,std::size_t start,const std::string& close) {
    for(auto i=start;i<source.size();++i) {
        if((close=="\""||close=="'")&&source[i]=='\\') {if(i+1<source.size())++i;continue;}
        if(source.substr(i,close.size())==close)return i+close.size();
    }
    throw std::invalid_argument("Incomplete source value");
}
inline std::string nextValue(std::string_view source) {
    std::size_t at=0;
    while(at<source.size()&&(std::isspace(static_cast<unsigned char>(source[at]))||source[at]==':'||source[at]=='='))++at;
    for(const std::string dash:{"—","–","-"})if(source.substr(at,dash.size())==dash&&
        !(dash=="-"&&at+1<source.size()&&std::isdigit(static_cast<unsigned char>(source[at+1])))) {at+=dash.size();break;}
    while(at<source.size()&&std::isspace(static_cast<unsigned char>(source[at])))++at;
    for(const auto& quote:std::vector<std::pair<std::string,std::string>>{{"«","»"},{"“","”"},{"\"","\""},{"'","'"}})
        if(source.substr(at,quote.first.size())==quote.first) {
            const auto end=quotedEnd(source,at+quote.first.size(),quote.second);
            if(quote.first=="\"")return Json::parse(std::string(source.substr(at,end-at))).get<std::string>();
            return std::string(source.substr(at+quote.first.size(),end-at-quote.first.size()-quote.second.size()));
        }
    const auto end=source.find_first_of(" \t\r\n,;",at);return std::string(source.substr(at,end-at));
}
inline bool quotedLiteral(std::string_view source,const std::string& value) {
    for(std::size_t at=0;at<source.size();++at) {
        for(const auto& quote:std::vector<std::pair<std::string,std::string>>{{"«","»"},{"“","”"},{"\"","\""},{"'","'"}})
            if(source.substr(at,quote.first.size())==quote.first) {
                try {
                    const auto end=quotedEnd(source,at+quote.first.size(),quote.second);
                    if(nextValue(source.substr(at,end-at))==value)return true;
                    at=end-1;
                } catch(const std::exception&) {return false;}
                break;
            }
    }
    return false;
}
// Return the whole lexical value before a relation word, including quoted
// spaces/escapes. Regex token suffixes are not wire payloads.
inline std::string valueBefore(std::string_view source,std::size_t end) {
    std::size_t last=std::string_view::npos,lastEnd=0;
    for(std::size_t at=0;at<end;) {
        if(std::isspace(static_cast<unsigned char>(source[at]))||source[at]==','||source[at]==';') {++at;continue;}
        last=at;bool quoted=false;
        for(const auto& quote:std::vector<std::pair<std::string,std::string>>{{"«","»"},{"“","”"},{"\"","\""},{"'","'"}})
            if(source.substr(at,quote.first.size())==quote.first) {
                at=quotedEnd(source,at+quote.first.size(),quote.second);quoted=true;break;
            }
        if(!quoted)while(at<end&&!std::isspace(static_cast<unsigned char>(source[at]))&&source[at]!=','&&source[at]!=';')++at;
        if(at>end)throw std::invalid_argument("Incomplete source relation");
        lastEnd=at;
    }
    return last==std::string_view::npos?std::string():nextValue(source.substr(last,lastEnd-last));
}
// Scan whole string tokens: a quoted key followed by ':' is syntax;
// words inside its quoted value are never recursively scanned as keys.
inline std::set<std::string> quotedAssignments(std::string_view source,const std::string& label) {
    std::set<std::string> result;int depth=0;std::size_t object=0;bool relevant=false;
    const std::regex key("(?:"+label+")");
    for(std::size_t i=0;i<source.size();++i) {
        if(source[i]=='{'||source[i]=='[') {if(depth==0){object=i;relevant=false;}++depth;}
        if(source[i]=='}'||source[i]==']') {
            if(depth>0&&--depth==0&&relevant)
                if(!Json::parse(std::string(source.substr(object,i-object+1))).is_structured())
                    throw std::invalid_argument("Structured field object required");
        }
        for(const auto& quote:std::vector<std::pair<std::string,std::string>>{{"«","»"},{"“","”"},{"\"","\""},{"'","'"}})
            if(source.substr(i,quote.first.size())==quote.first) {
                const auto end=quotedEnd(source,i+quote.first.size(),quote.second);
                const auto after=source.find_first_not_of(" \t\r\n",end);
                const auto name=nextValue(source.substr(i,end-i));
                if(depth<=1&&after!=std::string_view::npos&&source[after]==':'&&std::regex_match(detail::lowercase(name),key)) {
                    const auto start=source.find_first_not_of(" \t\r\n",after+1);
                    std::string value;
                    if(start!=std::string_view::npos) {
                        const auto tail=source.substr(start);bool quoted=false;
                        for(const std::string opening:{"«","“","\"","'"})
                            if(tail.substr(0,opening.size())==opening)quoted=true;
                        // Structured scalars are not prose: no removal of '-',
                        // separators, or empty values before numeric validation.
                        value=quoted?nextValue(tail):std::string(tail.substr(0,tail.find_first_of(" \t\r\n,;} ]")));
                    }
                    result.insert(value); // Presence and an empty value are distinct.
                    if(depth==1)relevant=true;
                }
                i=end-1;break;
            }
    }
    if(depth>0&&relevant)throw std::invalid_argument("Incomplete structured field object");
    return result;
}
inline std::set<std::string> assignedValues(std::string_view source,const std::string& label) {
    const auto folded=detail::lowercase(source);const auto mask=protectedValues(source);auto result=quotedAssignments(source,label);
    const std::regex field("(?:^|[ \\t\\r\\n,;{\\\"'])("+label+")[\\\"']?(?:[ \\t\\r\\n:=]+)");
    for(auto i=std::sregex_iterator(folded.begin(),folded.end(),field);i!=std::sregex_iterator();++i) {
        if(mask[static_cast<std::size_t>(i->position(1))])continue;
        const auto value=nextValue(source.substr(static_cast<std::size_t>(i->position()+i->length())));
        result.insert(value);
    }
    return result;
}
inline std::set<std::string> powerValues(std::string_view source,bool mqtt) {
    auto result=assignedValues(source,mqtt?"payload|точный текст|текст":"value|значение");
    if(!result.empty())return result;
    for(const auto& unit:units(source)) {
        const auto folded=labels(unit);const auto first=folded.find_first_of(" \t:=—–");
        const auto head=folded.substr(0,first);
        if(mqtt&&(head=="on"||head=="off")) {
            if(folded.find("включает")!=std::string::npos||folded.find("выключает")!=std::string::npos)
                result.insert(unit.substr(0,first));
        } else if(first!=std::string::npos&&(head=="включение"||head=="выключение"||head=="отключение")) {
            const auto value=nextValue(std::string_view(unit).substr(first));
            if(value!="и"&&value!="или"&&value!="не"&&!value.empty())result.insert(value);
        }
    }
    return result;
}
inline void requirePowerValue(const Json& value,std::string_view source,bool mqtt) {
    const auto expected=powerValues(source,mqtt);const auto actual=mqtt&&value.is_string()?value.get<std::string>():value.dump();
    if(expected.size()!=1||!expected.count(actual))throw std::invalid_argument("unstated_action_mapping: значение не соответствует явно заданному включению/выключению.");
}
inline std::vector<Json> bodyObjects(std::string_view source) {
    std::vector<Json> result;bool quoted=false,escape=false;int level=0;std::size_t begin=0;
    for(std::size_t i=0;i<source.size();++i) {
        const char c=source[i];if(escape){escape=false;continue;}
        if(quoted&&c=='\\'){escape=true;continue;}
        if(c=='"'){quoted=!quoted;continue;}if(quoted)continue;
        if(c=='{') {if(!level)begin=i;++level;}
        if(c=='}'&&level&&!--level)result.push_back(Json::parse(std::string(source.substr(begin,i-begin+1))));
    }
    if(level)throw std::invalid_argument("Incomplete source HTTP JSON body");
    return result;
}
inline void requireHttpBody(const Json& body,std::string_view source) {
    const auto objects=bodyObjects(source);
    if(!objects.empty()) {
        if(std::any_of(objects.begin(),objects.end(),[&](const Json& x){return x!=body;}))
            throw std::invalid_argument("unstated_action_mapping: HTTP body не соответствует JSON этого действия.");
        return;
    }
    if(body.empty())throw std::invalid_argument("Укажите HTTP body этого действия явно.");
    for(const auto& item:body.items()) {
        const std::regex assignment("(?:^|[^A-Za-z0-9_])"+escapedRegex(item.key())+"[ \\t\\\"':=]+"+escapedRegex(item.value().dump())+"(?:$|[^A-Za-z0-9_.])");
        if(!std::regex_search(std::string(source),assignment))throw std::invalid_argument("unstated_action_mapping: HTTP field/value не связаны в описании действия.");
    }
}
inline bool requestsModbusRead(std::string_view source) {
    for(const auto& unit:units(source)) {
        const auto s=labels(unit);
        if((s.find("чтен")!=std::string::npos||s.find("читать")!=std::string::npos||s.find("состояни")!=std::string::npos)&&
           s.find("не знаю")==std::string::npos&&s.find("не нужно")==std::string::npos&&s.find("не задан")==std::string::npos&&
           s.find("неизвест")==std::string::npos&&s.find("без чтения")==std::string::npos&&s.find("не читать")==std::string::npos)
            return true;
    }
    return false;
}
inline std::vector<int> fieldNumbers(std::string_view source,const std::string& field) {
    std::vector<int> values;const auto s=labels(std::string(source));
    const auto mask=protectedValues(source);
    const std::regex pattern("(?:^|[ \t\r\n,;(])(?:"+field+")(?=[ \t\r\n\"':=]|$)[ \t\r\n\"':=]*");
    for(auto i=std::sregex_iterator(s.begin(),s.end(),pattern);i!=std::sregex_iterator();++i) {
        if(mask[static_cast<std::size_t>(i->position())])continue;
        const auto tail=s.substr(static_cast<std::size_t>(i->position()+i->length()));std::smatch n;
        if(!std::regex_search(tail,n,std::regex(R"(^([0-9]+)(?=$|[ \t\r\n,;.)]))"))||
           (n.length()<static_cast<int>(tail.size())&&(tail[n.length()]=='.'||tail[n.length()]==',')&&
            n.length()+1<static_cast<int>(tail.size())&&std::isdigit(static_cast<unsigned char>(tail[n.length()+1]))))
            throw std::invalid_argument("Укажите целое неотрицательное числовое поле: "+field);
        const auto value=std::stoll(n.str(1));
        if(value>std::numeric_limits<int>::max())throw std::invalid_argument("Числовое поле вне диапазона: "+field);
        values.push_back(static_cast<int>(value));
    }
    return values;
}
inline void requireModbusWrite(const Json& command,std::string_view source,bool on) {
    const auto specific=actionFacts(source,on);std::string common;bool reading=false;
    for(const auto& unit:units(source)) {
        const auto s=labels(unit);
        if(s.find("чтен")!=std::string::npos||s.find("читать")!=std::string::npos||s.find("состояни")!=std::string::npos)reading=true;
        if(s.find("запись")!=std::string::npos)reading=false;
        if(!reading&&powerParts(unit).empty())common+=unit+";\n";
    }
    const auto check=[&](const std::string& field,const char* name) {
        auto values=fieldNumbers(specific,field);if(values.empty())values=fieldNumbers(common,field);
        const std::set<int> unique(values.begin(),values.end());
        if(unique.size()!=1||command.at(name)!=*unique.begin())
            throw std::invalid_argument(std::string("unstated_modbus_write: проверьте ")+name+" именно этой записи.");
    };
    check(R"((?:function|fc|функцию|функция|функции))","function");
    check(R"((?:address|адрес регистра|адрес катушки|адрес coil|адрес(?=[ \t]*(?::|=|[0-9+-]|$))))","address");
    const auto function=command.at("function").get<int>();
    if(function==5&&command.at("data_type")!="boolean")throw std::invalid_argument("Invalid Modbus coil write type");
    if(function==6&&!mentions(specific+common,command.at("data_type").get<std::string>()))
        throw std::invalid_argument("Укажите тип регистра этой записи явно.");
    requirePowerValue(command.at("value"),specific,false);
}
inline std::string mqttReadFacts(std::string_view source) {
    std::string result;bool reading=false;
    for(const auto& unit:units(source)) {
        const auto s=instructionText(unit);
        if(s.find("состояни")!=std::string::npos||s.find("true_payload")!=std::string::npos||s.find("false_payload")!=std::string::npos||!quotedAssignments(unit,"true_payload|false_payload").empty())reading=true;
        if(s.find("топик команд")!=std::string::npos||s.find("включение")!=std::string::npos||s.find("выключение")!=std::string::npos)reading=false;
        if(reading)result+=unit+";\n";
    }
    return result;
}
inline void requireMqttRead(const Json& observation,std::string_view source) {
    const auto read=mqttReadFacts(source);const auto folded=labels(read);
    std::set<std::string> yes=assignedValues(read,"true_payload"),no=assignedValues(read,"false_payload");
    // The wire token before 'означает' and its boolean meaning are separate
    // roles: neither the command mapping nor English ON/OFF implies the state.
    const auto protectedText=protectedValues(read);
    const std::regex meaning(R"((?:^|[ \t\r\n])(означает)[ \t\r\n]+(выключено|отключено|включено)(?=$|[ \t\r\n,;.]))");
    for(auto i=std::sregex_iterator(folded.begin(),folded.end(),meaning);i!=std::sregex_iterator();++i) {
        const auto at=static_cast<std::size_t>(i->position(1));
        if(protectedText[at])continue;
        auto value=valueBefore(read,at);
        (i->str(2)=="включено"?yes:no).insert(value);
    }
    // Existing compact text notation explicitly lists true/false in that order.
    if(yes.empty()&&no.empty()&&instructionText(read).find("on/off")!=std::string::npos) {yes.insert("ON");no.insert("OFF");}
    if(yes.size()!=1||no.size()!=1||observation.at("true_payload")!=*yes.begin()||observation.at("false_payload")!=*no.begin())
        throw std::invalid_argument("unstated_mqtt_read: payload состояния не соответствует явно заданному смыслу true/false.");
}
inline bool selectionPredicate(std::string_view text) {
    for(const std::string word:{"использ","примен","подключ","управля","читать","читай","через ","без "})
        if(text.find(word)!=std::string_view::npos)return true;
    return false;
}
inline std::string withoutConjunction(std::string text) {
    text=trim(text);
    for(const std::string prefix:{"и ","а ","но "})
        if(text.find(prefix)==0)return trim(text.substr(prefix.size()));
    return text;
}
inline std::vector<std::string> selectionClauses(std::string_view source) {
    std::vector<std::string> result;
    std::vector<std::string> coordinated;
    for(const auto& unit:units(source)) {
        const auto folded=instructionText(unit);std::size_t begin=0;
        for(std::size_t i=0;i<folded.size();++i) {
            std::size_t skip=0;
            for(const std::string conjunction:{" а "," но "," и "})if(folded.compare(i,conjunction.size(),conjunction)==0) {
                const auto tail=std::string_view(folded).substr(i+conjunction.size());
                if(conjunction!=" и ")skip=conjunction.size();
                else for(const std::string predicate:{"не ","использ","примен","подключ","через "})
                    if(tail.substr(0,predicate.size())==predicate)skip=conjunction.size();
            }
            if(skip) {const auto part=trim(unit.substr(begin,i-begin));if(!part.empty())coordinated.push_back(part);begin=i+skip;i=begin-1;}
        }
        const auto part=trim(unit.substr(begin));if(!part.empty())coordinated.push_back(part);
    }
    for(const auto& unit:coordinated) {
        const auto folded=instructionText(unit);const auto mask=protectedValues(unit);std::size_t begin=0;
        for(std::size_t i=0;i<unit.size();++i) {
            if(mask[i])continue;
            std::size_t skip=0;
            const bool decimalComma=unit[i]==','&&i>0&&i+1<unit.size()&&std::isdigit(static_cast<unsigned char>(unit[i-1]))&&std::isdigit(static_cast<unsigned char>(unit[i+1]));
            if(unit[i]==','&&!decimalComma) {
                const auto left=trim(folded.substr(begin,i-begin));
                const auto right=withoutConjunction(folded.substr(i+1));
                // A comma alone does not end a shared predicate/list. Keep
                // "MQTT, HTTP, Modbus не используй" and prefix prohibitions
                // intact; separate explicit predicates and contrasts.
                if(right.find("не ")==0||
                   (selectionPredicate(left)&&selectionPredicate(right)))skip=1;
            }
            if(skip) {const auto part=trim(unit.substr(begin,i-begin));if(!part.empty())result.push_back(part);begin=i+skip;i=begin-1;}
        }
        const auto part=trim(unit.substr(begin));if(!part.empty())result.push_back(part);
    }
    return result;
}
inline bool forbiddenSelection(std::string_view source) {
    const auto s=withoutConjunction(instructionText(source));
    return s.find("не использ")!=std::string::npos||s.find("не примен")!=std::string::npos||
        s.find("не подключ")!=std::string::npos||s.find("не читать")!=std::string::npos||s.find("не читай")!=std::string::npos||
        s.find("без ")==0||s.find("не ")==0;
}
inline std::pair<std::set<std::string>,std::set<std::string>> transports(std::string_view source) {
    std::set<std::string> positive,forbidden;
    for(const auto& unit:selectionClauses(source)) {
        const auto text=instructionText(unit);
        auto& values=forbiddenSelection(text)?forbidden:positive;
        if(mentions(text,"MQTT"))values.insert("mqtt");
        if(mentions(text,"HTTP")||text.find("http://")!=std::string::npos)values.insert("http");
        if(mentions(text,"Modbus")) {if(mentions(text,"TCP"))values.insert("modbus_tcp");if(mentions(text,"RTU"))values.insert("modbus_rtu");}
    }
    return {positive,forbidden};
}
inline std::vector<int> qosNumbers(std::string_view source) {
    std::vector<int> result;
    const auto add=[&](std::string value) {
        value=trimWhitespace(value);
        if(!value.empty()&&value[0]=='"')value=nextValue(value);
        const auto values=fieldNumbers("qos "+value,"qos");
        if(values.empty())throw std::invalid_argument("Укажите целый QoS 0 или 1.");
        result.push_back(values[0]);
    };
    for(const auto& value:quotedAssignments(source,"qos"))add(value);
    const auto syntax=instructionText(source);
    const std::regex label(R"((?:^|[ \t\r\n,;(])(qos)(?=$|[ \t\r\n:=]))");
    for(auto i=std::sregex_iterator(syntax.begin(),syntax.end(),label);i!=std::sregex_iterator();++i) {
        auto tail=trimWhitespace(std::string(source.substr(static_cast<std::size_t>(i->position(1)+i->length(1)))));
        const auto folded=detail::lowercase(tail);
        // A mention in "измени QoS состояния" is not an empty declaration.
        // A delimiter/value marker, including one with a missing RHS, is.
        if(!tail.empty()&&(tail[0]==':'||tail[0]=='='))add(tail.substr(1));
        else if(!tail.empty()&&(std::isdigit(static_cast<unsigned char>(tail[0]))||tail[0]=='-'||tail[0]=='+'||tail[0]=='"'))add(tail);
        else if(folded.find("на ")==0||folded=="на")add(tail.substr(std::string("на").size()));
        else if(const auto marker=folded.find(" на ");marker!=std::string::npos)add(tail.substr(marker+std::string(" на ").size()));
        else if(folded.size()>=std::string(" на").size()&&folded.compare(folded.size()-std::string(" на").size(),std::string(" на").size()," на")==0)add("");
        else if(const auto colon=tail.find(':');colon!=std::string::npos) {
            // Explicit structured QoS is independently validated above.
            if(quotedAssignments(tail,"qos").empty())add(tail.substr(colon+1));
        } else if(trimWhitespace(syntax)=="qos")add("");
    }
    return result;
}
inline void requireMqttQos(const Json& actual,std::string_view source,bool observation) {
    std::set<int> common,selected,forbidden;int role=0;
    for(const auto& unit:units(source)) {
        for(const auto& clause:selectionClauses(unit)) {
            // Resolve scope after splitting predicates: a later command clause
            // must not overwrite the state role of an earlier QoS declaration.
            const auto syntax=instructionText(clause);
            if(syntax.find("состояни")!=std::string::npos||syntax.find("чтен")!=std::string::npos)role=1;
            if(syntax.find("для команд")!=std::string::npos||syntax.find("qos команд")!=std::string::npos||syntax.find("топик команд")!=std::string::npos||syntax.find("включение")!=std::string::npos||syntax.find("выключение")!=std::string::npos)role=2;
            if(role&&role!=(observation?1:2))continue;
            const auto numbers=qosNumbers(clause);
            auto& values=forbiddenSelection(clause)?forbidden:(role?selected:common);
            values.insert(numbers.begin(),numbers.end());
        }
    }
    if(selected.empty())selected=common;
    if(forbidden.count(actual.get<int>())||(!selected.empty()&&(selected.size()!=1||actual!=*selected.begin())))
        throw std::invalid_argument("unstated_mqtt_qos: QoS не соответствует явно заданному значению/запрету.");
}
inline void requireModbusRead(const Json& observation,std::string_view source) {
    if(observation.is_null()) {
        if(requestsModbusRead(source))throw std::invalid_argument("incomplete_setup_document: явно запрошенное Modbus-чтение отсутствует.");
        return;
    }
    std::string read,positive,forbidden;bool reading=false;
    for(const auto& unit:selectionClauses(source)) {
        const auto s=labels(unit);
        if(forbiddenSelection(s)) {forbidden+=unit+";\n";continue;}
        positive+=unit+";\n";
        if(s.find("чтен")!=std::string::npos||s.find("читать")!=std::string::npos||s.find("состояни")!=std::string::npos)reading=true;
        if(s.find("запись")!=std::string::npos||!powerParts(unit).empty())reading=false;
        if(reading)read+=unit+";\n";
    }
    auto functions=fieldNumbers(read,R"((?:function|fc|функцию|функция|функции))");
    auto addresses=fieldNumbers(read,R"((?:address|адрес регистра|адрес катушки|адрес coil|адрес(?=[ \t]*(?::|=|[0-9+-]|$))))");
    if(addresses.empty())addresses=fieldNumbers(positive,R"((?:address|адрес регистра|адрес катушки|адрес coil))");
    const auto check=[&](const std::vector<int>& values,const char* name) {
        const std::set<int> unique(values.begin(),values.end());
        if(unique.size()!=1||!observation.contains(name)||observation.at(name)!=*unique.begin())
            throw std::invalid_argument(std::string("unstated_modbus_read: проверьте ")+name+" независимого чтения.");
    };
    check(functions,"function");check(addresses,"address");
    for(const auto& field:std::vector<std::pair<std::string,std::string>>{{"(?:function|fc|функцию|функция|функции)","function"},{"(?:address|адрес регистра|адрес катушки|адрес coil)","address"}})
        for(const auto value:fieldNumbers(forbidden,field.first))if(observation.at(field.second)==value)
            throw std::invalid_argument("forbidden_modbus_read: запрещённое поле чтения "+field.second);
    if((observation.at("function")==1||observation.at("function")==2)&&observation.value("data_type",std::string())!="boolean")
        throw std::invalid_argument("Invalid Modbus coil read type");
    if((observation.at("function")==3||observation.at("function")==4)&&!mentions(read,observation.at("data_type").get<std::string>()))
        throw std::invalid_argument("Укажите тип регистра независимого чтения явно.");
}
} // namespace firefly::admin::setup_source
