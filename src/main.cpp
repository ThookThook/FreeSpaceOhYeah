#include "sha256.h"
#include "gpu_loader.h"
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
using namespace std; namespace fs=std::filesystem;
constexpr int W=1920,H=1080,BLOCK=4,FPS=24; constexpr size_t BLOCKS=(W/BLOCK)*(H/BLOCK), BYTES_PER_FRAME=BLOCKS/8, CHUNK_SIZE=1024, ECC_GROUP=10;
struct Opt{string mode,input,output,checkpoint,gpu="off",gpu_plugin; vector<string> inputs; uint64_t max_size=0; bool resume=false, ckdecode=false, auto_continue=false;};
void die(const string&s){throw runtime_error(s);}
bool ends_with(const string&s,const string&suffix){return s.size()>=suffix.size() && s.compare(s.size()-suffix.size(),suffix.size(),suffix)==0;}
string normalize_gpu_mode(const string& mode){
 if(mode=="off"||mode=="auto"||mode=="frames"||mode=="encode"||mode=="decode") return mode;
 if(mode=="process"||mode=="do-all") return "auto";
 die("unknown --gpu mode '"+mode+"' (expected off, auto, frames, encode, decode, process, or do-all)");
 return "off";
} bool have_ffmpeg(){return system("command -v ffmpeg >/dev/null 2>&1")==0;} uint64_t parse_size(string s){uint64_t m=1; char c=s.empty()?0:s.back(); if(!isdigit(c)){s.pop_back(); if(c=='K')m=1024; else if(c=='M')m=1024ull*1024; else if(c=='G')m=1024ull*1024*1024; else die("bad size suffix");} return stoull(s)*m;}
void put32(vector<uint8_t>&v,uint32_t x){for(int i=0;i<4;i++)v.push_back((x>>(i*8))&255);} uint32_t get32(const vector<uint8_t>&v,size_t p){uint32_t x=0; for(int i=3;i>=0;i--)x=(x<<8)|v[p+i]; return x;}
void put64(vector<uint8_t>&v,uint64_t x){for(int i=0;i<8;i++)v.push_back((x>>(i*8))&255);} uint64_t get64(const vector<uint8_t>&v,size_t p){uint64_t x=0; for(int i=7;i>=0;i--)x=(x<<8)|v[p+i]; return x;}
uint32_t checksum32(const uint8_t* data,size_t n){uint32_t h=2166136261u; for(size_t i=0;i<n;i++){h^=data[i]; h*=16777619u;} return h;}
array<uint8_t,32> file_sha(const string&p){ifstream f(p,ios::binary); if(!f)die("cannot open "+p); Sha256 s; vector<uint8_t>b(1<<20); while(f){f.read((char*)b.data(),b.size()); if(f.gcount())s.update(b.data(),f.gcount());} return s.final();}
string json_escape(const string&s){string o; for(char ch:s){if(ch=='\\'||ch=='"') {o.push_back('\\'); o.push_back(ch);} else if(ch=='\n') o+="\\n"; else o.push_back(ch);} return o;}
string checkpoint_field(const string&path,const string&key){ifstream f(path); if(!f)die("cannot open checkpoint "+path); string data((istreambuf_iterator<char>(f)),{}); string needle="\""+key+"\""; size_t p=data.find(needle); if(p==string::npos)return {}; p=data.find(':',p); if(p==string::npos)return {}; p=data.find('"',p); if(p==string::npos)return {}; size_t e=data.find('"',p+1); if(e==string::npos)return {}; return data.substr(p+1,e-p-1);}
void encode_checkpoint(const string&path,const string&src,const string&out,uint64_t size,const array<uint8_t,32>&sha,const string&status,uint64_t byte_offset){ofstream c(path); c<<"{\n  \"type\": \"encode\",\n  \"source_path\": \""<<json_escape(src)<<"\",\n  \"source_size\": "<<size<<",\n  \"sha256\": \""<<Sha256::hex(sha)<<"\",\n  \"hare_version\": 1,\n  \"block_size\": 4,\n  \"ecc_ratio\": 0.10,\n  \"fps\": 24,\n  \"byte_offset\": "<<byte_offset<<",\n  \"volume_index\": 1,\n  \"output_path\": \""<<json_escape(out)<<"\",\n  \"outputs\": [\""<<json_escape(out)<<"\"],\n  \"status\": \""<<status<<"\"\n}\n";}
void decode_checkpoint(const string&path,const vector<string>&inputs,const string&out,uint64_t payload_size,const string&sha,const string&status){
 ofstream c(path);
 c << "{\n";
 c << "  \"type\": \"decode\",\n";
 c << "  \"inputs\": [";
 for(size_t i=0;i<inputs.size();++i){if(i)c<<", "; c<<"\""<<json_escape(inputs[i])<<"\"";}
 c << "],\n";
 c << "  \"output_path\": \"" << json_escape(out) << "\",\n";
 c << "  \"decoded_payload_size\": " << payload_size << ",\n";
 c << "  \"sha256\": \"" << sha << "\",\n";
 c << "  \"hare_version\": 1,\n";
 c << "  \"block_size\": 4,\n";
 c << "  \"fps\": 24,\n";
 c << "  \"status\": \"" << status << "\"\n";
 c << "}\n";
}
vector<uint8_t> build_hare_payload(const string& path,uint64_t sz,const array<uint8_t,32>&sha){
 vector<vector<uint8_t>> chunks;
 ifstream f(path,ios::binary);
 if(!f)die("cannot open "+path);
 while(f){vector<uint8_t> c(CHUNK_SIZE,0); f.read((char*)c.data(),c.size()); streamsize got=f.gcount(); if(got>0)chunks.push_back(c);}
 vector<uint8_t> payload;
 string magic="FSOYHARE2";
 payload.insert(payload.end(),magic.begin(),magic.end());
 put64(payload,sz);
 payload.insert(payload.end(),sha.begin(),sha.end());
 put32(payload,CHUNK_SIZE);
 put32(payload,ECC_GROUP);
 put64(payload,chunks.size());
 for(size_t i=0;i<chunks.size();++i){payload.push_back(0); put64(payload,i); put32(payload,checksum32(chunks[i].data(),chunks[i].size())); payload.insert(payload.end(),chunks[i].begin(),chunks[i].end());}
 for(size_t g=0;g<chunks.size();g+=ECC_GROUP){vector<uint8_t> parity(CHUNK_SIZE,0); for(size_t i=g;i<min(g+ECC_GROUP,chunks.size());++i) for(size_t j=0;j<CHUNK_SIZE;++j) parity[j]^=chunks[i][j]; payload.push_back(1); put64(payload,g/ECC_GROUP); put32(payload,checksum32(parity.data(),parity.size())); payload.insert(payload.end(),parity.begin(),parity.end());}
 return payload;
}
vector<uint8_t> recover_hare_payload(const vector<uint8_t>& bytes,array<uint8_t,32>&sha){
 if(bytes.size()<65 || string(bytes.begin(),bytes.begin()+9)!="FSOYHARE2") die("not an FSOY HARE v1 ECC stream");
 uint64_t sz=get64(bytes,9);
 copy(bytes.begin()+17,bytes.begin()+49,sha.begin());
 uint32_t chunk_size=get32(bytes,49), group=get32(bytes,53);
 uint64_t data_count=get64(bytes,57);
 if(chunk_size!=CHUNK_SIZE||group!=ECC_GROUP) die("unsupported HARE v1 parameters");
 size_t pos=65;
 vector<vector<uint8_t>> data(data_count,vector<uint8_t>(CHUNK_SIZE));
 vector<char> ok(data_count,0);
 vector<vector<uint8_t>> parity((data_count+group-1)/group);
 vector<char> pok(parity.size(),0);
 while(pos+13+chunk_size<=bytes.size()){
  uint8_t type=bytes[pos++]; uint64_t idx=get64(bytes,pos); pos+=8; uint32_t chk=get32(bytes,pos); pos+=4;
  const uint8_t* block=bytes.data()+pos; bool good=checksum32(block,chunk_size)==chk;
  if(type==0&&idx<data_count&&good){copy(block,block+chunk_size,data[idx].begin()); ok[idx]=1;}
  else if(type==1&&idx<parity.size()&&good){parity[idx].assign(block,block+chunk_size); pok[idx]=1;}
  pos+=chunk_size;
 }
 for(size_t g=0;g<data_count;g+=group){
  size_t missing=data_count, missing_count=0, gi=g/group;
  for(size_t i=g;i<min<uint64_t>(g+group,data_count);++i) if(!ok[i]){missing=i; missing_count++;}
  if(missing_count==1&&gi<parity.size()&&pok[gi]){data[missing]=parity[gi]; for(size_t i=g;i<min<uint64_t>(g+group,data_count);++i) if(i!=missing) for(size_t j=0;j<chunk_size;++j) data[missing][j]^=data[i][j]; ok[missing]=1;}
 }
 for(size_t i=0;i<data_count;++i) if(!ok[i]) die("ECC recovery failed: missing or corrupt chunk "+to_string(i));
 vector<uint8_t> out; out.reserve(data_count*chunk_size);
 for(auto&c:data) out.insert(out.end(),c.begin(),c.end());
 if(out.size()<sz) die("truncated recovered payload");
 out.resize(sz);
 return out;
}
string ffmpeg_encode_cmd(const string&o,const string&gpu){ string enc="libx264 -preset veryfast -profile:v main -crf 20 -pix_fmt yuv420p"; if(gpu=="auto"||gpu=="encode") enc="h264_nvenc -preset fast -profile:v main -b:v 7M -pix_fmt yuv420p"; return "ffmpeg -hide_banner -loglevel error -y -f yuv4mpegpipe -i - -c:v "+enc+" -movflags +faststart+frag_keyframe+empty_moov "+"'"+o+"'"; }
void synthesize_frame_cpu(vector<uint8_t>&y,const vector<uint8_t>&data,size_t off){fill(y.begin(),y.end(),128); for(size_t bi=0;bi<BLOCKS;bi++){size_t byte=off+bi/8; bool bit= byte<data.size() && ((data[byte]>>(7-(bi%8)))&1); int bx=(bi%(W/BLOCK))*BLOCK, by=(bi/(W/BLOCK))*BLOCK; uint8_t val=bit?255:0; for(int yy=0;yy<BLOCK;yy++) memset(&y[(by+yy)*W+bx],val,BLOCK);}}
void write_frame(FILE*pipe,const vector<uint8_t>&data,size_t off,const GpuPlugin*gpu){static vector<uint8_t> y(W*H), uv((W/2)*(H/2),128); bool gpu_ok=false; if(gpu && gpu->available()){FsoyGpuFrameJob job{y.data(),W,H,BLOCK,data.data(),data.size(),off}; gpu_ok=gpu->synthesize_luma_frame(job);} if(!gpu_ok) synthesize_frame_cpu(y,data,off); fwrite("FRAME\n",1,6,pipe); fwrite(y.data(),1,y.size(),pipe); fwrite(uv.data(),1,uv.size(),pipe); fwrite(uv.data(),1,uv.size(),pipe);}
int encode(const Opt&o){
 if(!have_ffmpeg()) die("ffmpeg is required on PATH for MP4 encode/decode");
 GpuPlugin gpu;
 const GpuPlugin* frame_gpu=nullptr;
 if(o.gpu=="auto"||o.gpu=="frames"){
  gpu=GpuPlugin::load(o.gpu_plugin);
  if(gpu.loaded()&&gpu.available()){
   cerr<<"Loaded GPU add-on: "<<gpu.name()<<" ("<<gpu.status()<<")\n";
   frame_gpu=&gpu;
  } else {
   cerr<<"NVIDIA GPU add-on unavailable: "<<gpu.status()<<"; using CPU frame synthesis.\n";
  }
 }
 if(o.gpu=="auto") cerr<<"GPU process suite requested; frame synthesis and NVENC will be attempted with CPU fallback where possible.\n";
 if(o.gpu=="encode") cerr<<"GPU encode requested; NVENC will be attempted and CPU libx264 used if unavailable.\n";
 uint64_t sz=fs::file_size(o.input); auto sha=file_sha(o.input); string ck=o.resume&&!o.checkpoint.empty()?o.checkpoint:o.output+".encode.checkpoint"; if(!ends_with(ck,".encode.checkpoint")) die("encode checkpoint path must end with .encode.checkpoint"); encode_checkpoint(ck,o.input,o.output,sz,sha,"started",0); vector<uint8_t> payload=build_hare_payload(o.input,sz,sha);
 string cmd=ffmpeg_encode_cmd(o.output,o.gpu); FILE*p=popen(cmd.c_str(),"w"); if(!p)die("failed to start ffmpeg"); fprintf(p,"YUV4MPEG2 W%d H%d F%d:1 Ip A1:1 C420jpeg\n",W,H,FPS); for(size_t off=0;off<payload.size();off+=BYTES_PER_FRAME) write_frame(p,payload,off,frame_gpu); int rc=pclose(p); if(rc!=0 && (o.gpu=="auto"||o.gpu=="encode")){ cerr<<"GPU encode failed; retrying with CPU libx264.\n"; Opt c=o; c.gpu="off"; return encode(c);} if(rc!=0){encode_checkpoint(ck,o.input,o.output,sz,sha,"failed",0); die("ffmpeg encode failed");} encode_checkpoint(ck,o.input,o.output,sz,sha,"complete",sz); return 0;}
