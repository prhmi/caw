#include "startup_logo.h"
#include <cassert>
#include <iostream>
int main(){size_t pixels=0, nonzero=0; for(size_t i=0;i+1<sizeof(kLogoRle);i+=2){unsigned n=kLogoRle[i], index=kLogoRle[i+1];assert(n>0);pixels+=n;if(kLogoPalette[index*4+3])nonzero+=n;}assert(pixels==512*512);assert(nonzero>0);std::cout<<"logo: "<<pixels<<" pixels, "<<nonzero<<" visible\n";}
