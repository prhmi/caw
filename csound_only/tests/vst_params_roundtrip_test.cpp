#include "project_io.h"
#include <cassert>
#include <iostream>
#include <string>
int main() {
    Project p; p.cs.programFontSize=23; p.cs.editorFontSize=14;
    p.cs.ffmpegExe="C:/Program Files/ffmpeg/bin/ffmpeg.exe";
    Track t; t.kind=3; t.id=1; t.name="Synth"; t.vstPath="synth.vst3"; t.vstParameterState[123]=0.78; t.vstParameterState[999]=0.25; p.tracks.push_back(t);
    auto s=serializeProject(p); Project q; std::string error;
    assert(s.find("CAW_PROJECT 51\n") == 0 && deserializeProject(q,s,error));
    assert(q.cs.programFontSize==23 && q.cs.editorFontSize==14 && q.cs.ffmpegExe==p.cs.ffmpegExe);
    assert(q.tracks.size()==1 && q.tracks[0].vstParameterState==t.vstParameterState);
    std::string old=s; old.replace(0,14,"CAW_PROJECT 49"); auto at=old.find("VSTPARAMS1\n"); assert(at!=std::string::npos); old.erase(at);
    Project legacy; assert(deserializeProject(legacy,old,error)); assert(legacy.cs.programFontSize==23 && legacy.cs.ffmpegExe.empty() && legacy.tracks[0].vstParameterState.empty());
    auto bad=s; auto b=bad.find("123 0.78"); if(b!=std::string::npos) { bad.replace(b,8,"123 2.00"); Project x; assert(!deserializeProject(x,bad,error)); }
    std::cout << "parameter values, FFmpeg path and format 49 migration passed\n";
}
