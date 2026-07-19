#include "sha256.h"
#include "gpu_loader.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#include <unistd.h>
#include <sys/wait.h>
using namespace std; namespace fs=std::filesystem; //
// v2 defaults: 1x1 tile (one payload bit per pixel, no 4x4 blocking), 2-color (B/W) palette.
// W/H/FPS stay fixed constants (matches the "assume 1080p" design); tile/palette are runtime
// values because they're now recorded in the stream header itself (see build_fsoy_stream) so a
// lone .mp4 -- e.g. one that's been round-tripped through YouTube and lost its checkpoint -- can
// still be decoded correctly without external metadata.
constexpr int W=1920,H=1080,FPS=24; constexpr size_t CHUNK_SIZE=1024, ECC_GROUP=10;
constexpr int DEFAULT_TILE=1, DEFAULT_PALETTE=2;
inline size_t blocks_for_tile(int tile){ return (size_t)(W/tile)*(size_t)(H/tile); }
inline size_t bytes_per_frame_for_tile(int tile){ return blocks_for_tile(tile)/8; }
struct Opt{string mode,input,output,checkpoint,gpu="off",gpu_plugin; vector<string> inputs; uint64_t max_size=0; bool resume=false, ckdecode=false, auto_continue=false; int tile=DEFAULT_TILE, palette=DEFAULT_PALETTE;};
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
string human_size(uint64_t bytes){
 static const char* units[]={"B","KB","MB","GB","TB"};
 double v=(double)bytes; int u=0;
 while(v>=1024.0 && u<4){v/=1024.0; u++;}
 ostringstream o; o<<fixed<<setprecision(u==0?0:1)<<v<<" "<<units[u];
 return o.str();
}
array<uint8_t,32> file_sha(const string&p){ifstream f(p,ios::binary); if(!f)die("cannot open "+p); Sha256 s; vector<uint8_t>b(1<<20); while(f){f.read((char*)b.data(),b.size()); if(f.gcount())s.update(b.data(),f.gcount());} return s.final();}
// FSOY v2 stream layout, split into two framing phases (see rationale below):
//   HEADER (exactly HEADER_BYTES, always pixel-sampled at tile=1, occupies frame 0 by itself):
//     magic(8) | orig_size u64(8) | sha256(32) | num_data_chunks u64(8) | chunk_size u32(4) |
//     ecc_group u32(4) | tile u32(4) | palette u32(4) | width u32(4) | height u32(4) | fps u32(4)
//   BODY (pixel-sampled at the header's recorded `tile`, starting at frame 1):
//     per ECC group: g data chunks of [checksum32(4) + chunk_size bytes], followed by one parity
//     chunk [checksum32(4) + chunk_size bytes], where g = min(ecc_group, chunks remaining) and
//     parity = XOR of that group's g data chunks (zero-padded). Gives single-chunk erasure/corruption
//     recovery per group, at ~1/ecc_group (10%) storage overhead.
//
// WHY THE HEADER IS PINNED TO tile=1 AND ITS OWN FRAME: the body's tile size is itself a header
// field, so decode can't know how to sample the body's pixels until it has read the header -- but it
// also can't know how to sample the header's pixels correctly unless the header's tile size is fixed
// and known in advance. Pinning the header to tile=1 (finest possible granularity, unconditionally)
// breaks that chicken-and-egg problem: decode always reads frame 0 one-pixel-per-bit to learn the
// real body tile size, then switches sampling stride for frame 1 onward. Wasting one whole frame on
// an 84-byte header is a rounding error next to typical payload sizes.
//
// v2 vs v1 (HARE): v1 hardcoded a 4x4 luma block per bit and never recorded that fact anywhere in the
// stream. v2 defaults to a 1x1 body tile (one payload bit per pixel) for much higher density, and --
// because a video file can lose its sidecar checkpoint (e.g. after a YouTube upload/download round
// trip) -- tile size, palette, and frame geometry are now embedded directly in the stream itself so
// decode is fully self-describing from the video alone, no external metadata required.
//
// NOTE (reliability caveat, not a code bug): the ECC above can only repair a single *chunk-boundary-
// aligned* erasure/corruption per ECC group. Real-world corruption of the underlying compressed video
// bitstream (e.g. a single bit-flip, or lossy re-encoding such as YouTube's transcode ladder) typically
// cascades across many pixels/blocks/frames due to intra/inter prediction and entropy coding, and will
// usually corrupt far more than one chunk per group of ECC_GROUP -- more so at tile=1 than the old
// tile=4 default, since 1x1 payload pixels are the worst case for block-transform video compression
// (see design notes / README). This ECC is best understood as protection against a cleanly missing or
// replaced data chunk, not as a guarantee against arbitrary lossy-recompression corruption -- test
// against your actual YouTube round-trip and treat results as empirical, not assumed.
constexpr size_t HEADER_BYTES = 8+8+32+8+4+4+4+4+4+4+4; // = 84
vector<uint8_t> build_fsoy_header(uint64_t out_sz,const array<uint8_t,32>&out_sha,uint64_t num_data_chunks,int tile,int palette){
 vector<uint8_t> h; string magic="FSOYV2\0\0"s;
 h.insert(h.end(),magic.begin(),magic.end());
 put64(h,out_sz); h.insert(h.end(),out_sha.begin(),out_sha.end());
 put64(h,num_data_chunks); put32(h,(uint32_t)CHUNK_SIZE); put32(h,(uint32_t)ECC_GROUP);
 put32(h,(uint32_t)tile); put32(h,(uint32_t)palette); put32(h,(uint32_t)W); put32(h,(uint32_t)H); put32(h,(uint32_t)FPS);
 if(h.size()!=HEADER_BYTES) die("internal error: header size mismatch");
 return h;
}
vector<uint8_t> build_fsoy_body(const string&input_path,uint64_t out_sz){
 vector<uint8_t> file_bytes(out_sz);
 if(out_sz){ ifstream f(input_path,ios::binary); if(!f)die("cannot open "+input_path); f.read((char*)file_bytes.data(),(streamsize)out_sz); }
 uint64_t num_data_chunks = out_sz==0 ? 1 : (out_sz+CHUNK_SIZE-1)/CHUNK_SIZE;
 vector<uint8_t> stream;
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
// Header parser -- inverse of build_fsoy_header. Always called on exactly HEADER_BYTES read from
// frame 0 at tile=1 (see framing rationale above build_fsoy_header). Populates every header field,
// including tile/palette so the caller knows how to sample frame 1 onward for the body.
struct FsoyHeader{ uint64_t out_sz=0; array<uint8_t,32> out_sha{}; uint64_t num_data_chunks=0; uint32_t chunk_size=0, ecc_group=0; int tile=0, palette=0; uint32_t stream_w=0, stream_h=0, stream_fps=0; };
FsoyHeader parse_fsoy_header(const vector<uint8_t>&bytes){
 constexpr size_t MAGIC_LEN=8;
 if(bytes.size()<HEADER_BYTES) die("truncated FSOY v2 header (got "+to_string(bytes.size())+" of "+to_string(HEADER_BYTES)+" bytes -- input video may be too short or corrupted)");
 if(string(bytes.begin(),bytes.begin()+MAGIC_LEN)!=string("FSOYV2\0\0"s)) die("not an FSOY v2 stream (bad magic -- is this a v1/HARE file? decode it with the v1 build)");
 FsoyHeader h; size_t pos=MAGIC_LEN;
 h.out_sz=get64(bytes,pos); pos+=8;
 copy(bytes.begin()+pos,bytes.begin()+pos+32,h.out_sha.begin()); pos+=32;
 h.num_data_chunks=get64(bytes,pos); pos+=8;
 h.chunk_size=get32(bytes,pos); pos+=4;
 h.ecc_group=get32(bytes,pos); pos+=4;
 h.tile=(int)get32(bytes,pos); pos+=4;
 h.palette=(int)get32(bytes,pos); pos+=4;
 h.stream_w=get32(bytes,pos); pos+=4;
 h.stream_h=get32(bytes,pos); pos+=4;
 h.stream_fps=get32(bytes,pos); pos+=4;
 if(h.chunk_size==0||h.ecc_group==0||h.tile==0) die("corrupt FSOY v2 header (zero chunk_size/ecc_group/tile)");
 if(h.palette!=2) die("unsupported palette "+to_string(h.palette)+" (this build only supports palette=2)");
 if((int)h.stream_w!=W || (int)h.stream_h!=H) die("stream was encoded at "+to_string(h.stream_w)+"x"+to_string(h.stream_h)+", this build expects "+to_string(W)+"x"+to_string(H));
 if((int)h.stream_fps!=FPS) cerr<<"Warning: stream recorded fps="<<h.stream_fps<<", this build uses fps="<<FPS<<" (informational only, frame reader doesn't depend on it).\n";
 return h;
}
// Inverse of build_fsoy_body. Verifies each chunk's checksum; if exactly one chunk (data or parity) in a
// group fails, repairs a bad data chunk via XOR of the group's other chunks + parity. More than one failure
// per group is beyond this scheme's repair capacity and is reported as an unrecoverable error.
vector<uint8_t> reconstruct_fsoy_body(const vector<uint8_t>&bytes,const FsoyHeader&h){
 size_t pos=0;
 vector<uint8_t> out; out.reserve((size_t)min<uint64_t>(h.num_data_chunks*(uint64_t)h.chunk_size, (uint64_t)1<<34));
 uint64_t idx=0, group=0;
 while(idx<h.num_data_chunks){
  uint64_t g=min<uint64_t>(h.ecc_group,h.num_data_chunks-idx);
  vector<vector<uint8_t>> chunks(g); vector<bool> ok(g,false);
  for(uint64_t j=0;j<g;j++){
   if(pos+4+h.chunk_size>bytes.size()) die("truncated FSOY stream in group "+to_string(group));
   uint32_t stored=get32(bytes,pos); pos+=4;
   chunks[j].assign(bytes.begin()+pos,bytes.begin()+pos+h.chunk_size); pos+=h.chunk_size;
   ok[j]=(checksum32(chunks[j].data(),chunks[j].size())==stored);
  }
  if(pos+4+h.chunk_size>bytes.size()) die("truncated FSOY stream (missing parity) in group "+to_string(group));
  uint32_t parity_stored=get32(bytes,pos); pos+=4;
  vector<uint8_t> parity(bytes.begin()+pos,bytes.begin()+pos+h.chunk_size); pos+=h.chunk_size;
  bool parity_ok=(checksum32(parity.data(),parity.size())==parity_stored);
  int bad=0; int bad_idx=-1;
  for(uint64_t j=0;j<g;j++) if(!ok[j]){bad++; bad_idx=(int)j;}
  if(!parity_ok) bad++;
  if(bad==0 || (bad==1 && !parity_ok)){
   for(uint64_t j=0;j<g;j++) out.insert(out.end(),chunks[j].begin(),chunks[j].end());
  } else if(bad==1 && bad_idx>=0){
   vector<uint8_t> recovered=parity;
   for(uint64_t j=0;j<g;j++) if((int)j!=bad_idx) for(size_t b=0;b<h.chunk_size;b++) recovered[b]^=chunks[j][b];
   chunks[bad_idx]=recovered;
   cerr<<"Repaired chunk "<<(idx+bad_idx)<<" in ECC group "<<group<<" using XOR parity.\n";
   for(uint64_t j=0;j<g;j++) out.insert(out.end(),chunks[j].begin(),chunks[j].end());
  } else {
   die("unrecoverable corruption in ECC group "+to_string(group)+": more than one chunk failed checksum (this scheme repairs at most 1 bad chunk per group of "+to_string(g)+")");
  }
  idx+=g; group++;
 }
 if(out.size()<h.out_sz) die("reconstructed stream shorter than recorded size");
 out.resize(h.out_sz);
 return out;
}
string json_escape(const string&s){string o; for(char ch:s){if(ch=='\\'||ch=='"') {o.push_back('\\'); o.push_back(ch);} else if(ch=='\n') o+="\\n"; else o.push_back(ch);} return o;}
string checkpoint_field(const string&path,const string&key){ifstream f(path); if(!f)die("cannot open checkpoint "+path); string data((istreambuf_iterator<char>(f)),{}); string needle="\""+key+"\""; size_t p=data.find(needle); if(p==string::npos)return {}; p=data.find(':',p); if(p==string::npos)return {}; p=data.find('"',p); if(p==string::npos)return {}; size_t e=data.find('"',p+1); if(e==string::npos)return {}; return data.substr(p+1,e-p-1);}
void encode_checkpoint(const string&path,const string&src,const string&out,uint64_t size,const array<uint8_t,32>&sha,const string&status,uint64_t byte_offset,int tile,int palette){ofstream c(path); c<<"{\n  \"type\": \"encode\",\n  \"source_path\": \""<<json_escape(src)<<"\",\n  \"source_size\": "<<size<<",\n  \"sha256\": \""<<Sha256::hex(sha)<<"\",\n  \"fsoy_version\": 2,\n  \"tile\": "<<tile<<",\n  \"palette\": "<<palette<<",\n  \"ecc_ratio\": 0.10,\n  \"fps\": "<<FPS<<",\n  \"byte_offset\": "<<byte_offset<<",\n  \"volume_index\": 1,\n  \"output_path\": \""<<json_escape(out)<<"\",\n  \"outputs\": [\""<<json_escape(out)<<"\"],\n  \"status\": \""<<status<<"\"\n}\n";}
void decode_checkpoint(const string&path,const vector<string>&inputs,const string&out,uint64_t payload_size,const string&sha,const string&status,int tile,int palette){ofstream c(path); c<<"{\n  \"type\": \"decode\",\n  \"inputs\": ["; for(size_t i=0;i<inputs.size();++i){if(i)c<<", "; c<<"\""<<json_escape(inputs[i])<<"\"";} c<<"],\n  \"output_path\": \""<<json_escape(out)<<"\",\n  \"decoded_payload_size\": "<<payload_size<<",\n  \"sha256\": \""<<sha<<"\",\n  \"fsoy_version\": 2,\n  \"tile\": "<<tile<<",\n  \"palette\": "<<palette<<",\n  \"fps\": "<<FPS<<",\n  \"status\": \""<<status<<"\"\n}\n";}

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
// v2 default encode path is genuinely lossless FFV1 in an MKV container. This replaces v1's
// "libx264 -crf 0", which -- despite being lossless -- is still a DCT/CABAC block-transform coder;
// FFV1 is a dedicated lossless codec (range-coded, no transform/quantization step at all) and is
// the standard choice when "no information may be discarded" is a hard requirement, which matters
// more at tile=1 since single-pixel payload data has zero spatial redundancy for a coder to exploit.
// NOTE: FFV1 is intentionally NOT what gets handed to YouTube -- YouTube re-transcodes every upload
// to its own (lossy) ladder regardless of input codec, so there is no "YouTube-safe" codec choice on
// the encode side. The .mkv this produces is the lossless local artifact; if/when the user uploads it,
// decode must (and already does, via ffmpeg's rawvideo pixel demux) tolerate whatever comes back.
// GPU modes still request NVENC H.264 (no lossless NVENC FFV1 equivalent exists), so --gpu encode/auto
// remains a lossy path by nature of the hardware encoder available; this is called out to the user at
// runtime rather than silently swapped.
vector<string> ffmpeg_encode_args(const string&o,const string&gpu){
 vector<string> a{"ffmpeg","-hide_banner","-loglevel","error","-y","-f","yuv4mpegpipe","-i","-","-c:v"};
 if(gpu=="auto"||gpu=="encode") a.insert(a.end(),{"h264_nvenc","-preset","lossless","-rc","constqp","-qp","0","-pix_fmt","yuv420p"});
 else a.insert(a.end(),{"ffv1","-level","3","-g","1","-slicecrc","1","-pix_fmt","yuv420p"});
 a.insert(a.end(),{o});
 return a;
}
vector<string> ffmpeg_decode_args(const string&i,bool use_gpu){
 vector<string> a{"ffmpeg","-hide_banner","-loglevel","error"};
 if(use_gpu) a.insert(a.end(),{"-hwaccel","cuda"});
 a.insert(a.end(),{"-i",i,"-f","rawvideo","-pix_fmt","gray","-"});
 return a;
}
// tile=1 (v2 default) maps each payload bit to exactly one 1x1 pixel, hard-thresholded black/white --
// no dithering/error-diffusion, since any softening of the black/white edge would only hurt bit
// recovery on decode, not help compression survive.
void synthesize_frame_cpu(vector<uint8_t>&y,const vector<uint8_t>&data,size_t off,int tile){
 fill(y.begin(),y.end(),128);
 size_t blocks=blocks_for_tile(tile);
 int cols=W/tile;
 for(size_t bi=0;bi<blocks;bi++){
  size_t byte=off+bi/8; bool bit= byte<data.size() && ((data[byte]>>(7-(bi%8)))&1);
  int bx=(int)(bi%cols)*tile, by=(int)(bi/cols)*tile;
  uint8_t val=bit?255:0;
  for(int yy=0;yy<tile;yy++) memset(&y[(by+yy)*W+bx],val,tile);
 }
}
void write_frame(FILE*pipe,const vector<uint8_t>&data,size_t off,const GpuPlugin*gpu,int tile){
 static vector<uint8_t> y(W*H), uv((W/2)*(H/2),128);
 bool gpu_ok=false;
 if(gpu && gpu->available()){FsoyGpuFrameJob job{y.data(),W,H,tile,data.data(),data.size(),off}; gpu_ok=gpu->synthesize_luma_frame(job);}
 if(!gpu_ok) synthesize_frame_cpu(y,data,off,tile);
 fwrite("FRAME\n",1,6,pipe); fwrite(y.data(),1,y.size(),pipe); fwrite(uv.data(),1,uv.size(),pipe); fwrite(uv.data(),1,uv.size(),pipe);
}
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
 if(o.gpu=="auto") cerr<<"GPU process suite requested; frame synthesis and NVENC will be attempted with CPU fallback where possible. Note: NVENC has no lossless-FFV1 equivalent, so this path encodes lossy H.264 even though CPU mode (--gpu off) is lossless.\n";
 if(o.gpu=="encode") cerr<<"GPU encode requested; NVENC will be attempted (lossy H.264) and CPU libx264 used if unavailable. For the default lossless FFV1 path, use --gpu off.\n";
 if(o.palette!=2) die("unsupported --palette "+to_string(o.palette)+" (this build only supports palette=2)");
 if(o.tile<1) die("--tile must be a positive integer");
 if(W%o.tile!=0 || H%o.tile!=0) die("--tile "+to_string(o.tile)+" doesn't evenly divide the "+to_string(W)+"x"+to_string(H)+" frame");
 size_t body_bytes_per_frame=bytes_per_frame_for_tile(o.tile);
 size_t header_bytes_per_frame=bytes_per_frame_for_tile(1);
 uint64_t sz=fs::file_size(o.input); array<uint8_t,32> sha=file_sha(o.input);
 uint64_t num_data_chunks = sz==0 ? 1 : (sz+CHUNK_SIZE-1)/CHUNK_SIZE;
 vector<uint8_t> header=build_fsoy_header(sz,sha,num_data_chunks,o.tile,o.palette);
 vector<uint8_t> body=build_fsoy_body(o.input,sz);
 // +1 for the dedicated header frame (always tile=1; see framing rationale above build_fsoy_header).
 uint64_t body_frames = body.empty()?0:(body.size()+body_bytes_per_frame-1)/body_bytes_per_frame;
 uint64_t total_frames = 1+body_frames;
 // Upfront estimate. We know the exact frame count (and thus video duration) before encoding, since
 // that's purely a function of payload size and tile size. We can NOT know the exact compressed output
 // size ahead of time for the GPU/NVENC (lossy) path -- that depends on data entropy -- but the default
 // CPU/FFV1 path is lossless, so its output size is close to the raw payload size regardless of content.
 cerr<<"Input: "<<human_size(sz)<<" -> FSOY body "<<human_size(body.size())<<" (tile="<<o.tile<<", palette="<<o.palette<<", with ECC overhead) + 1 header frame\n";
 cerr<<"Will encode "<<total_frames<<" frame(s), ~"<<fixed<<setprecision(1)<<((double)total_frames/FPS)<<"s of video @ "<<FPS<<"fps.\n";
 Proc proc=spawn_argv(ffmpeg_encode_args(o.output,o.gpu),"w");
 fprintf(proc.f,"YUV4MPEG2 W%d H%d F%d:1 Ip A1:1 C420jpeg\n",W,H,FPS);
 // Live progress -- frame count is exact (we're the ones writing them), and every so often we also
 // stat() the output file on disk to show how large the output has grown so far.
 uint64_t frame_i=0; auto last=chrono::steady_clock::now(); bool any_progress_line=false;
 write_frame(proc.f,header,0,frame_gpu,1); frame_i++; // frame 0: header, always tile=1
 for(size_t off=0;off<body.size();off+=body_bytes_per_frame){
  write_frame(proc.f,body,off,frame_gpu,o.tile); frame_i++;
  auto now=chrono::steady_clock::now();
  if(chrono::duration_cast<chrono::milliseconds>(now-last).count()>=500 || frame_i==total_frames){
   last=now; any_progress_line=true;
   error_code ec; uint64_t cur=fs::file_size(o.output,ec);
   double pct = total_frames? (100.0*(double)frame_i/(double)total_frames) : 100.0;
   cerr<<"\rEncoding: frame "<<frame_i<<"/"<<total_frames<<" ("<<fixed<<setprecision(1)<<pct<<"%) -- "
       <<"output so far: "<<(ec?string("0 B"):human_size(cur))<<"          "<<flush;
  }
 }
 (void)header_bytes_per_frame;
 if(any_progress_line) cerr<<"\n";
 int rc=close_proc(proc);
 if(rc!=0 && (o.gpu=="auto"||o.gpu=="encode")){ cerr<<"GPU encode failed; retrying with CPU libx264.\n"; Opt c=o; c.gpu="off"; return encode(c);}
 uint64_t total_payload_bytes = header.size()+body.size();
 if(rc!=0){ encode_checkpoint(o.output+".encode.checkpoint",o.input,o.output,sz,sha,"failed",total_payload_bytes,o.tile,o.palette); die("ffmpeg encode failed"); }
 encode_checkpoint(o.output+".encode.checkpoint",o.input,o.output,sz,sha,"complete",total_payload_bytes,o.tile,o.palette);
 error_code ec; uint64_t final_sz=fs::file_size(o.output,ec);
 if(!ec) cerr<<"Done. Final output size: "<<human_size(final_sz)<<"\n";
 return 0;
}
// Extracts one frame's worth of bits from a raw grayscale plane at the given tile stride, appending
// whole bytes to local (a trailing partial byte, if any, is carried forward via cur/n by the caller
// -- but since bytes_per_frame_for_tile(tile) is always a whole number of bytes for our W/H/tile
// combinations, each call here starts and ends on a byte boundary, so no cross-frame bit carry is needed).
void extract_frame_bits(const vector<uint8_t>&y,int tile,vector<uint8_t>&local){
 size_t blocks=blocks_for_tile(tile); int cols=W/tile;
 uint8_t cur=0; int n=0;
 for(size_t bi=0;bi<blocks;bi++){
  int bx=(int)(bi%cols)*tile, by=(int)(bi/cols)*tile;
  cur=(cur<<1)|(y[(size_t)by*W+bx]>127);
  if(++n==8){ local.push_back(cur); cur=0; n=0; }
 }
}
// Reads frames from one ffmpeg process. frame 0 (only meaningful for the first input of a decode job)
// is always tile=1 (the header frame); every frame after that uses body_tile. Returns false (leaving
// header_bytes/body_bytes untouched) if ffmpeg exits non-zero, so a failed attempt never contaminates
// the accumulated stream with partial data. If want_header is false, frame 0 is still consumed from the
// pipe (so later frames align correctly) but its bits are discarded -- used for 2nd+ input videos in a
// multi-volume decode, where only the first input carries the real header frame.
bool decode_frames(const vector<string>&args,bool want_header,int body_tile,vector<uint8_t>&header_bytes,vector<uint8_t>&body_bytes,const string&label){
 Proc proc=spawn_argv(args,"r");
 vector<uint8_t> y(W*H); vector<uint8_t> local_header, local_body;
 uint64_t frame_i=0; auto last=chrono::steady_clock::now(); bool any_progress_line=false;
 // Live progress. Unlike encode, decode can't know the total frame count of an input video up front
 // without a separate ffprobe-style pass, so this reports running counts (frames read so far, bytes
 // reconstructed so far) rather than a percentage -- still far better than complete silence during
 // what can be a multi-second-to-multi-minute read for large inputs.
 while(fread(y.data(),1,y.size(),proc.f)==y.size()){
  if(frame_i==0){ if(want_header) extract_frame_bits(y,1,local_header); }
  else extract_frame_bits(y,body_tile,local_body);
  frame_i++;
  auto now=chrono::steady_clock::now();
  if(chrono::duration_cast<chrono::milliseconds>(now-last).count()>=500){
   last=now; any_progress_line=true;
   cerr<<"\rDecoding "<<label<<": frame "<<frame_i<<" read, "<<human_size(local_body.size())<<" reconstructed so far          "<<flush;
  }
 }
 if(any_progress_line) cerr<<"\rDecoding "<<label<<": frame "<<frame_i<<" read, "<<human_size(local_body.size())<<" reconstructed so far          \n";
 int rc=close_proc(proc);
 if(rc!=0) return false;
 header_bytes.insert(header_bytes.end(),local_header.begin(),local_header.end());
 body_bytes.insert(body_bytes.end(),local_body.begin(),local_body.end());
 return true;
}
int decode(const Opt&o){
 if(!have_ffmpeg()) die("ffmpeg is required on PATH for MP4 encode/decode");
 // The original decode() never looked at o.gpu at all, so --gpu process/auto/decode silently did
 // nothing and every decode ran on CPU regardless of the flag, contradicting the documented "attempts
 // CUDA hwaccel decode, falls back to CPU" behavior. This actually attempts hwaccel decode per input
 // and falls back to CPU on failure.
 bool want_gpu=(o.gpu=="auto"||o.gpu=="decode");
 if(want_gpu) cerr<<"GPU decode requested; FFmpeg CUDA hwaccel will be attempted per input, with CPU fallback.\n";
 // Two-pass framing (see build_fsoy_header rationale): we don't know the real body tile size until
 // we've read and parsed frame 0 of the FIRST input at tile=1. So the first input is decoded once to
 // fetch just enough (the header) and body_tile stays 1 for that pass; every input (including the
 // first) is then re-decoded (or in the first input's case, re-used) once body_tile is known. To avoid
 // decoding the first input's video stream twice, we hold body_tile=1 for its single pass and instead
 // re-slice its already-read raw frame bytes in memory once we know the real tile -- but since ffmpeg
 // output isn't buffered in full here (streamed), the simplest correct approach is: decode input 1
 // once assuming tile=1 for frame 0 only (header), parse it, then decode ALL inputs (including input 1
 // again) at the now-known body_tile. This costs one extra ffmpeg pass over the first input, which is
 // cheap relative to the size of typical payload videos and keeps the logic simple and correct.
 if(o.inputs.empty()) die("decode needs at least one input video");
 const string& first=o.inputs.front();
 vector<uint8_t> probe_header, probe_body_unused;
 {
  error_code ec; uint64_t in_sz=fs::file_size(first,ec);
  cerr<<"Probing "<<first<<(ec?string():" ("+human_size(in_sz)+")")<<" for FSOY header...\n";
  bool ok=decode_frames(ffmpeg_decode_args(first,false),true,1,probe_header,probe_body_unused,first);
  if(!ok) die("ffmpeg failed to read header frame from "+first);
 }
 FsoyHeader h=parse_fsoy_header(probe_header);
 cerr<<"Header: tile="<<h.tile<<" palette="<<h.palette<<" orig_size="<<human_size(h.out_sz)<<"\n";
 vector<uint8_t> header_bytes, body_bytes;
 for(auto&in:o.inputs){
  error_code ec; uint64_t in_sz=fs::file_size(in,ec);
  cerr<<"Reading "<<in<<(ec?string():" ("+human_size(in_sz)+")")<<"...\n";
  bool want_header=(&in==&o.inputs.front());
  bool ok=false;
  if(want_gpu){
   ok=decode_frames(ffmpeg_decode_args(in,true),want_header,h.tile,header_bytes,body_bytes,in);
   if(!ok) cerr<<"GPU decode failed for "<<in<<"; retrying with CPU decode.\n";
  }
  if(!ok) ok=decode_frames(ffmpeg_decode_args(in,false),want_header,h.tile,header_bytes,body_bytes,in);
  if(!ok) die("ffmpeg failed to decode input "+in);
 }
 cerr<<"Reassembling and verifying FSOY stream ("<<human_size(body_bytes.size())<<" of decoded frame data)...\n";
 vector<uint8_t> payload=reconstruct_fsoy_body(body_bytes,h);
 ofstream out(o.output,ios::binary); out.write((char*)payload.data(),(streamsize)payload.size()); out.close();
 auto got=file_sha(o.output);
 if(got!=h.out_sha){ decode_checkpoint(o.output+".decode.checkpoint",o.inputs,o.output,h.out_sz,Sha256::hex(h.out_sha),"failed",h.tile,h.palette); die("SHA-256 verification failed: got "+Sha256::hex(got)+" expected "+Sha256::hex(h.out_sha)); }
 decode_checkpoint(o.output+".decode.checkpoint",o.inputs,o.output,h.out_sz,Sha256::hex(got),"complete",h.tile,h.palette);
 cerr<<"Done. Output size: "<<human_size(h.out_sz)<<". Verified SHA-256 "<<Sha256::hex(got)<<"\n";
 return 0;
}
Opt parse(int argc,char**argv){ if(argc<2)die("usage: fsoy encode <file> -o <out.mkv> [--tile N] [--palette 2] | fsoy decode <video...> -o <file>"); Opt o; o.mode=argv[1]; for(int i=2;i<argc;i++){string a=argv[i]; if(a=="-o"&&i+1<argc)o.output=argv[++i]; else if(a=="--gpu"&&i+1<argc)o.gpu=normalize_gpu_mode(argv[++i]); else if(a=="--gpu-plugin"&&i+1<argc)o.gpu_plugin=argv[++i]; else if(a=="--max-output-size"&&i+1<argc)o.max_size=parse_size(argv[++i]); else if(a=="--auto-continue")o.auto_continue=true; else if(a=="--tile"&&i+1<argc)o.tile=stoi(argv[++i]); else if(a=="--palette"&&i+1<argc)o.palette=stoi(argv[++i]); else if(a=="--resume"&&i+1<argc){o.resume=true; o.checkpoint=argv[++i];} else if(a=="--checkpoint"&&i+1<argc){o.ckdecode=true; o.checkpoint=argv[++i];} else if(o.mode=="decode")o.inputs.push_back(a); else if(o.input.empty())o.input=a; else die("unexpected arg "+a);} if(o.mode=="encode"&&!o.resume&&(o.input.empty()||o.output.empty()))die("encode needs input and -o"); if(o.mode=="decode"&&(o.inputs.empty()||o.output.empty()))die("decode needs input video(s) and -o"); return o;}
int main(int argc,char**argv){
 signal(SIGPIPE,SIG_IGN); // Without this, writing frames to ffmpeg after it exits early (e.g. a bad
                          // encoder arg) delivers SIGPIPE, whose default action kills this process
                          // outright instead of letting us see/report the failure via ffmpeg's exit code.
 try{Opt o=parse(argc,argv); if(o.max_size) cerr<<"--max-output-size is recorded in checkpoint; v1 scaffold writes one feed volume.\n"; if(o.resume) die("resume from checkpoint is scaffolded but not yet executable"); if(o.ckdecode) die("decode --checkpoint is scaffolded but not yet executable"); if(o.mode=="encode")return encode(o); if(o.mode=="decode")return decode(o); die("unknown mode");}catch(const exception&e){cerr<<"fsoy: "<<e.what()<<"\n"; return 1;}
}
