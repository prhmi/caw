#define CAW_VST3 1
#include "vst_rack.h"
#include <cassert>
#include <map>
#include <iostream>
static std::map<int,double> applied;
namespace caw {
class Fake : public IPlugin {
 std::vector<PluginParam> p = {{0,100,"Cutoff","",0,0,0,false},{1,200,"Resonance","",0,0,0,false},{2,300,"Read-only","",0,0,0,true}};
public:
 void setEditorSystemScaling(bool) override {} bool load(const std::string&,std::string&) override { return true; }
 bool isInstrument() const override {return true;} void prepare(double,int) override {}
 void process(const float*,float*,int,int,Endpoint&) override {} const std::vector<PluginParam>& params() const override{return p;}
 void setParam(int i,double v) override {applied[i]=v;} double getParam(int) const override{return 0;}
 bool showEditor(const std::string&,std::string&) override{return false;} bool editorVisible() const override{return false;}
 void setLearnEnabled(bool) override {} int takeTouchedParam() override{return -1;}
 std::string getState() const override{return {};} void setState(const std::string&) override{}
};
std::unique_ptr<IPlugin> makeVst3Plugin(){return std::make_unique<Fake>();}
}
int main(){VstJob j;j.name="synth";j.path="fake";j.parameterState={{100,.42},{200,.81},{300,.5},{400,.3}};
 VstRack r; bool ok=r.init({j},44100,64,2,8,[](const std::string&,int)->void*{return nullptr;},[](int)->const void*{return nullptr;},[](const std::string&){},false);
 assert(ok && applied.size()==2 && applied[0]==.42 && applied[1]==.81); std::cout<<"render rack restores writable values by parameter ID\n";}
