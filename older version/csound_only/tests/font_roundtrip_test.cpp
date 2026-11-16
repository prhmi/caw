#include "project_io.h"
#include <cassert>
#include <cmath>
#include <iostream>
#include <string>
int main() {
    Project p; p.cs.programFontSize=23; p.cs.editorFontSize=14;
    std::string s=serializeProject(p), error; Project q;
    assert(s.find("CAW_PROJECT 51\n") == 0);
    assert(deserializeProject(q,s,error));
    assert(q.cs.programFontSize==23 && q.cs.editorFontSize==14);
    s.replace(0, std::string("CAW_PROJECT 51").size(), "CAW_PROJECT 48");
    auto at=s.find("UIFONT1 "); assert(at != std::string::npos); s.erase(at);
    Project legacy; assert(deserializeProject(legacy,s,error));
    assert(legacy.cs.programFontSize==17 && legacy.cs.editorFontSize==14);
    std::cout << "format 51 round-trip and format 48 migration passed\n";
}
