#include "sidecar_store.h"

#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>

#ifndef _WIN32
#include <unistd.h>
#else
#include <process.h>
#endif

namespace {
int Fail(const char* message) { std::cerr << "FAIL: " << message << '\n'; return 1; }
}
int main() {
#ifndef _WIN32
    const std::string base="/tmp/d2-pop-sidecar-test-"+std::to_string(static_cast<unsigned long>(::getpid()));
#else
    const std::string base="d2-pop-sidecar-test-"+
        std::to_string(static_cast<unsigned long>(::_getpid()));
#endif
    const std::string file=base+".dat", corrupt=base+".bad";
    std::remove(file.c_str()); std::remove(corrupt.c_str());
    d2population::RoomKey a{"hero:guid-17",2,0x12345678u,42,-3,9};
    d2population::RoomKey b=a; b.room_y=10;
    d2population::SidecarStore store; std::string error;
    if(!store.Load(file,&error)||store.Size()!=0) return Fail("missing file should load as empty");
    store.Set(a,{12,7});store.Set(b,{3,1});
    if(!store.DecrementLiving(a)||!store.Save(file,&error)) return Fail("decrement and save should succeed");
    d2population::SidecarStore loaded;
    if(!loaded.Load(file,&error)||loaded.Size()!=2) return Fail("saved store should reload");
    d2population::RoomBudget value{};
    if(!loaded.Get(a,&value)||value.initial_ordinary_count!=12||value.living_budget!=6) return Fail("room counts should round-trip");
    if(!loaded.Get(b,&value)||value.initial_ordinary_count!=3||value.living_budget!=1) return Fail("room coordinate should distinguish entries");
    d2population::RoomKey distinct=a; distinct.character_identity="other";
    if(loaded.Get(distinct,&value)) return Fail("character identity should distinguish entries");
    if(loaded.DecrementLiving(b)==false||loaded.DecrementLiving(b)) return Fail("living budget must stop at zero");

    { std::ifstream in(file,std::ios::binary); std::string bytes((std::istreambuf_iterator<char>(in)),{}); if(bytes.empty()) return Fail("saved file should not be empty"); bytes.back()^=1; std::ofstream out(corrupt,std::ios::binary);out.write(bytes.data(),static_cast<std::streamsize>(bytes.size())); }
    d2population::SidecarStore rejected;rejected.Set(a,{99,99});
    if(rejected.Load(corrupt,&error)||rejected.Size()!=1) return Fail("corrupt file should fail without changing live state");
    if(!error.size()) return Fail("corrupt-file error should be described");
    std::remove(file.c_str());std::remove(corrupt.c_str());
    std::cout << "sidecar_store_test: all checks passed\n";
    return 0;
}
