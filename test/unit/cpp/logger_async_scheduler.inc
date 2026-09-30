static void (*pendingTask)(void*) = nullptr;
static void* pendingContext = nullptr;
static bool failTask = false;
static size_t taskCreates = 0, taskDeletes = 0;
static constexpr int pdPASS = 1;
int xTaskCreate(void (*entry)(void*), const char* name, uint32_t stack,
                void* context, int priority, void*) {
    assert(std::strcmp(name, "log_flush") == 0 && stack == 4096 && priority == 0);
    ++taskCreates;
    if (failTask) return 0;
    assert(!pendingTask);
    pendingTask = entry;
    pendingContext = context;
    return pdPASS;
}
void vTaskDelete(void*) { ++taskDeletes; }
unsigned long uxTaskGetStackHighWaterMark(void*) { return 1024; }
void runPendingTask() {
    assert(pendingTask);
    auto task = pendingTask;
    auto context = pendingContext;
    pendingTask = nullptr;
    pendingContext = nullptr;
    task(context);
}