string ffmpeg_decode_cmd(const string&i,const string&gpu){string hw=(gpu=="auto"||gpu=="decode")?" -hwaccel cuda":""; return "ffmpeg -hide_banner -loglevel error"+hw+" -i '"+i+"' -f rawvideo -pix_fmt gray -";}
int decode_with_gpu(const Opt&o,const string&gpu){vector<uint8_t> bytes; for(auto&in:o.inputs){FILE*p=popen(ffmpeg_decode_cmd(in,gpu).c_str(),"r"); if(!p)die("failed to start ffmpeg"); vector<uint8_t> y(W*H); while(fread(y.data(),1,y.size(),p)==y.size()){uint8_t cur=0; int n=0; for(size_t bi=0;bi<BLOCKS;bi++){int bx=(bi%(W/BLOCK))*BLOCK, by=(bi/(W/BLOCK))*BLOCK; cur=(cur<<1)|(y[by*W+bx]>127); if(++n==8){bytes.push_back(cur); cur=0; n=0;}}} int rc=pclose(p); if(rc!=0)die("ffmpeg decode failed");}
 string ck=o.ckdecode&&!o.checkpoint.empty()?o.checkpoint:o.output+".decode.checkpoint"; if(!ends_with(ck,".decode.checkpoint")) die("decode checkpoint path must end with .decode.checkpoint"); decode_checkpoint(ck,o.inputs,o.output,0,"","started"); array<uint8_t,32> sha{}; vector<uint8_t> recovered; try{recovered=recover_hare_payload(bytes,sha);}catch(const exception&){decode_checkpoint(ck,o.inputs,o.output,0,"","failed"); throw;} ofstream out(o.output,ios::binary); out.write((char*)recovered.data(),recovered.size()); out.close(); auto got=file_sha(o.output); if(got!=sha){decode_checkpoint(ck,o.inputs,o.output,recovered.size(),Sha256::hex(sha),"failed"); die("SHA-256 verification failed: got "+Sha256::hex(got)+" expected "+Sha256::hex(sha));} decode_checkpoint(ck,o.inputs,o.output,recovered.size(),Sha256::hex(got),"complete"); cerr<<"Verified SHA-256 "<<Sha256::hex(got)<<"\n"; return 0;}
