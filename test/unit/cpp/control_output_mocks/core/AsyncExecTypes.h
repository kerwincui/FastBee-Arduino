#pragma once
#include <mutex>
inline std::recursive_mutex controlMutex;
inline bool controlBusy = false;
inline unsigned controlDepth = 0;
class RecursiveMutexGuard {
public:
    RecursiveMutexGuard(void* mutex, unsigned timeout=0)
        : _locked(mutex && !controlBusy && timeout == 0 && controlMutex.try_lock()) {
        if (_locked) ++controlDepth;
    }
    ~RecursiveMutexGuard() { if (_locked) { --controlDepth; controlMutex.unlock(); } }
    bool isLocked() const { return _locked; }
private:
    bool _locked;
};
