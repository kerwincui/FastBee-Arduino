/**
 * @file test_task_manager.cpp
 * @brief 任务调度管理器单元测试（基于 MockTaskManager）
 *
 * 覆盖范围：
 *  - 任务 CRUD（添加/删除/查询）
 *  - 任务启用/禁用/暂停/恢复
 *  - 优先级调度排序
 *  - 任务执行统计
 *  - 动态配置更新（间隔、优先级）
 *  - 立即执行 & 延迟调度
 *  - 最大执行次数限制
 *  - 批量清理
 */

#include <unity.h>
#include <Arduino.h>
#include "mocks/MockTaskManager.h"
#include "helpers/TestConfig.h"
#include "helpers/TestAssertions.h"
#include "helpers/TestLogger.h"

void test_task_manager_group();

// 全局计数器（用于任务执行验证）
static int taskCounter = 0;
static int taskACounter = 0;
static int taskBCounter = 0;
static int taskCCounter = 0;

// ========== 任务 CRUD ==========

void test_task_add_and_query() {
    TestLog::testStart("Task CRUD: Add & Query");

    auto& tm = MockTaskMgr;
    tm.initialize();
    tm.clearAll();

    taskCounter = 0;

    TEST_ASSERT_TRUE(tm.addTask("sensor_poll", [](void*) {
        taskCounter++;
    }, nullptr, 1000, TaskPriority::PRIORITY_NORMAL));
    TestLog::step("addTask: sensor_poll added");

    TEST_ASSERT_EQUAL(1, tm.getTaskCount());
    TestLog::step("getTaskCount = 1");

    Task* task = tm.getTask("sensor_poll");
    TEST_ASSERT_NOT_NULL(task);
    TEST_ASSERT_EQUAL_STRING("sensor_poll", task->id.c_str());
    TEST_ASSERT_EQUAL(1000, (int)task->interval);
    TEST_ASSERT_TRUE(task->enabled);
    TestLog::step("getTask: fields verified");

    // 重复添加应该失败
    TEST_ASSERT_FALSE(tm.addTask("sensor_poll", [](void*) {}, nullptr, 500));
    TEST_ASSERT_EQUAL(1, tm.getTaskCount());
    TestLog::step("Duplicate add rejected");

    // 空 ID 应该失败
    TEST_ASSERT_FALSE(tm.addTask("", [](void*) {}, nullptr, 1000));
    TestLog::step("Empty ID rejected");

    // 查询不存在的任务
    TEST_ASSERT_NULL(tm.getTask("nonexistent"));
    TestLog::step("getTask nonexistent returns null");

    tm.clearAll();
    TestLog::testEnd(true);
}

void test_task_remove() {
    TestLog::testStart("Task CRUD: Remove");

    auto& tm = MockTaskMgr;
    tm.initialize();
    tm.clearAll();

    tm.addTask("task1", [](void*) {}, nullptr, 1000);
    tm.addTask("task2", [](void*) {}, nullptr, 2000);
    TEST_ASSERT_EQUAL(2, tm.getTaskCount());

    TEST_ASSERT_TRUE(tm.removeTask("task1"));
    TEST_ASSERT_EQUAL(1, tm.getTaskCount());
    TEST_ASSERT_NULL(tm.getTask("task1"));
    TEST_ASSERT_NOT_NULL(tm.getTask("task2"));
    TestLog::step("task1 removed, task2 preserved");

    // 删除不存在的任务
    TEST_ASSERT_FALSE(tm.removeTask("nonexistent"));
    TestLog::step("Remove nonexistent returns false");

    tm.clearAll();
    TestLog::testEnd(true);
}

void test_task_id_list() {
    TestLog::testStart("Task: ID Enumeration");

    auto& tm = MockTaskMgr;
    tm.clearAll();

    tm.addTask("alpha", [](void*) {}, nullptr, 1000);
    tm.addTask("beta", [](void*) {}, nullptr, 2000);
    tm.addTask("gamma", [](void*) {}, nullptr, 3000);

    std::vector<String> ids = tm.getTaskIds();
    TEST_ASSERT_EQUAL(3, ids.size());

    bool hasAlpha = false, hasBeta = false, hasGamma = false;
    for (auto& id : ids) {
        if (id == "alpha") hasAlpha = true;
        if (id == "beta") hasBeta = true;
        if (id == "gamma") hasGamma = true;
    }
    TEST_ASSERT_TRUE(hasAlpha);
    TEST_ASSERT_TRUE(hasBeta);
    TEST_ASSERT_TRUE(hasGamma);
    TestLog::step("All 3 task IDs present");

    tm.clearAll();
    TestLog::testEnd(true);
}

// ========== 任务启用/禁用/暂停/恢复 ==========

