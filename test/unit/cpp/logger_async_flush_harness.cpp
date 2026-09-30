#include "logger_async_base.inc"

void enqueue(LoggerSystem& logger, const char* value = "async batch\n") {
    assert(logger.lockLogFiles());
    logger.logToBuffer(value, std::strlen(value));
    logger.unlockLogFiles();
    clockMs += LoggerSystem::LOG_FLUSH_INTERVAL_MS;
}

int main() {
    FastBee::MemoryBudget::freeBytes = 32000;
    {
        LoggerSystem logger;
        logger.initialize();
        logger.enableFileLogging(true);
        enqueue(logger);
        const auto before = opens;
        logger.flushBufferIfDue();
        assert(pendingTask && opens == before && written.empty());
        const auto created = taskCreates;
        for (int i = 0; i < 20; ++i) logger.flushBufferIfDue();
        assert(taskCreates == created && opens == before);
        duringWrite = [&]() {
            // A slow LittleFS call owns the mutex in the worker. The scheduler
            // callback still returns without waiting or queuing another task.
            clockMs += 4300;
            logger.flushBufferIfDue();
            assert(taskCreates == created && mutex.held);
        };
        runPendingTask();
        assert(written == "async batch\n" && !logger._flushTaskState.load());
        logger.flushBufferIfDue();
        assert(!pendingTask); // No idle task or empty periodic filesystem IO.

        enqueue(logger, "admission\n");
        FastBee::MemoryBudget::freeBytes = 20000;
        logger.flushBufferIfDue();
        assert(!pendingTask && taskCreates == created);
        FastBee::MemoryBudget::freeBytes = 32000;
        FastBee::MemoryBudget::largestBytes = 4096;
        logger.flushBufferIfDue();
        assert(!pendingTask && taskCreates == created);
        FastBee::MemoryBudget::largestBytes = 32000;
        failTask = true;
        const auto after = opens;
        logger.flushBufferIfDue();
        assert(!pendingTask && !logger._flushTaskState.load() && opens == after);
        assert(logger.bufferedDataSize() == std::strlen("admission\n"));
        failTask = false;
        logger.flushBufferIfDue();
        logger.flushBuffer(); // Explicit flush joins queued work before returning.
        assert(!pendingTask && logger.bufferedDataSize() == 0);
        assert(written == "async batch\nadmission\n");

        enqueue(logger, "old session\n");
        logger.flushBufferIfDue();
        logger.enableFileLogging(false);
        assert(!logger._ringBuffer);
        logger.enableFileLogging(true);
        enqueue(logger, "new session\n");
        const auto beforeCancel = opens;
        runPendingTask();
        assert(opens == beforeCancel && logger.bufferedDataSize() != 0);
        logger.flushBufferIfDue();
        runPendingTask();
        assert(written == "async batch\nadmission\nnew session\n");

        enqueue(logger, "lock retry\n");
        logger.flushBufferIfDue();
        mutex.reject = true;
        runPendingTask();
        mutex.reject = false;
        assert(!logger._flushTaskState.load() && logger.bufferedDataSize() != 0);
        logger.flushBufferIfDue();
        runPendingTask();
        assert(logger.bufferedDataSize() == 0);

        enqueue(logger, "disable during write\n");
        logger.flushBufferIfDue();
        duringWrite = [&]() {
            logger.enableFileLogging(false);
            assert(logger._ringBuffer && !logger.isFileLoggingEnabled());
        };
        runPendingTask();
        assert(!logger._ringBuffer && !logger._flushTaskState.load());
        logger.enableFileLogging(true);

        enqueue(logger, "file failure\n");
        logger.flushBufferIfDue();
        openSucceeds = false;
        runPendingTask();
        openSucceeds = true;
        assert(!logger.isFileLoggingEnabled() && !logger._ringBuffer);
        assert(!logger._flushTaskState.load());
        logger.enableFileLogging(true);
        enqueue(logger, "shutdown\n");
        logger.flushBufferIfDue();
        assert(pendingTask);
    } // Destructor joins the queued task before deleting its mutex/ring.
    assert(!pendingTask && !mutex.held && !ringAllocation);
    assert(written.find("shutdown\n") != String::npos);
    assert(taskCreates == taskDeletes + 1); // Only the simulated create failure.
    return 0;
}
