#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <new>
#include <type_traits>
#include <map>
#include <cstdio>
#include <cstring>

static bool ready=true, externalFail=false, internalFail=false, nodeFail=false;
static size_t externalCalls=0,internalCalls=0,capFrees=0,nodeAllocations=0,nodeFrees=0;
static size_t alignmentSeen=0,bytesSeen=0;
static unsigned checks=0;
static void check(bool value) {assert(value);++checks;}
static void* slab_malloc(size_t bytes) {++nodeAllocations;return nodeFail?nullptr:std::malloc(bytes);}
static void slab_free(void* ptr) {++nodeFrees;std::free(ptr);}
#define malloc slab_malloc
#define free slab_free
#include "utils/StaticPoolAllocator.h"
#undef malloc
#undef free

size_t heap_caps_get_total_size(uint32_t) {return ready?1024*1024:0;}
void* heap_caps_aligned_alloc(size_t alignment,size_t bytes,uint32_t caps) {
    alignmentSeen=alignment;bytesSeen=bytes;
    if(caps & 2U) {++externalCalls;if(externalFail)return nullptr;}
    else {assert(caps==(4U|1U));++internalCalls;if(internalFail)return nullptr;}
    void* ptr=std::malloc(bytes);
    assert(!ptr || reinterpret_cast<uintptr_t>(ptr)%alignment==0);
    return ptr;
}
void heap_caps_free(void* ptr) {++capFrees;std::free(ptr);}

// Declared before the global map so this runs after its destructor. A shared
// Meyers pool first constructed inside GlobalMap is destroyed before the map.
struct FinalVerifier {~FinalVerifier(){
    assert(capFrees==0);
    std::printf("GLOBAL_DESTRUCTOR_SAFE checks=%u capFrees=%zu\n",checks,capFrees);
}} finalVerifier;
struct GlobalMap {
    using A=FastBee::PooledAllocator<std::pair<const int,int>,80,8>;
    std::map<int,int,std::less<int>,A> values;
    GlobalMap(){values.emplace(7,19);}
    ~GlobalMap(){assert(values.at(7)==19);values.clear();}
} globalMap;

int main(){
#if defined(TEST_DYNAMIC)
    check(externalCalls==1 && internalCalls==0);
    size_t previousExternal=externalCalls;
    FastBee::StaticSlabPool<64,2> pool;
    check(externalCalls==previousExternal+1 && pool.externalStorage() && pool.storageAvailable());
    check(pool.capacity()==2 && pool.blockSize()==64 && pool.totalBytes()==128 && pool.allocatedStorageBytes()==128);
    check(alignmentSeen==alignof(std::max_align_t) && bytesSeen==128);
    void* a=pool.allocate();void* b=pool.allocate();
    check(a && b && a!=b && reinterpret_cast<uintptr_t>(a)%alignof(std::max_align_t)==0);
    check(pool.usedCount()==2 && pool.peakUsed()==2 && pool.overflowCount()==0);
    std::memset(a,0x33,64);std::memset(b,0x55,64);
    size_t invalidFree=nodeFrees;
    pool.deallocate(static_cast<unsigned char*>(a)+1);
    pool.deallocate(static_cast<unsigned char*>(b)+63);
    check(pool.usedCount()==2 && nodeFrees==invalidFree);

    size_t previousNode=nodeAllocations;
    void* overflow=pool.allocate();check(overflow && nodeAllocations==previousNode+1 && pool.overflowCount()==1);
    size_t previousFree=nodeFrees;pool.deallocate(overflow);check(nodeFrees==previousFree+1 && pool.usedCount()==2);
    pool.deallocate(a);check(pool.usedCount()==1 && nodeFrees==previousFree+1);
    check(pool.allocate()==a && pool.usedCount()==2);
    check(pool.peakUsed()==2);
    pool.deallocate(a);pool.deallocate(b);pool.deallocate(nullptr);check(pool.usedCount()==0);
    void* outside=std::malloc(9);previousFree=nodeFrees;pool.deallocate(outside);check(nodeFrees==previousFree+1);
    externalFail=true;
    size_t previousInternal=internalCalls;
    FastBee::StaticSlabPool<48,3> fallback;
    check(fallback.storageAvailable() && !fallback.externalStorage() && internalCalls==previousInternal+1);
    check(fallback.allocatedStorageBytes()==144 && fallback.capacity()==3);
    void* internal=fallback.allocate();check(internal!=nullptr);fallback.deallocate(internal);
    ready=false;previousExternal=externalCalls;previousInternal=internalCalls;
    FastBee::StaticSlabPool<32,4> noPsram;
    check(!noPsram.externalStorage() && noPsram.storageAvailable() && externalCalls==previousExternal && internalCalls==previousInternal+1);
    ready=true;internalFail=true;
    FastBee::StaticSlabPool<96,5> unavailable;
    check(!unavailable.storageAvailable() && !unavailable.externalStorage() && unavailable.allocatedStorageBytes()==0 && unavailable.capacity()==5);
    void* single=unavailable.allocate();check(single && unavailable.usedCount()==0 && unavailable.overflowCount()==1);
    previousFree=nodeFrees;unavailable.deallocate(single);check(nodeFrees==previousFree+1);
    nodeFail=true;check(unavailable.allocate()==nullptr && unavailable.overflowCount()==2);
    using Failed=FastBee::PooledAllocator<int,112,6>;
    bool threw=false;try{Failed allocator;allocator.allocate(1);}catch(const std::bad_alloc&){threw=true;}
    check(threw && !Failed::pool().storageAvailable());
    externalFail=internalFail=nodeFail=false;
    using X=FastBee::PooledAllocator<int,128,7>;
    using Y=FastBee::PooledAllocator<long,128,7>;
    check(&X::pool()==&Y::pool());
    X x;int* many=x.allocate(4);check(many);previousFree=nodeFrees;x.deallocate(many,4);check(nodeFrees==previousFree+1);
    {FastBee::StaticSlabPool<64,9> lifetime;void* node=lifetime.allocate();lifetime.deallocate(node);}
    check(capFrees==0);
#else
    check(externalCalls==0 && internalCalls==0);
    FastBee::StaticSlabPool<64,2> pool;
    check(!pool.externalStorage() && pool.storageAvailable() && pool.allocatedStorageBytes()==128);
    check(sizeof(pool)>=128 && sizeof(pool)<200);
    void* a=pool.allocate();void* b=pool.allocate();check(a && b && a!=b);
    pool.deallocate(a);check(pool.allocate()==a);pool.deallocate(a);pool.deallocate(b);
    check(externalCalls==0 && internalCalls==0 && capFrees==0);
#endif
    std::printf("TEMPLATE_BEHAVIOR_PASS checks=%u dynamic=%d\n",checks,
#if defined(TEST_DYNAMIC)
        1
#else
        0
#endif
    );
}
