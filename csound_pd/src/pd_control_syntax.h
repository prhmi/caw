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
inline std::vector<Output> outputs(const std::string& text) {
    static const std::regex row(
        R"pd(^[ \t]*#X[ \t]+obj[ \t]+[-+]?[0-9]+[ \t]+[-+]?[0-9]+[ \t]+(?:s|send)[ \t]+\\?\$0-(cawset|cawscore)-([A-Za-z0-9_-]+)[ \t]*;[ \t\r]*$)pd");
    std::set<std::pair<std::string,std::string>> names;
    std::istringstream input(text);std::string line;
    while(std::getline(input,line)) {
        std::smatch m;if(std::regex_match(line,m,row))names.emplace(m[1].str(),m[2].str());
    }
    std::vector<Output> result;for(const auto& name:names)result.push_back({name.first,name.second});
    return result;
}
}
