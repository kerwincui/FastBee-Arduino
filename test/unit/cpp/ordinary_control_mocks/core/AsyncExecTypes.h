#pragma once
// Explicit FreeRTOS lock seam for the production arbitration translation unit.
// Keep this separate from the PM mock so a missing production include fails
// compilation instead of being accidentally supplied by the fixture manager.
extern bool ordinaryMutexBusy;
extern unsigned ordinaryLockDepth;
struct RecursiveMutexGuard {
    bool locked;
    explicit RecursiveMutexGuard(int, int = -1) : locked(!ordinaryMutexBusy) {
        if (locked) ++ordinaryLockDepth;
    }
    ~RecursiveMutexGuard() { if (locked) --ordinaryLockDepth; }
    bool isLocked() const { return locked; }
};
