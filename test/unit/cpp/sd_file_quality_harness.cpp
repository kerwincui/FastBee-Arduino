#include "peripherals/SDCardManager.h"
#include "core/SpiBusInitializer.h"
#include <SD.h>
#include <cassert>
#include <string>

int main(int argc,char** argv) {
    assert(argc==2);const std::string mode=argv[1];
    auto& manager=SDCardManager::getInstance();String error,content="old complete snapshot";
    assert(manager.configureAndMount("sd",18,23,19,5,1000000,error));
    if(mode=="empty") {sdFileContent="";sdFileDeclaredSize=0;}
    if(mode=="binary") {sdFileContent=std::string("a\0b",3);}
    if(mode=="short") {sdFileContent="ab";}
    if(mode=="negative") {sdReadFailAt=1;}
    if(mode=="wide") {sdReadFailAt=1;sdReadFault=256;}
    if(mode=="extra") {sdFileContent="abcd";}
    if(mode=="grow") {sdFileFinalSize=4;}
    if(mode=="shrink") {sdFileFinalSize=2;}
    if(mode=="endless") {sdAvailableInfinite=true;}
    if(mode=="oversize") {sdFileDeclaredSize=16385;}
    if(mode=="append_overflow" || mode=="append_unreadable" || mode=="append_boundary") {
        sdFileDeclaredSize=mode=="append_overflow"?static_cast<size_t>(-1):16381;
        if(mode=="append_unreadable")SD.openOk=false;
        const uint8_t bytes[]={1,2,3};
        assert(manager.writeFile("test.txt",bytes,3,true,error,"sd")== (mode=="append_boundary"));
        assert(sdWriteCalls== (mode=="append_boundary"?1U:0U));
        if(mode=="append_unreadable")assert(error.startsWith("SD_APPEND_SIZE_UNCONFIRMED"));
        if(mode=="append_overflow")assert(error.startsWith("SD_FILE_TOO_LARGE"));
    } else {
        const bool ok=manager.readFile("test.txt",content,error,16384,"sd");
        const bool expected=mode=="normal" || mode=="empty" || mode=="binary";
        assert(ok==expected);
        if(expected) {assert(content.length()==sdFileContent.size());assert(std::string(content.c_str(),content.length())==sdFileContent);}
        else {
            assert(content=="old complete snapshot");assert(sdReadCalls<=3);
            assert(!manager.getStatus().lastError.isEmpty());
            if(mode!="oversize")assert(!manager.getStatus().capacityValid);
            // A later complete read clears the operation error, but cannot
            // promote the invalidated historical capacity into a fresh sample.
            sdFileContent="abc";sdFileDeclaredSize=3;sdFileFinalSize=static_cast<size_t>(-1);
            sdReadFailAt=-1;sdAvailableInfinite=false;
            assert(manager.readFile("test.txt",content,error,16384,"sd") && content=="abc");
            assert(manager.getStatus().lastError.isEmpty());
            if(mode!="oversize")assert(!manager.getStatus().capacityValid);
        }
    }
    assert(manager.unmount("sd",&error,false));
    SD.openOk=true;
    assert(manager.mount(error,"sd") && manager.getStatus().capacityValid);
    assert(manager.unmount("sd",&error));
    assert(FastBee::SpiBusInitializer::instance().end(error));
}