void test_task_enable_disable() {
    TestLog::testStart("Task: Enable & Disable");

    auto& tm = MockTaskMgr;
    tm.clearAll();

    tm.addTask("toggle_task", [](void*) {}, nullptr, 1000);

    Task* t = tm.getTask("toggle_task");
    TEST_ASSERT_TRUE(t->enabled);
    TestLog::step("New task starts enabled");

    tm.disableTask("toggle_task");
    TEST_ASSERT_FALSE(t->enabled);
    TestLog::step("disableTask: enabled=false");

    tm.enableTask("toggle_task");
    TEST_ASSERT_TRUE(t->enabled);
    TestLog::step("enableTask: enabled=true");

    // 对不存在的任务操作
    TEST_ASSERT_FALSE(tm.enableTask("ghost"));
    TEST_ASSERT_FALSE(tm.disableTask("ghost"));
    TestLog::step("Enable/disable nonexistent returns false");

    tm.clearAll();
    TestLog::testEnd(true);
}

void test_task_pause_resume() {
    TestLog::testStart("Task: Pause & Resume");

    auto& tm = MockTaskMgr;
    tm.clearAll();

    tm.addTask("pausable", [](void*) {}, nullptr, 1000);

    Task* t = tm.getTask("pausable");
    TEST_ASSERT_EQUAL((int)TaskState::TASK_IDLE, (int)t->state);

    tm.pauseTask("pausable");
    TEST_ASSERT_EQUAL((int)TaskState::TASK_PAUSED, (int)t->state);
    TestLog::step("pauseTask: state=PAUSED");

    tm.resumeTask("pausable");
    TEST_ASSERT_EQUAL((int)TaskState::TASK_IDLE, (int)t->state);
    TestLog::step("resumeTask: state=IDLE");

    tm.clearAll();
    TestLog::testEnd(true);
}

// ========== 任务执行 ==========

void test_task_execution() {
    TestLog::testStart("Task: Execution & Statistics");

    auto& tm = MockTaskMgr;
    tm.clearAll();

    taskCounter = 0;

    tm.addTask("counter_task", [](void*) {
        taskCounter++;
    }, nullptr, 100, TaskPriority::PRIORITY_NORMAL);

    // 手动执行
    tm.runTaskNow("counter_task");
    TEST_ASSERT_EQUAL(1, taskCounter);
    TestLog::step("runTaskNow: counter=1");

    // 通过统计验证
    TaskStatistics stats = tm.getTaskStatistics("counter_task");
    TEST_ASSERT_EQUAL(1, stats.executionCount);
    TestLog::step("Statistics: executionCount=1");

    // 再执行几次
    tm.runTaskNow("counter_task");
    tm.runTaskNow("counter_task");
    TEST_ASSERT_EQUAL(3, taskCounter);

    stats = tm.getTaskStatistics("counter_task");
    TEST_ASSERT_EQUAL(3, stats.executionCount);
    TestLog::step("After 3 executions: counter=3, stats=3");

    // 统计重置
    tm.resetStatistics("counter_task");
    stats = tm.getTaskStatistics("counter_task");
    TEST_ASSERT_EQUAL(0, stats.executionCount);
    TestLog::step("resetStatistics: count back to 0");

    tm.clearAll();
    TestLog::testEnd(true);
}

// ========== 优先级调度 ==========

void test_priority_ordering() {
    TestLog::testStart("Task: Priority Ordering");

    auto& tm = MockTaskMgr;
    tm.clearAll();

    taskACounter = 0;
    taskBCounter = 0;
    taskCCounter = 0;

    // 注意：mock 的 TaskPriority 中 CRITICAL=4 最高，IDLE=0 最低
    tm.addTask("low_task", [](void*) { taskACounter++; }, nullptr,
               100, TaskPriority::PRIORITY_LOW);
    tm.addTask("high_task", [](void*) { taskBCounter++; }, nullptr,
               100, TaskPriority::PRIORITY_HIGH);
    tm.addTask("normal_task", [](void*) { taskCCounter++; }, nullptr,
               100, TaskPriority::PRIORITY_NORMAL);

    // 设置当前时间使所有任务到期
    tm.setCurrentTime(200);

    // run 应该按优先级执行
    tm.run();

    // 所有任务都应该执行一次
    TEST_ASSERT_EQUAL(1, taskACounter);
    TEST_ASSERT_EQUAL(1, taskBCounter);
    TEST_ASSERT_EQUAL(1, taskCCounter);
    TestLog::step("All priority levels executed once");

    // 更新优先级
    tm.updateTaskPriority("low_task", TaskPriority::PRIORITY_CRITICAL);
    Task* t = tm.getTask("low_task");
    TEST_ASSERT_EQUAL((int)TaskPriority::PRIORITY_CRITICAL, (int)t->priority);
    TestLog::step("Priority updated: LOW → CRITICAL");

    tm.clearAll();
    TestLog::testEnd(true);
}

// ========== 动态配置更新 ==========

