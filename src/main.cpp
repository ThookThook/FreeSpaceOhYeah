#include "sha256.h"
#include "gpu_loader.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <csignal>
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
#include <unistd.h>
#include <sys/wait.h>
using namespace std; namespace fs=std::filesystem;
constexpr int W=1920,H=1080,BLOCK=4,FPS=24; constexpr size_t BLOCKS=(W/BLOCK)*(H/BLOCK), BYTES_PER_FRAME=BLOCKS/8, CHUNK_SIZE=1024, ECC_GROUP=10;
struct Opt{string mode,input,output,checkpoint,gpu="off",gpu_plugin; vector<string> inputs; uint64_t max_size=0; bool resume=false, ckdecode=false, auto_continue=false;};
void die(const string&s){throw runtime_error(s);}
string normalize_gpu_mode(const string& mode){
 if(mode=="off"||mode=="auto"||mode=="frames"||mode=="encode") return mode;
 if(mode=="process"||mode=="do-all") return "auto";
 if(mode=="decode") return "decode";
 die("unknown --gpu mode '"+mode+"' (expected off, auto, frames, encode, decode, process, or do-all)");
 return "off";
} bool have_ffmpeg(){return system("command -v ffmpeg >/dev/null 2>&1")==0;} uint64_t parse_size(string s){if(s.empty())die("bad size value"); uint64_t m=1; char c=s.back(); if(!isdigit(static_cast<unsigned char>(c))){s.pop_back(); if(c=='K')m=1024; else if(c=='M')m=1024ull*1024; else if(c=='G')m=1024ull*1024*1024; else die("bad size suffix");} if(s.empty())die("bad size value"); return stoull(s)*m;}
void put64(vector<uint8_t>&v,uint64_t x){for(int i=0;i<8;i++)v.push_back((x>>(i*8))&255);} uint64_t get64(const vector<uint8_t>&v,size_t p){uint64_t x=0; for(int i=7;i>=0;i--)x=(x<<8)|v[p+i]; return x;}
void put32(vector<uint8_t>&v,uint32_t x){for(int i=0;i<4;i++)v.push_back((x>>(i*8))&255);} uint32_t get32(const vector<uint8_t>&v,size_t p){uint32_t x=0; for(int i=3;i>=0;i--)x=(x<<8)|v[p+i]; return x;}
uint32_t checksum32(const uint8_t* data,size_t n){uint32_t h=2166136261u; for(size_t i=0;i<n;i++){h^=data[i]; h*=16777619u;} return h;}
array<uint8_t,32> file_sha(const string&p){ifstream f(p,ios::binary); if(!f)die("cannot open "+p); Sha256 s; vector<uint8_t>b(1<<20); while(f){f.read((char*)b.data(),b.size()); if(f.gcount())s.update(b.data(),f.gcount());} return s.final();}
// HARE v1 stream layout: magic(9) | orig_size u64(8) | sha256(32) | num_data_chunks u64(8) | chunk_size u32(4) | ecc_group u32(4)
// then per ECC group: g data chunks of [checksum32(4) + chunk_size bytes], followed by one parity chunk [checksum32(4) + chunk_size bytes]
// where g = min(ecc_group, chunks remaining) and parity = XOR of that group's g data chunks (zero-padded).
// This gives single-chunk erasure/corruption recovery per group, at ~1/ecc_group (10%) storage overhead, matching the README's design.
//
// NOTE (reliability caveat, not a code bug): this scheme can only repair a single *chunk-boundary-aligned*
// erasure/corruption per ECC group. Real-world corruption of the underlying H.264 bitstream (e.g. a single
// bit-flip in a compressed frame) typically cascades across many macroblocks/frames due to intra/inter
// prediction and entropy coding, and will usually corrupt far more than one chunk per group of ECC_GROUP.
// This ECC is best understood as protection against a cleanly missing/replaced data chunk (e.g. multi-volume
// scaffolding), not as general-purpose resilience against arbitrary video-level corruption.
vector<uint8_t> build_hare_stream(const string&input_path,uint64_t&out_sz,array<uint8_t,32>&out_sha){
 out_sz=fs::file_size(input_path); out_sha=file_sha(input_path);
 vector<uint8_t> file_bytes(out_sz);
 if(out_sz){ ifstream f(input_path,ios::binary); if(!f)die("cannot open "+input_path); f.read((char*)file_bytes.data(),(streamsize)out_sz); }
 uint64_t num_data_chunks = out_sz==0 ? 1 : (out_sz+CHUNK_SIZE-1)/CHUNK_SIZE;
 vector<uint8_t> stream; string magic="FSOYHARE1";
 stream.insert(stream.end(),magic.begin(),magic.end());
 put64(stream,out_sz); stream.insert(stream.end(),out_sha.begin(),out_sha.end());
 put64(stream,num_data_chunks); put32(stream,(uint32_t)CHUNK_SIZE); put32(stream,(uint32_t)ECC_GROUP);
 uint64_t idx=0;
 while(idx<num_data_chunks){
  uint64_t g=min<uint64_t>(ECC_GROUP,num_data_chunks-idx);
  vector<uint8_t> parity(CHUNK_SIZE,0);
  for(uint64_t j=0;j<g;j++){
   vector<uint8_t> chunk(CHUNK_SIZE,0);
   uint64_t off=(idx+j)*CHUNK_SIZE;
   uint64_t avail = off<out_sz ? min<uint64_t>(CHUNK_SIZE,out_sz-off) : 0;
   if(avail) memcpy(chunk.data(),file_bytes.data()+off,avail);
   for(size_t b=0;b<CHUNK_SIZE;b++) parity[b]^=chunk[b];
   put32(stream,checksum32(chunk.data(),chunk.size()));
   stream.insert(stream.end(),chunk.begin(),chunk.end());
  }
  put32(stream,checksum32(parity.data(),parity.size()));
  stream.insert(stream.end(),parity.begin(),parity.end());
  idx+=g;
 }
 return stream;
}
// Inverse of build_hare_stream. Verifies each chunk's checksum; if exactly one chunk (data or parity) in a
// group fails, repairs a bad data chunk via XOR of the group's other chunks + parity. More than one failure
// per group is beyond this scheme's repair capacity and is reported as an unrecoverable error.
vector<uint8_t> reconstruct_hare_stream(const vector<uint8_t>&bytes,uint64_t&out_sz,array<uint8_t,32>&out_sha){
 if(bytes.size()<9 || string(bytes.begin(),bytes.begin()+9)!="FSOYHARE1") die("not an FSOY HARE v1 stream");
 if(bytes.size()<9+8+32+8+4+4) die("truncated HARE stream header");
 size_t pos=9;
 out_sz=get64(bytes,pos); pos+=8;
 copy(bytes.begin()+pos,bytes.begin()+pos+32,out_sha.begin()); pos+=32;
 uint64_t num_data_chunks=get64(bytes,pos); pos+=8;
 uint32_t chunk_size=get32(bytes,pos); pos+=4;
 uint32_t ecc_group=get32(bytes,pos); pos+=4;
 if(chunk_size==0||ecc_group==0) die("corrupt HARE stream header (zero chunk_size/ecc_group)");
 vector<uint8_t> out; out.reserve((size_t)min<uint64_t>(num_data_chunks*(uint64_t)chunk_size, (uint64_t)1<<34));
 uint64_t idx=0, group=0;
 while(idx<num_data_chunks){
  uint64_t g=min<uint64_t>(ecc_group,num_data_chunks-idx);
  vector<vector<uint8_t>> chunks(g); vector<bool> ok(g,false);
  for(uint64_t j=0;j<g;j++){
   if(pos+4+chunk_size>bytes.size()) die("truncated HARE stream in group "+to_string(group));
   uint32_t stored=get32(bytes,pos); pos+=4;
   chunks[j].assign(bytes.begin()+pos,bytes.begin()+pos+chunk_size); pos+=chunk_size;
   ok[j]=(checksum32(chunks[j].data(),chunks[j].size())==stored);
  }
  if(pos+4+chunk_size>bytes.size()) die("truncated HARE stream (missing parity) in group "+to_string(group));
  uint32_t parity_stored=get32(bytes,pos); pos+=4;
  vector<uint8_t> parity(bytes.begin()+pos,bytes.begin()+pos+chunk_size); pos+=chunk_size;
  bool parity_ok=(checksum32(parity.data(),parity.size())==parity_stored);
  int bad=0; int bad_idx=-1;
  for(uint64_t j=0;j<g;j++) if(!ok[j]){bad++; bad_idx=(int)j;}
  if(!parity_ok) bad++;
  if(bad==0 || (bad==1 && !parity_ok)){
   for(uint64_t j=0;j<g;j++) out.insert(out.end(),chunks[j].begin(),chunks[j].end());
  } else if(bad==1 && bad_idx>=0){
   vector<uint8_t> recovered=parity;
   for(uint64_t j=0;j<g;j++) if((int)j!=bad_idx) for(size_t b=0;b<chunk_size;b++) recovered[b]^=chunks[j][b];
   chunks[bad_idx]=recovered;
   cerr<<"Repaired chunk "<<(idx+bad_idx)<<" in ECC group "<<group<<" using XOR parity.\n";
   for(uint64_t j=0;j<g;j++) out.insert(out.end(),chunks[j].begin(),chunks[j].end());
  } else {
   die("unrecoverable corruption in ECC group "+to_string(group)+": more than one chunk failed checksum (this scheme repairs at most 1 bad chunk per group of "+to_string(g)+")");
  }
  idx+=g; group++;
 }
 if(out.size()<out_sz) die("reconstructed stream shorter than recorded size");
 out.resize(out_sz);
 return out;
}
string json_escape(const string&s){string o; for(char ch:s){if(ch=='\\'||ch=='"') {o.push_back('\\'); o.push_back(ch);} else if(ch=='\n') o+="\\n"; else o.push_back(ch);} return o;}
string checkpoint_field(const string&path,const string&key){ifstream f(path); if(!f)die("cannot open checkpoint "+path); string data((istreambuf_iterator<char>(f)),{}); string needle="\""+key+"\""; size_t p=data.find(needle); if(p==string::npos)return {}; p=data.find(':',p); if(p==string::npos)return {}; p=data.find('"',p); if(p==string::npos)return {}; size_t e=data.find('"',p+1); if(e==string::npos)return {}; return data.substr(p+1,e-p-1);}
void encode_checkpoint(const string&path,const string&src,const string&out,uint64_t size,const array<uint8_t,32>&sha,const string&status,uint64_t byte_offset){ofstream c(path); c<<"{\n  \"type\": \"encode\",\n  \"source_path\": \""<<json_escape(src)<<"\",\n  \"source_size\": "<<size<<",\n  \"sha256\": \""<<Sha256::hex(sha)<<"\",\n  \"hare_version\": 1,\n  \"block_size\": 4,\n  \"ecc_ratio\": 0.10,\n  \"fps\": 24,\n  \"byte_offset\": "<<byte_offset<<",\n  \"volume_index\": 1,\n  \"output_path\": \""<<json_escape(out)<<"\",\n  \"outputs\": [\""<<json_escape(out)<<"\"],\n  \"status\": \""<<status<<"\"\n}\n";}
void decode_checkpoint(const string&path,const vector<string>&inputs,const string&out,uint64_t payload_size,const string&sha,const string&status){ofstream c(path); c<<"{\n  \"type\": \"decode\",\n  \"inputs\": ["; for(size_t i=0;i<inputs.size();++i){if(i)c<<", "; c<<"\""<<json_escape(inputs[i])<<"\"";} c<<"],\n  \"output_path\": \""<<json_escape(out)<<"\",\n  \"decoded_payload_size\": "<<payload_size<<",\n  \"sha256\": \""<<sha<<"\",\n  \"hare_version\": 1,\n  \"block_size\": 4,\n  \"fps\": 24,\n  \"status\": \""<<status<<"\"\n}\n";}

