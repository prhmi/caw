#include "csd_builder.h"
#include <chrono>
#include <iostream>
#include <string>
#include <vector>
int main(){
 std::vector<Track> tracks;
 for(int t=0;t<10;++t){Track tr;tr.id=t+1;tr.name="Track"+std::to_string(t);tr.kind=(t%4==0)?3:0;tr.vstPick={0,1};tr.vstPickName={"cutoff","gain"};tr.automationRanges["cutoff"]={"cutoff",0,1,.01,.5};
 for(int i=0;i<20;++i){Clip c;c.id=t*20+i+1;c.name="Clip"+std::to_string(c.id);c.code=tr.kind==3?"P1 = 0.7\nnote 60,100,0.4\n":"kCut cawget \"cutoff\"\naOut oscili kCut, 440\nout aOut\n";tr.clips.push_back(c);}tracks.push_back(std::move(tr));}
 const auto a=std::chrono::steady_clock::now(); size_t checksum=0;
 for(int pass=0;pass<50;++pass) for(const auto& t:tracks){auto result=parseTrack(t);checksum+=result.params.size()+result.midiEnabled;}
 const auto b=std::chrono::steady_clock::now();std::cout<<"elapsed_ms="<<std::chrono::duration_cast<std::chrono::milliseconds>(b-a).count()<<" checksum="<<checksum<<"\n";
}
