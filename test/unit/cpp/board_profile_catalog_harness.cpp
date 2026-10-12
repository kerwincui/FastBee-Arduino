#include "network/BoardProfileCatalog.h"
#include <cassert>
#include <cstdlib>
#include <iostream>
#include <new>
#include <string>
#include <cstring>

static size_t allocations = 0;
void* operator new(size_t size) { ++allocations; if (void* p = std::malloc(size)) return p; throw std::bad_alloc(); }
void* operator new[](size_t size) { return ::operator new(size); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, size_t) noexcept { std::free(p); }
void operator delete[](void* p, size_t) noexcept { std::free(p); }

int main(int argc, char** argv) {
    using namespace FastBee;
    static_assert(sizeof(BoardProfileCatalog) <= 1792, "Catalog must fit a small fixed buffer");
    const BoardProfile* profiles[] = {&getPuzhongPzEsp32Profile(), &getEsp32C6DevKitC1Profile(),
        &getEsp32C3Profile(), &getEsp32S3Profile(true), &getEsp32S3Profile(false),
        &getEsp32ModuleProfile(false), &getEsp32ModuleProfile(true)};
    const int choice = argc > 1 ? std::atoi(argv[1]) : 2;
    const char* chip = argc > 2 ? argv[2] : "ESP32-C3";
    const size_t before = allocations;
    BoardProfileCatalog catalog(choice >= 0 && choice < 7 ? profiles[choice] : nullptr, chip);
    const size_t length = catalog.measureLength();
    assert(allocations == before);
    if (std::strlen(chip) > 1536) {
        assert(!catalog.valid() && length == 0);
        uint8_t ignored = 0;
        assert(catalog.read(&ignored, 1) == 0);
        std::cout << "overflow rejected";
        return 0;
    }
    assert(catalog.valid() && length > 10000);
    std::string expected;
    std::string actual;
    expected.reserve(length);
    actual.reserve(length);
    uint8_t buffer[1460];
    const size_t chunks[] = {1, 7, 64, 137, 1460};
    for (size_t chunk : chunks) {
        const size_t noAllocations = allocations;
        assert(catalog.measureLength() == length);
        assert(catalog.read(buffer, 0) == 0);
        assert(catalog.read(nullptr, chunk) == 0);
        actual.clear();
        size_t count;
        while ((count = catalog.read(buffer, chunk)) != 0) actual.append(reinterpret_cast<char*>(buffer), count);
        assert(actual.size() == length);
        assert(catalog.read(buffer, chunk) == 0);
        assert(allocations == noAllocations);
        if (expected.empty()) expected = actual;
        else assert(actual == expected);
    }
    // Repeated requests, early disconnects and restarts never allocate or grow.
    for (size_t i = 0; i < 1000; ++i) {
        const size_t noAllocations = allocations;
        catalog.rewind();
        assert(catalog.read(buffer, 137) == 137);
        assert(catalog.measureLength() == length);
        assert(allocations == noAllocations);
    }
    std::cout << expected;
}
