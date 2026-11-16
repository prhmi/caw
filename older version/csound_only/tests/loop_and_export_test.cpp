#include "timeline_loop.h"
#include "project_io.h"
#include "code_export.h"
#include <cassert>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
namespace fs=std::filesystem;
static std::string read(const fs::path& p){std::ifstream f(p,std::ios::binary);assert(f);std::ostringstream s;s<<f.rdbuf();return s.str();}
int main(){Project p;p.loopStart=2.;p.loopEnd=5.;p.loopEnabled=true; assert(timeline_loop::valid(p));assert(timeline_loop::wrap(p,1.)==2.);assert(timeline_loop::wrap(p,4.)==4.);assert(timeline_loop::wrap(p,5.)==2.);assert(timeline_loop::wrap(p,11.)==2.);
 TimelineEvent a;a.automation=true;a.start=1;a.dur=4;a.a=0;a.b=1;assert(timeline_loop::trim(a,3));assert(a.dur==2&&a.b==0.5);TimelineEvent n;n.start=3;n.dur=1;assert(!timeline_loop::trim(n,3));
 std::string s=serializeProject(p),err;Project restored;assert(deserializeProject(restored,s,err));assert(restored.loopEnabled&&restored.loopStart==2&&restored.loopEnd==5);
 s.replace(0,14,"CAW_PROJECT 50");auto pos=s.rfind("LOOP1 ");assert(pos!=std::string::npos);s.erase(pos);Project old;assert(deserializeProject(old,s,err));assert(!old.loopEnabled);
 Track c;c.kind=0;c.name="Orch";Clip x;x.id=1;x.name="Lead";x.code="a1 oscili 0.1,440\nout a1\n";c.clips.push_back(x);p.tracks.push_back(c);
 Track v;v.kind=3;v.id=3;v.name="VST";Clip y;y.id=2;y.name="Plugin";y.code="note 60,100,0.4";v.clips.push_back(y);p.tracks.push_back(v);
 Track sc;sc.kind=5;sc.name="SC placeholder";Clip z;z.id=3;z.name="SC";z.code="futureSuperColliderCode";sc.clips.push_back(z);p.tracks.push_back(sc);
 auto base=fs::temp_directory_path()/"caw_code_export_test_unique";std::error_code ec;fs::remove_all(base,ec);assert(fs::create_directories(base));fs::path d;int written=0;
 assert(code_export::write(p,base,d,written,err,code_export::Mode::SeparateRaw,code_export::Filter::All));assert(written==3&&read(d/"1_Lead.orc")==x.code&&read(d/"2_Plugin.cawcode")==y.code&&read(d/"3_SC.txt")==z.code);auto first=d;
 assert(code_export::write(p,base,d,written,err,code_export::Mode::CombinedText,code_export::Filter::Csound));auto all=read(d/"all_clip_code.txt");assert(written==1&&all.find(x.code)!=std::string::npos&&all.find(y.code)==std::string::npos&&d!=first);
 assert(code_export::write(p,base,d,written,err,code_export::Mode::CsoundRaw));assert(read(d/"csound_clip_code.orc").find(x.code)!=std::string::npos);
 assert(code_export::write(p,base,d,written,err,code_export::Mode::CsoundInstruments));auto orc=read(d/"csound_instruments.orc");assert(orc.find("instr Lead\n")!=std::string::npos&&orc.find("endin")!=std::string::npos&&orc.find(y.code)==std::string::npos);
 assert(code_export::write(p,base,d,written,err,code_export::Mode::CsoundProject));auto csd=read(d/"project.csd");assert(csd.find("<CsoundSynthesizer>")!=std::string::npos&&csd.find("<CsScore>")!=std::string::npos&&csd.find(z.code)==std::string::npos);
 fs::remove_all(base,ec);std::cout<<"Loop boundaries, v50 migration and five export modes passed\n";}