int decode(const Opt&o){if(!have_ffmpeg()) die("ffmpeg is required on PATH for MP4 encode/decode"); if(o.gpu=="auto"||o.gpu=="decode"){cerr<<"GPU decode requested; FFmpeg CUDA hwaccel will be attempted and CPU decode used if unavailable.\n"; try{return decode_with_gpu(o,o.gpu);}catch(const exception&e){cerr<<"GPU decode failed: "<<e.what()<<"; retrying with CPU decode.\n"; Opt c=o; c.gpu="off"; return decode_with_gpu(c,c.gpu);}} return decode_with_gpu(o,o.gpu);}
Opt parse(int argc,char**argv){ if(argc<2)die("usage: fsoy encode <file> -o <out.mp4> | fsoy decode <mp4...> -o <file>"); Opt o; o.mode=argv[1]; for(int i=2;i<argc;i++){string a=argv[i]; if(a=="-o"&&i+1<argc)o.output=argv[++i]; else if(a=="--gpu"&&i+1<argc)o.gpu=normalize_gpu_mode(argv[++i]); else if(a=="--gpu-plugin"&&i+1<argc)o.gpu_plugin=argv[++i]; else if(a=="--max-output-size"&&i+1<argc)o.max_size=parse_size(argv[++i]); else if(a=="--auto-continue")o.auto_continue=true; else if(a=="--resume"&&i+1<argc){o.resume=true; o.checkpoint=argv[++i];} else if(a=="--checkpoint"&&i+1<argc){o.ckdecode=true; o.checkpoint=argv[++i];} else if(o.mode=="decode")o.inputs.push_back(a); else if(o.input.empty())o.input=a; else die("unexpected arg "+a);} if(o.mode=="encode"&&!o.resume&&(o.input.empty()||o.output.empty()))die("encode needs input and -o"); if(o.mode=="decode"&&(o.inputs.empty()||(!o.ckdecode&&o.output.empty())))die("decode needs input video(s) and -o"); return o;}
int main(int argc,char**argv){try{Opt o=parse(argc,argv); if(o.resume){if(!ends_with(o.checkpoint,".encode.checkpoint")) die("encode checkpoint path must end with .encode.checkpoint"); string t=checkpoint_field(o.checkpoint,"type"); o.input=checkpoint_field(o.checkpoint,"source_path"); o.output=checkpoint_field(o.checkpoint,"output_path"); if(t!="encode") die("--resume expects an encode checkpoint"); if(o.input.empty()||o.output.empty()) die("encode checkpoint is missing source_path or output_path"); o.mode="encode";} if(o.ckdecode){if(!ends_with(o.checkpoint,".decode.checkpoint")) die("decode checkpoint path must end with .decode.checkpoint"); string t=checkpoint_field(o.checkpoint,"type"); string out=checkpoint_field(o.checkpoint,"output_path"); if(t!="decode") die("--checkpoint expects a decode checkpoint"); if(!out.empty()) o.output=out;} if(o.max_size) cerr<<"--max-output-size is recorded in checkpoint; v1 scaffold writes one feed volume.\n"; if(o.mode=="encode")return encode(o); if(o.mode=="decode")return decode(o); die("unknown mode");}catch(const exception&e){cerr<<"fsoy: "<<e.what()<<"\n"; return 1;}}
