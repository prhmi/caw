#pragma once
#include <algorithm>
#include <cmath>
#include <map>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

// Numeric subset of CAW's p-field run rules. It is an experimental utility:
// it does not send data to libpd and is not yet wired to the clip editor.
// Csound expression rules are deliberately rejected, not misinterpreted.
namespace pd_pfield_numeric {
struct Rule { int field = 0; double from = 0.0, value = 0.0; char op = 0; bool interpolate = false; };
inline bool parse(const std::string& text, std::vector<Rule>& rules, std::string& error) {
    static const std::regex line(
        R"re(^\s*\{\s*p\s*:\s*(\d+)\s*,\s*from\s*:\s*([-+]?(?:\d+\.?\d*|\.\d+)(?:[eE][-+]?\d+)?)\s*(?:,\s*(?:(to)\s*:\s*|([-+*/])\s*)([-+]?(?:\d+\.?\d*|\.\d+)(?:[eE][-+]?\d+)?))?\s*\}\s*$)re");
    std::vector<Rule> parsed; std::map<int,bool> used; std::istringstream stream(text);
    std::string row; int rowNo=0;
    while (std::getline(stream,row)) {
        ++rowNo;
        if (row.find_first_not_of(" \t\r\n")==std::string::npos) continue;
        std::smatch match;
        if (!std::regex_match(row,match,line)) { error="Unsupported p-field rule on line "+std::to_string(rowNo)+"."; return false; }
        Rule rule;
        try {
            rule.field=std::stoi(match[1].str());rule.from=std::stod(match[2].str());
            if(match[5].matched)rule.value=std::stod(match[5].str());
        } catch (...) { error="Invalid numeric p-field value."; return false; }
        if(rule.field<4||rule.field>60||!std::isfinite(rule.from)||!std::isfinite(rule.value)||used[rule.field]) {
            error="P-fields must be unique, finite and numbered 4 through 60.";return false;
        }
        if(match[3].matched)rule.interpolate=true;
        if(match[4].matched)rule.op=match[4].str()[0];
        if(rule.op=='/' && rule.value==0.0) {error="Division by zero in p-field rule.";return false;}
        used[rule.field]=true;parsed.push_back(rule);
    }
    rules=std::move(parsed);error.clear();return true;
}
inline bool value(const Rule& rule,int run,int runs,double& result) {
    if(runs<1||run<0||run>=runs)return false;
    result=rule.from;
    if(rule.interpolate)result=rule.from+(rule.value-rule.from)*run/std::max(runs-1,1);
    else if(rule.op=='+')result=rule.from+run*rule.value;
    else if(rule.op=='-')result=rule.from-run*rule.value;
    else if(rule.op=='*')result=rule.from*std::pow(rule.value,run);
    else if(rule.op=='/')result=rule.from/std::pow(rule.value,run);
    return std::isfinite(result);
}
}
