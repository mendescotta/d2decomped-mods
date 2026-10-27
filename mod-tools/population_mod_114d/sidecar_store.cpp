#include "sidecar_store.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <filesystem>
#include <iterator>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <sstream>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace d2population {
namespace {
constexpr std::array<std::uint8_t, 8> kMagic{{'D','2','P','O','P','S','C','R'}};
constexpr std::uint32_t kVersion = 1;
constexpr std::uint32_t kMaxEntries = 1000000;
constexpr std::uint32_t kMaxIdentityBytes = 4096;
constexpr std::uintmax_t kMaxFileBytes = 128u * 1024u * 1024u;
std::atomic<std::uint64_t> g_temp_sequence{0};

void Put32(std::vector<std::uint8_t>& b, std::uint32_t v) {
    for (unsigned i=0; i<4; ++i) b.push_back(static_cast<std::uint8_t>(v >> (i*8)));
}
void Put64(std::vector<std::uint8_t>& b, std::uint64_t v) {
    for (unsigned i=0; i<8; ++i) b.push_back(static_cast<std::uint8_t>(v >> (i*8)));
}
bool Take32(const std::vector<std::uint8_t>& b, std::size_t& p, std::uint32_t& v) {
    if (b.size()-p < 4) return false;
    v=0; for (unsigned i=0; i<4; ++i) v |= std::uint32_t(b[p++]) << (i*8); return true;
}
bool Take64(const std::vector<std::uint8_t>& b, std::size_t& p, std::uint64_t& v) {
    if (b.size()-p < 8) return false;
    v=0; for (unsigned i=0; i<8; ++i) v |= std::uint64_t(b[p++]) << (i*8); return true;
}
std::uint64_t Checksum(const std::uint8_t* data, std::size_t size) {
    // FNV-1a 64-bit detects accidental corruption; the format is not intended
    // to provide cryptographic authenticity.
    std::uint64_t h=14695981039346656037ull;
    for (std::size_t i=0; i<size; ++i) { h ^= data[i]; h *= 1099511628211ull; }
    return h;
}
void Error(std::string* out, const std::string& message) { if (out) *out=message; }
bool AtomicReplace(const std::string& path, const std::vector<std::uint8_t>& data, std::string* error) {
    const std::string temp=path+".tmp."+std::to_string(
#ifdef _WIN32
        static_cast<unsigned long>(GetCurrentProcessId())
#else
        static_cast<unsigned long>(::getpid())
#endif
    )+"."+std::to_string(g_temp_sequence.fetch_add(1));
#ifdef _WIN32
    FILE* f=nullptr;
    if (fopen_s(&f,temp.c_str(),"wb") != 0 || !f) { Error(error,"cannot create temporary sidecar"); return false; }
#else
    int fd=::open(temp.c_str(), O_WRONLY|O_CREAT|O_EXCL, 0600);
    if (fd<0) { Error(error,"cannot create temporary sidecar: "+std::string(std::strerror(errno))); return false; }
    FILE* f=::fdopen(fd,"wb");
    if (!f) { ::close(fd); std::remove(temp.c_str()); Error(error,"cannot open temporary sidecar stream"); return false; }
#endif
    bool ok=data.empty() || std::fwrite(data.data(),1,data.size(),f)==data.size();
    if (ok) ok=std::fflush(f)==0;
#ifndef _WIN32
    if (ok) ok=::fsync(::fileno(f))==0;
#endif
    if (std::fclose(f)!=0) ok=false;
    if (!ok) { std::remove(temp.c_str()); Error(error,"failed writing temporary sidecar"); return false; }
#ifdef _WIN32
    if (!MoveFileExA(temp.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)) {
        std::remove(temp.c_str()); Error(error,"cannot atomically replace sidecar"); return false;
    }
#else
    if (std::rename(temp.c_str(),path.c_str())!=0) {
        std::remove(temp.c_str()); Error(error,"cannot atomically replace sidecar: "+std::string(std::strerror(errno))); return false;
    }
    // Persist the directory entry after rename where supported.
    auto slash=path.find_last_of('/');
    std::string dir=slash==std::string::npos?".":(slash==0?"/":path.substr(0,slash));
    int dfd=::open(dir.c_str(),O_RDONLY|O_DIRECTORY);
    if (dfd>=0) { ::fsync(dfd); ::close(dfd); }
#endif
    return true;
}
}

bool RoomKey::operator<(const RoomKey& o) const noexcept {
    if (character_identity!=o.character_identity) return character_identity<o.character_identity;
    if (difficulty!=o.difficulty) return difficulty<o.difficulty;
    if (map_seed!=o.map_seed) return map_seed<o.map_seed;
    if (level_id!=o.level_id) return level_id<o.level_id;
    if (room_x!=o.room_x) return room_x<o.room_x;
    return room_y<o.room_y;
}