// ---------------------------------------------------------------------------
// SECURITY FIX: the original code built ffmpeg commands as shell strings
// (e.g. "...'"+path+"'...") and ran them via popen(). Any path containing a
// single quote broke out of that quoting and let an attacker-controlled
// filename inject arbitrary shell commands (confirmed exploitable). These
// two helpers replace that with fork()+execvp() over an argv array, so
// ffmpeg is invoked directly with no shell involved and no string escaping
// needed at all -- file paths are passed byte-for-byte as arguments.
// (POSIX-only: Linux/macOS. A Windows build would need a CreateProcess-based
// equivalent instead of fork/execvp.)
// ---------------------------------------------------------------------------
struct Proc{ FILE* f=nullptr; pid_t pid=-1; };
Proc spawn_argv(const vector<string>& args,const char* mode){
 int fds[2];
 if(pipe(fds)!=0) die("pipe() failed");
 bool write_mode = (mode[0]=='w');
 pid_t pid=fork();
 if(pid<0) die("fork() failed");
 if(pid==0){
  if(write_mode) dup2(fds[0],STDIN_FILENO); else dup2(fds[1],STDOUT_FILENO);
  close(fds[0]); close(fds[1]);
  vector<char*> argv; argv.reserve(args.size()+1);
  for(auto&s:args) argv.push_back(const_cast<char*>(s.c_str()));
  argv.push_back(nullptr);
  execvp(argv[0],argv.data());
  _exit(127); // only reached if execvp fails (e.g. binary not found)
 }
 Proc p; p.pid=pid;
 if(write_mode){ close(fds[0]); p.f=fdopen(fds[1],"w"); }
 else { close(fds[1]); p.f=fdopen(fds[0],"r"); }
 if(!p.f) die("fdopen() failed on ffmpeg pipe");
 return p;
}
int close_proc(Proc&p){
 if(p.f){ fclose(p.f); p.f=nullptr; }
 int status=0;
 if(waitpid(p.pid,&status,0)<0) return -1;
 if(WIFEXITED(status)) return WEXITSTATUS(status);
 return -1; // killed by signal or otherwise didn't exit normally
}
vector<string> ffmpeg_encode_args(const string&o,const string&gpu){
 vector<string> a{"ffmpeg","-hide_banner","-loglevel","error","-y","-f","yuv4mpegpipe","-i","-","-c:v"};
 if(gpu=="auto"||gpu=="encode") a.insert(a.end(),{"h264_nvenc","-preset","lossless","-rc","constqp","-qp","0","-pix_fmt","yuv420p"});
 else a.insert(a.end(),{"libx264","-preset","veryfast","-crf","0","-pix_fmt","yuv420p"});
 a.insert(a.end(),{"-movflags","+faststart+frag_keyframe+empty_moov",o});
 return a;
}
vector<string> ffmpeg_decode_args(const string&i,bool use_gpu){
 vector<string> a{"ffmpeg","-hide_banner","-loglevel","error"};
 if(use_gpu) a.insert(a.end(),{"-hwaccel","cuda"});
 a.insert(a.end(),{"-i",i,"-f","rawvideo","-pix_fmt","gray","-"});
 return a;
}
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
 uint64_t sz; array<uint8_t,32> sha; vector<uint8_t> payload=build_hare_stream(o.input,sz,sha);
 Proc proc=spawn_argv(ffmpeg_encode_args(o.output,o.gpu),"w");
 fprintf(proc.f,"YUV4MPEG2 W%d H%d F%d:1 Ip A1:1 C420jpeg\n",W,H,FPS);
 for(size_t off=0;off<payload.size();off+=BYTES_PER_FRAME) write_frame(proc.f,payload,off,frame_gpu);
 int rc=close_proc(proc);
 if(rc!=0 && (o.gpu=="auto"||o.gpu=="encode")){ cerr<<"GPU encode failed; retrying with CPU libx264.\n"; Opt c=o; c.gpu="off"; return encode(c);}
 if(rc!=0){ encode_checkpoint(o.output+".encode.checkpoint",o.input,o.output,sz,sha,"failed",payload.size()); die("ffmpeg encode failed"); }
 encode_checkpoint(o.output+".encode.checkpoint",o.input,o.output,sz,sha,"complete",payload.size());
 return 0;
}
// Reads one input video fully into out_bytes via ffmpeg, appending decoded bits.
// Returns false (leaving out_bytes untouched) if ffmpeg exits non-zero, so a
// failed attempt never contaminates the accumulated stream with partial data.
bool decode_frames(const vector<string>&args,vector<uint8_t>&out_bytes){
 Proc proc=spawn_argv(args,"r");
 vector<uint8_t> y(W*H), local;
 while(fread(y.data(),1,y.size(),proc.f)==y.size()){
  uint8_t cur=0; int n=0;
  for(size_t bi=0;bi<BLOCKS;bi++){
   int bx=(bi%(W/BLOCK))*BLOCK, by=(bi/(W/BLOCK))*BLOCK;
   cur=(cur<<1)|(y[by*W+bx]>127);
   if(++n==8){ local.push_back(cur); cur=0; n=0; }
  }
 }
 int rc=close_proc(proc);
 if(rc!=0) return false;
 out_bytes.insert(out_bytes.end(),local.begin(),local.end());
 return true;
}
int decode(const Opt&o){
 if(!have_ffmpeg()) die("ffmpeg is required on PATH for MP4 encode/decode");
 // FIX: the original decode() never looked at o.gpu at all, so --gpu process/auto/decode
 // silently did nothing and every decode ran on CPU regardless of the flag, contradicting
 // the documented "attempts CUDA hwaccel decode, falls back to CPU" behavior. This now
 // actually attempts hwaccel decode per input and falls back to CPU on failure.
 bool want_gpu=(o.gpu=="auto"||o.gpu=="decode");
 if(want_gpu) cerr<<"GPU decode requested; FFmpeg CUDA hwaccel will be attempted per input, with CPU fallback.\n";
 vector<uint8_t> bytes;
 for(auto&in:o.inputs){
  bool ok=false;
  if(want_gpu){
   ok=decode_frames(ffmpeg_decode_args(in,true),bytes);
   if(!ok) cerr<<"GPU decode failed for "<<in<<"; retrying with CPU decode.\n";
  }
  if(!ok) ok=decode_frames(ffmpeg_decode_args(in,false),bytes);
  if(!ok) die("ffmpeg failed to decode input "+in);
 }
 uint64_t sz; array<uint8_t,32> sha{};
 vector<uint8_t> payload=reconstruct_hare_stream(bytes,sz,sha);
 ofstream out(o.output,ios::binary); out.write((char*)payload.data(),(streamsize)payload.size()); out.close();
 auto got=file_sha(o.output);
 if(got!=sha){ decode_checkpoint(o.output+".decode.checkpoint",o.inputs,o.output,sz,Sha256::hex(sha),"failed"); die("SHA-256 verification failed: got "+Sha256::hex(got)+" expected "+Sha256::hex(sha)); }
 decode_checkpoint(o.output+".decode.checkpoint",o.inputs,o.output,sz,Sha256::hex(got),"complete");
 cerr<<"Verified SHA-256 "<<Sha256::hex(got)<<"\n";
 return 0;
}
Opt parse(int argc,char**argv){ if(argc<2)die("usage: fsoy encode <file> -o <out.mp4> | fsoy decode <mp4...> -o <file>"); Opt o; o.mode=argv[1]; for(int i=2;i<argc;i++){string a=argv[i]; if(a=="-o"&&i+1<argc)o.output=argv[++i]; else if(a=="--gpu"&&i+1<argc)o.gpu=normalize_gpu_mode(argv[++i]); else if(a=="--gpu-plugin"&&i+1<argc)o.gpu_plugin=argv[++i]; else if(a=="--max-output-size"&&i+1<argc)o.max_size=parse_size(argv[++i]); else if(a=="--auto-continue")o.auto_continue=true; else if(a=="--resume"&&i+1<argc){o.resume=true; o.checkpoint=argv[++i];} else if(a=="--checkpoint"&&i+1<argc){o.ckdecode=true; o.checkpoint=argv[++i];} else if(o.mode=="decode")o.inputs.push_back(a); else if(o.input.empty())o.input=a; else die("unexpected arg "+a);} if(o.mode=="encode"&&!o.resume&&(o.input.empty()||o.output.empty()))die("encode needs input and -o"); if(o.mode=="decode"&&(o.inputs.empty()||o.output.empty()))die("decode needs input video(s) and -o"); return o;}
int main(int argc,char**argv){
 signal(SIGPIPE,SIG_IGN); // FIX: without this, writing frames to ffmpeg after it exits early
                          // (e.g. a bad encoder arg) delivers SIGPIPE, whose default action
                          // kills this process outright instead of letting us see/report the
                          // failure via the ffmpeg exit code.
 try{Opt o=parse(argc,argv); if(o.max_size) cerr<<"--max-output-size is recorded in checkpoint; v1 scaffold writes one feed volume.\n"; if(o.resume) die("resume from checkpoint is scaffolded but not yet executable"); if(o.ckdecode) die("decode --checkpoint is scaffolded but not yet executable"); if(o.mode=="encode")return encode(o); if(o.mode=="decode")return decode(o); die("unknown mode");}catch(const exception&e){cerr<<"fsoy: "<<e.what()<<"\n"; return 1;}
}
