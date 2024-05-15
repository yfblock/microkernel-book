#include <libs/user/syscall.h>
#include <libs/user/task.h>

//获取正在运行的任务的任务ID。
task_t task_self(void) {
    static task_t tid = 0;
    if (tid) {
        //由于任务ID已经获取，所以返回缓存的值。
        return tid;
    }

    //任务ID一旦获取就不会改变，所以缓存起来。
    tid = sys_task_self();
    return tid;
}
