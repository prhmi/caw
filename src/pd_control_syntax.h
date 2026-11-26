#pragma once
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace pd_control_syntax {
struct Output {std::string kind,name;};
// Vanilla PD object spelling. Patches store literal $0 as "\$0".
// Names are intentionally restricted to simple, unambiguous symbols.
inline std::vector<std::string> inputs(const std::string& text) {
    static const std::regex row(
        R"pd(^[ \t]*#X[ \t]+obj[ \t]+[-+]?[0-9]+[ \t]+[-+]?[0-9]+[ \t]+(?:r|receive)[ \t]+\\?\$0-cawget-([A-Za-z0-9_-]+)[ \t]*;[ \t\r]*$)pd");
    std::set<std::string> names;
    std::istringstream input(text);std::string line;
    while(std::getline(input,line)) {
        std::smatch m;
        if(std::regex_match(line,m,row))names.insert(m[1].str());
    }
    return {names.begin(),names.end()};
}
// CAW CABLES in a patch: the objects [cawr~ <name>] (receive a stereo cable) and [caws~ <name>]
// (send a stereo pair into a cable). They are small abstractions CAW installs beside the patch
// (cawr~.pd / caws~.pd); CAW gives every name a cable pair (1..8) and tells the abstraction which
// one through [r $0-...] - [adc~] stays the real microphone input, [dac~] the track's sound.
struct BusLink {std::string bus; int channel=1; bool send=false;};
inline std::vector<BusLink> busLinks(const std::string& text) {
    static const std::regex row(
        R"pd(^[ \t]*#X[ \t]+obj[ \t]+[-+]?[0-9]+[ \t]+[-+]?[0-9]+[ \t]+(cawr~|caws~)[ \t]+([A-Za-z0-9_-]+)[ \t]*[;,].*$)pd");
    std::vector<BusLink> out;
    std::istringstream input(text);std::string line;
    int nRecv=0,nSend=0;
    while(std::getline(input,line)) {
        if(!line.empty()&&line.back()=='\r')line.pop_back();
        std::smatch m;
        if(!std::regex_match(line,m,row))continue;
        BusLink b;b.send=m[1].str()=="caws~";b.bus=m[2].str();
        bool dup=false;for(const auto& o:out)if(o.send==b.send&&o.bus==b.bus)dup=true;
        if(dup)continue;
        int& n=b.send?nSend:nRecv;
        if(n>=8)continue;   // 8 receive cables and 8 send cables per patch
        b.channel=++n;      // the cable pair this name uses inside the patch (1..8)
        out.push_back(b);
    }
    return out;
}
inline std::vector<Output> outputs(const std::string& text) {
    static const std::regex row(
        R"pd(^[ \t]*#X[ \t]+obj[ \t]+[-+]?[0-9]+[ \t]+[-+]?[0-9]+[ \t]+(?:s|send)[ \t]+\\?\$0-(cawset|cawscore|caws)-([A-Za-z0-9_-]+)[ \t]*;[ \t\r]*$)pd");
    std::set<std::pair<std::string,std::string>> names;
    std::istringstream input(text);std::string line;
    while(std::getline(input,line)) {
        std::smatch m;if(std::regex_match(line,m,row))names.emplace(m[1].str(),m[2].str());
    }
    std::vector<Output> result;for(const auto& name:names)result.push_back({name.first,name.second});
    return result;
}
}