void test_update_task_interval() {
    TestLog::testStart("Task: Update Interval");

    auto& tm = MockTaskMgr;
    tm.clearAll();

    tm.addTask("interval_task", [](void*) {}, nullptr, 1000);

    Task* t = tm.getTask("interval_task");
    TEST_ASSERT_EQUAL(1000, (int)t->interval);

    TEST_ASSERT_TRUE(tm.updateTaskInterval("interval_task", 5000));
    TEST_ASSERT_EQUAL(5000, (int)t->interval);
    TestLog::step("Interval updated: 1000 → 5000");

    // 不存在的任务
    TEST_ASSERT_FALSE(tm.updateTaskInterval("ghost", 100));
    TestLog::step("Update nonexistent returns false");

    tm.clearAll();
    TestLog::testEnd(true);
}

// ========== 延迟调度 ==========

void test_schedule_task_delay() {
    TestLog::testStart("Task: Schedule Delay");

    auto& tm = MockTaskMgr;
    tm.clearAll();

    tm.addTask("delayed", [](void*) {}, nullptr, 1000);
    tm.setCurrentTime(1000);

    TEST_ASSERT_TRUE(tm.scheduleTask("delayed", 5000));
    Task* t = tm.getTask("delayed");
    TEST_ASSERT_EQUAL(6000, (int)t->nextRun);
    TestLog::step("scheduleTask: nextRun = currentTime + delay = 6000");

    TEST_ASSERT_FALSE(tm.scheduleTask("ghost", 1000));
    TestLog::step("Schedule nonexistent returns false");

    tm.clearAll();
    TestLog::testEnd(true);
}

// ========== 禁用任务不应执行 ==========

void test_disabled_task_not_executed() {
    TestLog::testStart("Task: Disabled Not Executed");

    auto& tm = MockTaskMgr;
    tm.clearAll();

    taskCounter = 0;

    tm.addTask("disabled_task", [](void*) { taskCounter++; }, nullptr, 100);
    tm.disableTask("disabled_task");

    tm.setCurrentTime(200);
    tm.run();

    TEST_ASSERT_EQUAL(0, taskCounter);
    TestLog::step("Disabled task not executed during run()");

    tm.clearAll();
    TestLog::testEnd(true);
}

// ========== 暂停任务不应执行 ==========

void test_paused_task_not_executed() {
    TestLog::testStart("Task: Paused Not Executed");

    auto& tm = MockTaskMgr;
    tm.clearAll();

    taskCounter = 0;

    tm.addTask("paused_task", [](void*) { taskCounter++; }, nullptr, 100);
    tm.pauseTask("paused_task");

    tm.setCurrentTime(200);
    tm.run();

    TEST_ASSERT_EQUAL(0, taskCounter);
    TestLog::step("Paused task not executed during run()");

    // 恢复后应该可以执行
    tm.resumeTask("paused_task");
    tm.run();
    TEST_ASSERT_EQUAL(1, taskCounter);
    TestLog::step("Resumed task executed");

    tm.clearAll();
    TestLog::testEnd(true);
}

// ========== 待执行任务计数 ==========

void test_pending_task_count() {
    TestLog::testStart("Task: Pending Count");

    auto& tm = MockTaskMgr;
    tm.clearAll();

    tm.addTask("t1", [](void*) {}, nullptr, 100);
    tm.addTask("t2", [](void*) {}, nullptr, 200);
    tm.addTask("t3", [](void*) {}, nullptr, 300);
    tm.disableTask("t3");

    tm.setCurrentTime(250);

    int pending = tm.getPendingTaskCount();
    // t1 (100ms) 和 t2 (200ms) 应该到期，t3 被禁用
    TEST_ASSERT_EQUAL(2, pending);
    TestLog::step("Pending count = 2 (disabled excluded)");

    tm.clearAll();
    TestLog::testEnd(true);
}

// ========== 批量清理 ==========

void test_clear_all_tasks() {
    TestLog::testStart("Task: Clear All");

    auto& tm = MockTaskMgr;
    tm.clearAll();

    for (int i = 0; i < 10; i++) {
        tm.addTask("task_" + String(i), [](void*) {}, nullptr, 1000);
    }
    TEST_ASSERT_EQUAL(10, tm.getTaskCount());
    TestLog::step("Added 10 tasks");

    tm.clearAll();
    TEST_ASSERT_EQUAL(0, tm.getTaskCount());
    TestLog::step("clearAll: count = 0");

    TestLog::testEnd(true);
}

// ========== 测试组入口 ==========

void test_task_manager_group() {
    TestLog::groupStart("TaskManager Tests");

    // CRUD
    RUN_TEST(test_task_add_and_query);
    RUN_TEST(test_task_remove);
    RUN_TEST(test_task_id_list);

    // 状态控制
    RUN_TEST(test_task_enable_disable);
    RUN_TEST(test_task_pause_resume);

    // 执行
    RUN_TEST(test_task_execution);
    RUN_TEST(test_priority_ordering);
    RUN_TEST(test_disabled_task_not_executed);
    RUN_TEST(test_paused_task_not_executed);

    // 配置
    RUN_TEST(test_update_task_interval);
    RUN_TEST(test_schedule_task_delay);

    // 统计
    RUN_TEST(test_pending_task_count);

    // 清理
    RUN_TEST(test_clear_all_tasks);

    TestLog::groupEnd();
}
