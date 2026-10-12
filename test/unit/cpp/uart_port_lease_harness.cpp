#include <cassert>
#include <atomic>
#include <thread>
#include <vector>
#include <new>
#include <string>
#include "core/UartPortLease.h"
using FastBee::UartPortLease;
bool otherTranslationUnitAvailable(uint8_t port);
int main() {
    UartPortLease a,b,c;
    for (uint8_t port : {0,3,255}) { assert(!a.acquire(port)); assert(!UartPortLease::available(port)); }
    using Role=FastBee::ResourceConsumerRole;
    assert(!a.acquire(1,Role::Peripheral,"01234567890123456789012345678901234567890"));
    assert(a.acquire(1,Role::Peripheral,"console-device") && a.owned() && a.acquire(1));
    assert(!a.acquire(1,Role::Peripheral,"other-device"));
    assert(!a.acquire(2) && !b.acquire(1) && !b.owned());
    assert(!otherTranslationUnitAvailable(1)); // Registry is shared across translation units.
    assert(b.acquire(2) && !c.acquire(2));
    FastBee::UartResourceSnapshot snapshot;
    UartPortLease::snapshotResources(snapshot, 3);
    assert(snapshot.portCount == 3 && snapshot.generations[1] && snapshot.generations[2]);
    assert(snapshot.generations[1] != snapshot.generations[2] && snapshot.generations[0] == 0);
    assert(std::string(snapshot.consumers[1].id)=="console-device" && snapshot.consumers[1].role==Role::Peripheral);
    UartPortLease::snapshotResources(snapshot, 2);
    assert(snapshot.portCount == 2 && snapshot.generations[1] && snapshot.generations[2] == 0);
    assert(!c.releaseAfterStop() && b.owned());
    assert(a.releaseAfterStop() && !a.releaseAfterStop());
    assert(otherTranslationUnitAvailable(1) && c.acquire(1));
    assert(!a.releaseAfterStop() && c.owned());
    assert(b.releaseAfterStop() && c.releaseAfterStop());
    std::atomic<int> live{0}, completed{0};
    std::vector<std::thread> workers;
    std::atomic<bool> observe{true};
    std::thread reader([&] {
        while(observe.load()) {
            FastBee::UartResourceSnapshot view;UartPortLease::snapshotResources(view,3);
            if(view.consumers[2].id[0]) {
                assert(view.generations[2] && view.consumers[2].role==Role::Peripheral);
                const char letter=view.consumers[2].id[0];assert(letter>='A' && letter<='H');
                for(unsigned i=0;i<40;++i)assert(view.consumers[2].id[i]==letter);
                assert(view.consumers[2].id[40]=='\0');
            }
        }
    });
    for(int thread=0;thread<8;++thread) workers.emplace_back([&,thread] {
        UartPortLease lease;
        std::string identity(40,static_cast<char>('A'+thread));
        for(int iteration=0;iteration<1000;++iteration) {
            while(!lease.acquire(2,Role::Peripheral,identity.c_str())) std::this_thread::yield();
            assert(live.fetch_add(1)==0 && lease.owned());
            FastBee::UartResourceSnapshot own;UartPortLease::snapshotResources(own,3);
            assert(std::string(own.consumers[2].id)==identity);
            assert(live.fetch_sub(1)==1);
            assert(lease.releaseAfterStop());
            ++completed;
        }
    });
    for(auto& worker:workers) worker.join();
    observe=false;reader.join();
    assert(completed==8000 && live==0 && UartPortLease::available(2));
    UartPortLease::snapshotResources(snapshot, 255);
    assert(snapshot.portCount == 3 && snapshot.generations[1] == 0 && snapshot.generations[2] == 0);
    // A destroyed busy owner must NOT publish its still-running UART as free.
    alignas(UartPortLease) unsigned char storage[sizeof(UartPortLease)];
    auto* retained=new(storage) UartPortLease; assert(retained->acquire(1,Role::Cellular));
    UartPortLease::snapshotResources(snapshot, 3);
    const uint32_t retainedGeneration = snapshot.generations[1];
    retained->~UartPortLease();
    auto* replacement=new(storage) UartPortLease;
    assert(!replacement->acquire(1) && !replacement->releaseAfterStop());
    assert(!UartPortLease::available(1));
    for (unsigned i = 0; i < 1000; ++i) UartPortLease::snapshotResources(snapshot, 3);
    assert(snapshot.generations[1] == retainedGeneration && retainedGeneration != 0);
    assert(snapshot.consumers[1].role==Role::Cellular && !snapshot.consumers[1].id[0]);
    replacement->~UartPortLease();
}
