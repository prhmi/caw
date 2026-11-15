#include "vst_audio_buffers.h"
#include <cassert>
#include <cmath>
#include <iostream>
#include <vector>
int main(){VstAudioBuffers b;b.prepare(64);auto l=b.left();auto r=b.right();auto ol=b.outLeft();auto orr=b.outRight();
 for(int ch: {1,2,4}) for(int n: {1,16,64}) {std::vector<float> input(n*ch), output(n*ch,99.f);for(int i=0;i<n*ch;++i)input[i]=float(i)/7.f;
  for(int repeat=0;repeat<100;++repeat){b.begin(input.data(),n,ch);assert(b.left()==l&&b.right()==r&&b.outLeft()==ol&&b.outRight()==orr);
   for(int i=0;i<n;++i){assert(b.left()[i]==input[i*ch]);assert(b.right()[i]==input[i*ch+(ch>1)]);assert(b.outLeft()[i]==0.f&&b.outRight()[i]==0.f);b.outLeft()[i]=b.left()[i]*.5f;b.outRight()[i]=b.right()[i]*.25f;}
   b.finish(output.data(),n,ch);for(int i=0;i<n;++i){assert(output[i*ch]==input[i*ch]*.5f);if(ch>1)assert(output[i*ch+1]==input[i*ch+1]*.25f);if(ch>2)assert(output[i*ch+2]==99.f);}}
 }
 b.begin(nullptr,64,2);for(int i=0;i<64;++i)assert(b.left()[i]==0.f&&b.right()[i]==0.f&&b.outLeft()[i]==0.f&&b.outRight()[i]==0.f);
 b.begin(nullptr,128,2);for(int i=0;i<128;++i)assert(b.left()[i]==0.f&&b.right()[i]==0.f);std::cout<<"VST buffers: mono/stereo/multichannel, silence, reuse, oversize passed\n";}
