// test_aim_target_mailbox.cpp — AimTargetMailbox 基础行为测试
//
// ★ P0-2 / item 9 口径统一：本文件已由"裸 main() + assert"并入 ttbox_test 框架。
//   理由（team-lead item 9 复查确认为**测试缺陷**、非实现缺陷）：
//     ① 顺序 bug：旧用例在 clear() **之前** offer 新周期任务，而 clear() 清空**全部**槽位
//        ⇒ 随后 take_latest(&got,0) 必然失败（新任务已被一并清掉）。最小修法=重排顺序。
//     ② 裸 main()+assert 的观测缺陷：
//        · 失败即 SIGABRT —— CTest 只见"异常退出"，无 passed/skipped/failed 摘要行，
//          无法与"断言失败"区分、无法计 skip（框架外无 report_skip）→ 破坏"绿=真绿"。
//        · assert 受 NDEBUG 控制：Release 构建下 `assert` 全部被编译掉 ⇒ 整个测试变**空操作**、
//          恒退出 0（"零断言静默 PASS"的另一种形态）。CHECK 恒生效，杜绝此坑。
//   改用 TEST/CHECK 后：断言失败被**计数**（不中断进程）、退出码由 run_all() 统一汇总，
//   与其余用例同口径。
//   注：本 TU 是**单文件可执行**（CMake 用 add_executable(test_aim_target_mailbox
//       仅编本文件，未与 tests/test_main.cpp 同编），故底部 main() 直接调用 run_all()
//       （等价 test_main.cpp 三行惯用法），**无需改动 core/CMakeLists.txt**，
//       CTest 项 test_aim_target_mailbox 计数保持不变。
#include "test_util.hpp"
#include "pipeline/AimTargetMailbox.hpp"

using namespace ttbox::core::aim;

// 基础：多 worker 槽位择"最大帧号"、去重水位(last_frame)、worker_id 越界拒绝。
TEST(aim_target_mailbox_picks_latest_frame) {
    AimTargetMailbox mailbox(3);
    AimTargetTask old_task; old_task.frame_number = 10; old_task.worker_id = 0;
    AimTargetTask new_task; new_task.frame_number = 12; new_task.worker_id = 1;
    CHECK(mailbox.offer(0, old_task));
    CHECK(mailbox.offer(1, new_task));

    AimTargetTask out;
    CHECK(mailbox.take_latest(&out, 0));
    CHECK_EQ(out.frame_number, static_cast<uint64_t>(12));  // 取到最大帧号
    CHECK_EQ(out.worker_id, 1);

    // 以 last_frame=12 再取 → 无更新任务（去重水位生效）
    CHECK(!mailbox.take_latest(&out, 12));
    // worker_id=3 越界（worker_count_=3）→ offer 必须拒绝
    CHECK(!mailbox.offer(3, old_task));
}

// 重启残留场景：旧周期帧号大 → 新周期从 0 重新计数，take_latest 被旧任务卡死；
// clear() 后新任务立即可取（修复 1~3 分钟检测框不更新）。
// ★ 顺序硬约束（item 9）：clear() 清空**全部**槽位 ⇒ 新周期任务必须在 clear() **之后**入队；
//   否则新任务一并被清、take_latest(&got,0) 必失败（即原用例的 bug）。
TEST(aim_target_mailbox_clear_releases_restart_residue) {
    AimTargetMailbox mailbox(3);

    // 1) 旧周期残留任务入队（frame 巨大）
    AimTargetTask stale; stale.frame_number = 100000; stale.worker_id = 2;
    CHECK(mailbox.offer(2, stale));

    AimTargetTask got;
    CHECK(mailbox.take_latest(&got, 0));
    CHECK_EQ(got.frame_number, static_cast<uint64_t>(100000));  // 残留任务把 last_frame 抬高
    CHECK_EQ(got.worker_id, 2);

    // 2) 清空全部槽位（模拟 CoreRuntime 重启）
    mailbox.clear();

    // 3) 新周期任务（frame 从 0/1 重新计数）必须在 clear() **之后**入队
    AimTargetTask fresh; fresh.frame_number = 1; fresh.worker_id = 0;
    CHECK(mailbox.offer(0, fresh));

    CHECK(mailbox.take_latest(&got, 0));
    CHECK_EQ(got.frame_number, static_cast<uint64_t>(1));       // 修复后：新周期任务立即可取
    CHECK_EQ(got.worker_id, 0);

    // clear() 已抹除旧残留：以 last_frame=1 再取 → 无更新（反证 frame=100000 已不在）
    CHECK(!mailbox.take_latest(&got, 1));
}

int main() {
    std::printf("=== ttbox_core tests (aim_target_mailbox) ===\n");
    const int failed = ::ttbox_test::run_all();
    std::printf("=== tests done (exit=%d) ===\n", failed == 0 ? 0 : 1);
    return failed == 0 ? 0 : 1;
}
