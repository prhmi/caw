#include "csd_builder.h"
#include <iostream>
#include <iomanip>
#include <string>
static void dump(const ParsedTrack& p){std::cout<<p.body<<"\n~ "<<p.midiEnabled<<' '<<p.launcher<<' '<<p.scoreSig<<' '<<p.hasRec<<' '<<p.outChannels<<'\n';for(const auto& x:p.params)std::cout<<"P "<<x.chn<<' '<<x.min<<' '<<x.max<<' '<<x.step<<' '<<x.value<<'\n';for(int n:p.lineMap)std::cout<<n<<',';std::cout<<'\n';for(const auto& s:p.scoreStreams)std::cout<<"S "<<s.first<<' '<<s.second<<'\n';for(const auto& s:p.writeChannels)std::cout<<"W "<<s<<'\n';}
int main(){std::cout<<std::setprecision(17);Track t;t.id=3;t.kind=3;t.name="Synth";t.vstPick={1,2};t.vstPickName={"gain","freq"};t.automationRanges["gain"]={"gain",0,1,.01,.4};Clip c;c.id=4;c.name="test";c.code="P1 = 0.3\nnote 60, 100, 0.2\n";
 for(int i=0;i<20;++i){switch(i){case 1:t.id=31;break;case 2:t.name="Synth B";break;case 3:t.kind=4;break;case 4:t.vstPickName[0]="tone";break;case 5:t.vstPick.pop_back();break;case 6:c.code+="cc 1, 2\n";break;case 7:c.midiEnabled=true;break;case 8:c.name="test2";break;case 9:c.runs=4;break;case 10:c.pFieldRules="copy: 1";break;case 11:t.automationRanges["gain"].min=-1;break;case 12:t.automationRanges["gain"].max=2;break;case 13:t.automationRanges["gain"].step=.2;break;case 14:t.automationRanges["gain"].value=.8;break;case 15:t.kind=0;c.code="kG cawget \"gain\"\naO oscili kG, 440\nout aO\n";break;case 16:c.code="<caw> midi: \"on\" </caw>\naO=0\nout aO\n";break;case 17:t.automationRanges.clear();break;case 18:c.code="chnset 1, \"score\"\ncawset 0.5, \"gain\"\n";break;case 19:c.code="";break;}
  std::cout<<"CASE "<<i<<'\n';dump(parseClipForTrack(c,t,i%2));dump(parseClip(c,i%2));dump(parseClipForTrack(c,t,i%2));}
}
