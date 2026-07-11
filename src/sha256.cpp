#include "sha256.h"
#include <algorithm>
#include <cstring>
#include <iomanip>
#include <sstream>
namespace { constexpr uint32_t k[64] = {
0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
uint32_t rotr(uint32_t x,uint32_t n){return (x>>n)|(x<<(32-n));}
uint32_t load32(const uint8_t*p){return (uint32_t(p[0])<<24)|(uint32_t(p[1])<<16)|(uint32_t(p[2])<<8)|p[3];}
void store64(uint8_t*p,uint64_t v){for(int i=7;i>=0;--i){p[i]=v&0xff;v>>=8;}}
}
Sha256::Sha256(){state_={0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};}
void Sha256::update(const uint8_t*d,size_t l){bit_len_+=uint64_t(l)*8; while(l){size_t n=std::min(l,64-buffer_len_); std::memcpy(buffer_.data()+buffer_len_,d,n); buffer_len_+=n; d+=n; l-=n; if(buffer_len_==64){transform(buffer_.data()); buffer_len_=0;}}}
std::array<uint8_t,32> Sha256::final(){uint64_t bits=bit_len_; buffer_[buffer_len_++]=0x80; if(buffer_len_>56){while(buffer_len_<64)buffer_[buffer_len_++]=0; transform(buffer_.data()); buffer_len_=0;} while(buffer_len_<56)buffer_[buffer_len_++]=0; store64(buffer_.data()+56,bits); transform(buffer_.data()); std::array<uint8_t,32> out{}; for(size_t i=0;i<8;i++){out[i*4]=state_[i]>>24;out[i*4+1]=state_[i]>>16;out[i*4+2]=state_[i]>>8;out[i*4+3]=state_[i];} return out;}
std::string Sha256::hex(const std::array<uint8_t,32>&d){std::ostringstream o; for(auto b:d)o<<std::hex<<std::setw(2)<<std::setfill('0')<<int(b); return o.str();}
void Sha256::transform(const uint8_t b[64]){uint32_t w[64]; for(int i=0;i<16;i++)w[i]=load32(b+4*i); for(int i=16;i<64;i++){uint32_t s0=rotr(w[i-15],7)^rotr(w[i-15],18)^(w[i-15]>>3),s1=rotr(w[i-2],17)^rotr(w[i-2],19)^(w[i-2]>>10); w[i]=w[i-16]+s0+w[i-7]+s1;} auto [a,bv,c,d,e,f,g,h]=state_; for(int i=0;i<64;i++){uint32_t S1=rotr(e,6)^rotr(e,11)^rotr(e,25),ch=(e&f)^((~e)&g),t1=h+S1+ch+k[i]+w[i],S0=rotr(a,2)^rotr(a,13)^rotr(a,22),maj=(a&bv)^(a&c)^(bv&c),t2=S0+maj; h=g;g=f;f=e;e=d+t1;d=c;c=bv;bv=a;a=t1+t2;} state_[0]+=a;state_[1]+=bv;state_[2]+=c;state_[3]+=d;state_[4]+=e;state_[5]+=f;state_[6]+=g;state_[7]+=h;}
