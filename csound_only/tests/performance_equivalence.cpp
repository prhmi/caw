#include "csd_builder.h"
#include <chrono>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>
static Project fixture(int n) {
    Project p; p.score.analyze=true; p.cs.monitorFeed=true;
    for (int k=0;k<4;++k) { Track t; t.id=k+1; t.name="Lane"+std::to_string(k); t.kind=k==2?3:k==3?4:0;
      t.vstPick={0,2}; t.vstPickName={"Cutoff","Gain"}; t.vstOut=k==2?"Lane3":""; t.vstOutMaster=true;
      t.automationRanges["Cutoff"]={"Cutoff",0,1,.01,.25};
      t.automation["Cutoff"]={{0,.25,false},{1,.8,false},{3,.4,false}};
      for (int i=0;i<n;++i) { Clip c; c.id=k*n+i+1; c.name="Clip"+std::to_string(c.id); c.start=i*.1; c.dur=1.2; c.runs=1+i%3; c.midiEnabled=(i%4==0);
        c.code=t.kind==0?"; test\nkCut cawget \"Cutoff\"\naOut oscili kCut, 440\nout aOut\n":"P1 = 0.7\nnote 60, 100, 0.4\n";
        t.clips.push_back(c);
        if(i%5==0){MidiClip m;m.id=10000+c.id;m.clipId=c.id;m.start=i*.1;m.dur=1.;m.notes.push_back({0,.25,60+i%12,90,0});t.midiClips.push_back(m);}
      } p.tracks.push_back(std::move(t)); }
    return p;
}
int main(int argc,char**argv) {
    if(argc < 3) return 2;
    Project p=fixture(std::stoi(argv[2]));
    const auto start=std::chrono::steady_clock::now();
    std::ofstream out(argv[1],std::ios::binary);
    for(const char* mode:{"engine","render"}){
      std::vector<OrcSpan> spans; auto text=buildCsd(p,mode,0.,&spans); out<<mode<<'\n'<<text<<"\nSPANS\n";
      for(const auto& s:spans){out<<s.name<<' '<<s.first;for(int v:s.lineMap)out<<' '<<v;out<<'\n';}
    }
    for(const auto& e:timelineEvents(p,.5,true))out<<e.automation<<' '<<e.name<<' '<<e.start<<' '<<e.dur<<' '<<e.a<<' '<<e.b<<'\n';
    out<<"SIGNATURE "<<timelineSignature(p)<<'\n';
    // Exercise invalidation when metadata and VST translation inputs change.
    for (auto& t:p.tracks) { if(t.clips.empty())continue; auto& c=t.clips[0];
      for(int i=0;i<8;++i){if(i==1)c.code+="\nkextra=1\n";if(i==2)c.midiEnabled=!c.midiEnabled;if(i==3)c.runs=3;if(i==4)c.pFieldRules="foo";if(i==5)t.automationRanges["Cutoff"].value=.88;if(i==6)t.vstPickName={"Other","Gain"};if(i==7)t.name="Renamed";
       auto parsed=parseClipForTrack(c,t,i%2);out<<parsed.body<<' '<<parsed.midiEnabled<<' '<<parsed.outChannels<<' '<<parsed.hasRec<<'\n';for(const auto& x:parsed.params)out<<x.chn<<' '<<x.min<<' '<<x.max<<' '<<x.step<<' '<<x.value<<'\n';}}
    out.close(); auto end=std::chrono::steady_clock::now(); std::cerr<<"elapsed_ms="<<std::chrono::duration_cast<std::chrono::milliseconds>(end-start).count()<<'\n';
}