bool SidecarStore::Load(const std::string& path, std::string* error) {
    std::error_code fs_error;
    const auto file_size=std::filesystem::file_size(path,fs_error);
    if (fs_error==std::errc::no_such_file_or_directory) {
        std::lock_guard<std::mutex> lock(mutex_); rooms_.clear(); if(error)error->clear(); return true;
    }
    if (fs_error || file_size>kMaxFileBytes) { Error(error,"cannot read sidecar file or file exceeds size limit"); return false; }
    std::ifstream in(path,std::ios::binary);
    if (!in) { Error(error,"cannot read sidecar file"); return false; }
    std::vector<std::uint8_t> b((std::istreambuf_iterator<char>(in)),{});
    if (b.size()!=file_size) { Error(error,"sidecar changed while being read"); return false; }
    constexpr std::size_t header=8+4+4+8;
    if (b.size()<header || !std::equal(kMagic.begin(),kMagic.end(),b.begin())) { Error(error,"invalid sidecar header"); return false; }
    std::size_t p=8; std::uint32_t version=0,count=0; std::uint64_t checksum=0;
    if (!Take32(b,p,version)||!Take32(b,p,count)||!Take64(b,p,checksum)) { Error(error,"truncated sidecar header"); return false; }
    if (version!=kVersion) { Error(error,"unsupported sidecar version"); return false; }
    if (count>kMaxEntries || Checksum(b.data()+header,b.size()-header)!=checksum) { Error(error,"sidecar checksum or entry count is invalid"); return false; }
    std::map<RoomKey,RoomBudget> loaded;
    for (std::uint32_t i=0;i<count;++i) {
        if (p>=b.size()) { Error(error,"truncated sidecar entry"); return false; }
        RoomKey k; k.difficulty=b[p++]; std::uint32_t n=0,seed=0,level=0,x=0,y=0,initial=0,living=0;
        if (!Take32(b,p,n)||n>kMaxIdentityBytes||b.size()-p<n) { Error(error,"invalid sidecar identity"); return false; }
        k.character_identity.assign(reinterpret_cast<const char*>(b.data()+p),n); p+=n;
        if (!Take32(b,p,seed)||!Take32(b,p,level)||!Take32(b,p,x)||!Take32(b,p,y)||!Take32(b,p,initial)||!Take32(b,p,living)) { Error(error,"truncated sidecar entry"); return false; }
        k.map_seed=seed;k.level_id=level;k.room_x=static_cast<std::int32_t>(x);k.room_y=static_cast<std::int32_t>(y);
        if (k.character_identity.empty() || living>initial || !loaded.emplace(k,RoomBudget{initial,living}).second) { Error(error,"invalid or duplicate sidecar room"); return false; }
    }
    if (p!=b.size()) { Error(error,"extra data after sidecar entries"); return false; }
    { std::lock_guard<std::mutex> lock(mutex_); rooms_.swap(loaded); }
    if (error) error->clear();
    return true;
}

bool SidecarStore::Save(const std::string& path, std::string* error) const {
    std::map<RoomKey,RoomBudget> copy;
    { std::lock_guard<std::mutex> lock(mutex_); copy=rooms_; }
    if(copy.size()>kMaxEntries){Error(error,"too many sidecar entries");return false;}
    std::vector<std::uint8_t> payload;
    for(const auto& item:copy){const auto& k=item.first;const auto& v=item.second;
        if(k.character_identity.empty()||k.character_identity.size()>kMaxIdentityBytes||v.living_budget>v.initial_ordinary_count){Error(error,"invalid room budget or character identity");return false;}
        payload.push_back(k.difficulty);Put32(payload,static_cast<std::uint32_t>(k.character_identity.size()));payload.insert(payload.end(),k.character_identity.begin(),k.character_identity.end());
        Put32(payload,k.map_seed);Put32(payload,k.level_id);Put32(payload,static_cast<std::uint32_t>(k.room_x));Put32(payload,static_cast<std::uint32_t>(k.room_y));Put32(payload,v.initial_ordinary_count);Put32(payload,v.living_budget);
    }
    std::vector<std::uint8_t> bytes(kMagic.begin(),kMagic.end());Put32(bytes,kVersion);Put32(bytes,static_cast<std::uint32_t>(copy.size()));Put64(bytes,Checksum(payload.data(),payload.size()));bytes.insert(bytes.end(),payload.begin(),payload.end());
    const bool ok=AtomicReplace(path,bytes,error);if(ok&&error)error->clear();return ok;
}
bool SidecarStore::Get(const RoomKey& k,RoomBudget* v) const { if(!v)return false;std::lock_guard<std::mutex> lock(mutex_);auto i=rooms_.find(k);if(i==rooms_.end())return false;*v=i->second;return true; }
void SidecarStore::Set(const RoomKey& k,RoomBudget v) { std::lock_guard<std::mutex> lock(mutex_);rooms_[k]=v; }
bool SidecarStore::DecrementLiving(const RoomKey& k) { std::lock_guard<std::mutex> lock(mutex_);auto i=rooms_.find(k);if(i==rooms_.end()||i->second.living_budget==0)return false;--i->second.living_budget;return true; }
std::size_t SidecarStore::Size() const { std::lock_guard<std::mutex> lock(mutex_);return rooms_.size(); }
}
