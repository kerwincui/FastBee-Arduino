#include "logger_recent_base.inc"

int main() {
    static_assert(sizeof(LoggerSystem::_criticalLogs) == 1024);
    static_assert(LoggerSystem::recentLogOptionalBytes() == 4098);
    static_assert(LoggerSystem::fileLoggingBudgetBytes() >= 2048 + 512);
    {
        LoggerSystem logger;
        logger.initialize();
        assert(!logger._recentLogBuffer && !logger.isLogViewerRequested());
        assert(logger.recentLogAvailableCapacity() == 8);
        const auto emptyAllocations = allocationCount;
        logger.log(LOG_INFO, "ordinary-disabled", "Test");
        logger.log(LOG_ERROR, "critical-survives", "Test");
        char snapshot[8192];
        logger.snapshotRecentLogs(snapshot, sizeof(snapshot));
        assert(std::strstr(snapshot, "critical-survives"));
        assert(!std::strstr(snapshot, "ordinary-disabled"));
        assert(allocationCount == emptyAllocations);

        for(int cycle=0; cycle<50; ++cycle) {
            logger.enableLogViewer(true);
            auto* allocation = logger._recentLogBuffer.get();
            assert(allocation && logger.isLogViewerEnabled());
            assert(logger.recentLogAvailableCapacity() == 32);
            logger.enableLogViewer(true);
            assert(logger._recentLogBuffer.get() == allocation);
            for(int line=0; line<40; ++line) {
                char entry[32]; std::snprintf(entry, sizeof(entry), "ordinary-%02d", line);
                logger.captureRecentLine(entry, false);
            }
            assert(logger.recentLogLineCount() == 32);
            logger.snapshotRecentLogs(snapshot, sizeof(snapshot));
            assert(!std::strstr(snapshot, "ordinary-07\n"));
            assert(std::strstr(snapshot, "ordinary-08\n") && std::strstr(snapshot, "ordinary-39\n"));
            logger.enableLogViewer(false);
            assert(!logger._recentLogBuffer && !retainedAllocation(allocation));
            logger.snapshotRecentLogs(snapshot, sizeof(snapshot));
            assert(std::strstr(snapshot, "critical-survives"));
            assert(!std::strstr(snapshot, "ordinary-"));
        }

        failAllocation=true;
        logger.enableLogViewer(true);
        assert(logger.isLogViewerRequested() && !logger.isLogViewerEnabled());
        assert(!logger._recentLogBuffer);
        logger.snapshotRecentLogs(snapshot, sizeof(snapshot));
        assert(std::strstr(snapshot, "critical-survives"));
        failAllocation=false;
        logger.enableLogViewer(true);
        assert(logger.isLogViewerEnabled());
        logger.enableLogViewer(false);

        duringAllocation=[&logger]{std::thread cancel([&]{logger.enableLogViewer(false);});cancel.join();};
        logger.enableLogViewer(true);
        assert(!logger.isLogViewerRequested() && !logger._recentLogBuffer);
        duringAllocation=[&logger]{std::thread replace([&]{logger.enableLogViewer(false);logger.enableLogViewer(true);});replace.join();};
        logger.enableLogViewer(true);
        assert(logger.isLogViewerRequested() && !logger.isLogViewerEnabled() && !logger._recentLogBuffer);
        logger.enableLogViewer(true);
        assert(logger.isLogViewerEnabled());

        logger.captureRecentLine("in-flight-reader", false);
        auto* readingBuffer=logger._recentLogBuffer.get();
        const auto releaseBefore=releaseCount;
        duringCopy=[&] {
            // The production snapshot already holds the shared timed mutex and
            // a pointer into the optional ring when cancellation runs elsewhere.
            std::thread disable([&]{logger.enableLogViewer(false);});
            disable.join();
            assert(!logger.isLogViewerRequested());
            assert(logger._recentLogBuffer.get() == readingBuffer);
            assert(retainedAllocation(readingBuffer) && releaseCount == releaseBefore);
        };
        logger.snapshotRecentLogs(snapshot, sizeof(snapshot));
        assert(std::strstr(snapshot, "in-flight-reader"));
        assert(!logger._recentLogBuffer && !retainedAllocation(readingBuffer));
        assert(releaseCount == releaseBefore + 1);

        logger.enableFileLogging(true);
        auto* fileRing=logger._ringBuffer.get();
        logger.enableLogViewer(true);
        assert(logger._recentLogBuffer && logger._ringBuffer.get() == fileRing);
        logger.enableLogViewer(false);
        assert(!logger._recentLogBuffer && logger.isFileLoggingEnabled());
        assert(logger._ringBuffer.get() == fileRing && retainedAllocation(fileRing));
        logger.enableFileLogging(false);
        assert(!logger._ringBuffer);

        for(int line=0; line<10; ++line) {
            char entry[32];std::snprintf(entry, sizeof(entry), "critical-%02d",line);
            logger.captureRecentLine(entry);
        }
        assert(logger.recentLogLineCount() == 8);
        logger.snapshotRecentLogs(snapshot, sizeof(snapshot));
        assert(!std::strstr(snapshot, "critical-01\n"));
        assert(std::strstr(snapshot, "critical-02\n") && std::strstr(snapshot, "critical-09\n"));
        char tiny[5];assert(logger.snapshotRecentLogs(tiny,sizeof(tiny))==0 && tiny[0]=='\0');
        mutex.reject=true;
        assert(logger.snapshotRecentLogs(snapshot,sizeof(snapshot))==0 && snapshot[0]=='\0');
        mutex.reject=false;
    }
    {
        LoggerSystem logger;
        const auto before=allocationCount;
        logger.enableLogViewer(true);
        assert(logger.isLogViewerRequested() && !logger._recentLogBuffer && allocationCount==before);
        logger.initialize();
        assert(logger.isLogViewerEnabled() && logger._recentLogBuffer);
    }
    {
        LoggerSystem logger;
        logger.enableLogViewer(true);logger.enableLogViewer(false);logger.initialize();
        assert(!logger.isLogViewerRequested() && !logger._recentLogBuffer);
    }
    {
        failMutex=true;
        LoggerSystem logger;
        logger.enableLogViewer(true);logger.initialize();
        assert(logger.isLogViewerRequested() && !logger.isLogViewerEnabled() && !logger._recentLogBuffer);
        char snapshot[16];assert(logger.snapshotRecentLogs(snapshot,sizeof(snapshot))==0 && snapshot[0]=='\0');
        failMutex=false;
    }
    assert(allocationCount == releaseCount);
}
