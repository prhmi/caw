#pragma once
#include <algorithm>
#include <regex>
#include <sstream>
#include <string>
#include <vector>
#include "pd_patch_editor.h"

// Conservative editor for flat, conventional PD canvases only. Unsupported
// records refuse editing rather than guessing object indices or rewriting data.
namespace pd_graph_edit {
struct Connection {int from=0,outlet=0,to=0,inlet=0;std::size_t line=0;};
struct Graph {std::vector<std::string> lines,objects;std::vector<std::size_t> objectLines;std::vector<Connection> wires;std::string newline="\n";};
inline bool parse(const std::string& source,Graph& g,std::string& error) {
    g=Graph{};bool root=false,seenConnections=false;
    static const std::regex object(R"pd(^#X[ \t]+(obj|msg|text|floatatom|symbolatom|listbox)[ \t]+[-+]?[0-9]+[ \t]+[-+]?[0-9]+[ \t]+.*;[ \t]*$)pd");
    static const std::regex connect(R"pd(^#X[ \t]+connect[ \t]+([0-9]+)[ \t]+([0-9]+)[ \t]+([0-9]+)[ \t]+([0-9]+)[ \t]*;[ \t]*$)pd");
    std::size_t pos=0;
    while(pos<source.size()) {
        auto end=source.find('\n',pos);if(end==std::string::npos)end=source.size();
        auto raw=source.substr(pos,end-pos);
        bool cr=!raw.empty()&&raw.back()=='\r';
        if(cr){raw.pop_back();g.newline="\r\n";}
        const auto line=g.lines.size();g.lines.push_back(source.substr(pos,(end<source.size()?end+1:end)-pos));
        std::smatch m;
        if(raw.rfind("#N canvas ",0)==0) {
            if(root){error="Nested canvases need the Pure Data app.";return false;}root=true;
        } else if(std::regex_match(raw,m,object)) {
            if(seenConnections){error="Objects after connections need the Pure Data app.";return false;}
            g.objectLines.push_back(line);g.objects.push_back(raw);
        } else if(std::regex_match(raw,m,connect)) {
            seenConnections=true;
            try {g.wires.push_back({std::stoi(m[1]),std::stoi(m[2]),std::stoi(m[3]),std::stoi(m[4]),line});}
            catch(...){error="Connection index is too large.";return false;}
        } else if(raw.rfind("#X coords ",0)==0 || raw.empty()) {
            // Keep canvas display metadata and blank lines untouched.
        } else {
            error="This patch has unsupported records. Edit its graph in Pure Data.";return false;
        }
        pos=end<source.size()?end+1:end;
    }
    if(!root){error="Missing PD canvas.";return false;}
    for(const auto& c:g.wires)
        if(c.from<0||c.to<0||c.from>=(int)g.objects.size()||c.to>=(int)g.objects.size()){
            error="A connection references an unknown object.";return false;
        }
    error.clear();return true;
}
inline std::string join(const Graph& g) {std::string out;for(const auto& l:g.lines)out+=l;return out;}
inline bool safeContent(const std::string& text) {
    if(text.empty())return false;
    for(std::size_t i=0;i<text.size();++i){
        unsigned char c=static_cast<unsigned char>(text[i]);
        if(c<32 || c==127)return false;
        if(c==';') {std::size_t n=0,j=i;while(j>0&&text[j-1]=='\\'){--j;++n;}if(n%2==0)return false;}
    }
    return true;
}
inline bool addObject(const std::string& source,const std::string& kind,int x,int y,
                      const std::string& content,std::string& output,std::string& error) {
    Graph g;if(!parse(source,g,error))return false;
    if((kind!="obj"&&kind!="msg"&&kind!="text")||!safeContent(content)){
        error="Enter a nonempty single-line PD object, message or comment; escape semicolons.";return false;
    }
    std::size_t where=g.lines.size();
    if(!g.wires.empty())where=g.wires.front().line;
    else for(std::size_t i=0;i<g.lines.size();++i)if(g.lines[i].rfind("#X coords ",0)==0){where=i;break;}
    if(where>0&&!g.lines[where-1].empty()&&g.lines[where-1].back()!='\n')g.lines[where-1]+=g.newline;
    g.lines.insert(g.lines.begin()+static_cast<std::ptrdiff_t>(where),
        "#X "+kind+" "+std::to_string(x)+" "+std::to_string(y)+" "+pd_patch_editor::escapeDollarArguments(content)+";"+g.newline);
    output=join(g);error.clear();return true;
}
inline bool addConnection(const std::string& source,int from,int outlet,int to,int inlet,
                          std::string& output,std::string& error) {
    Graph g;if(!parse(source,g,error))return false;
    if(from<0||to<0||from>=(int)g.objects.size()||to>=(int)g.objects.size()||outlet<0||inlet<0){
        error="Use valid object indices and nonnegative port numbers.";return false;
    }
    for(const auto& w:g.wires)if(w.from==from&&w.outlet==outlet&&w.to==to&&w.inlet==inlet){
        error="This connection already exists.";return false;
    }
    std::size_t where=g.lines.size();
    for(std::size_t i=0;i<g.lines.size();++i)if(g.lines[i].rfind("#X coords ",0)==0){where=i;break;}
    if(where>0&&!g.lines[where-1].empty()&&g.lines[where-1].back()!='\n')g.lines[where-1]+=g.newline;
    g.lines.insert(g.lines.begin()+static_cast<std::ptrdiff_t>(where),
        "#X connect "+std::to_string(from)+" "+std::to_string(outlet)+" "+std::to_string(to)+" "+std::to_string(inlet)+";"+g.newline);
    output=join(g);error.clear();return true;
}
inline bool removeConnection(const std::string& source,std::size_t index,std::string& output,std::string& error) {
    Graph g;if(!parse(source,g,error))return false;
    if(index>=g.wires.size()){error="Unknown connection.";return false;}
    g.lines.erase(g.lines.begin()+static_cast<std::ptrdiff_t>(g.wires[index].line));
    output=join(g);error.clear();return true;
}
inline bool removeObject(const std::string& source,int index,std::string& output,std::string& error) {
    Graph g;if(!parse(source,g,error))return false;
    if(index<0||index>=(int)g.objects.size()){error="Unknown object.";return false;}
    // Remove attached wires and remap indices above the removed object.
    for(const auto& wire:g.wires) {
        if(wire.from==index||wire.to==index){g.lines[wire.line].clear();continue;}
        if(wire.from>index||wire.to>index) {
            int a=wire.from-(wire.from>index),b=wire.to-(wire.to>index);
            const auto& old=g.lines[wire.line];
            const std::string ending=old.size()>=2&&old.substr(old.size()-2)=="\r\n"?"\r\n":
                (!old.empty()&&old.back()=='\n'?"\n":"");
            g.lines[wire.line]="#X connect "+std::to_string(a)+" "+std::to_string(wire.outlet)+
                " "+std::to_string(b)+" "+std::to_string(wire.inlet)+";"+ending;
        }
    }
    g.lines[g.objectLines[static_cast<std::size_t>(index)]].clear();
    output=join(g);error.clear();return true;
}
}
