#include "hinavm.h"
#include "ipc.h"
#include "memory.h"
#include "task.h"
#include <libs/common/print.h>
#include <libs/common/string.h>

//将有符号 20 位整数扩展为 32 位的宏
#define SIGN_EXTEND20(x) ((int32_t) ((x & (1 << 19)) ? (x | 0xfff00000) : x))

//字节码解释器“HinaVM”的实现。 HinaVM任务创建后，将处于用户模式。
//而是调用此函数。
__noreturn void hinavm_run(struct hinavm *hinavm) {
    struct task *current = CURRENT_TASK;

    //获取消息缓冲区的地址
    vaddr_t m = (vaddr_t) &current->m;

    //获取指令序列
    hinavm_inst_t *insts = (hinavm_inst_t *) &hinavm->insts;
    uint32_t num_insts = hinavm->num_insts;

    //初始化Hina虚拟机状态
    uint32_t pc = 0;
    uint32_t regs[16];
    uint32_t labels[16];

    //初始化寄存器
    memset(regs, 0, sizeof(regs));

    //将标签地址初始化为无效值
    memset(labels, 0xff, sizeof(labels));

    //解析标签地址
    for (uint32_t i = 0; i < num_insts; i++) {
        union hinavm_inst *inst = &insts[i];
        if (inst->opcode == HINAVM_LABEL) {
            uint32_t l = inst->labeldef.l;
            labels[l] = i + 1;//指向标签的下一条指令
        }
    }

    //主循环
    for (;;) {
        if (pc >= num_insts) {
            WARN("%s: invalid pc: %u (max=%u)", current->name, pc, num_insts);
            task_exit(EXP_ILLEGAL_EXCEPTION);
        }

        union hinavm_inst inst = insts[pc];//当前指令
        uint32_t next_pc = pc + 1;//下一条要执行的指令地址
        switch (inst.opcode) {
            case HINAVM_MOVI: {
                regs[inst.movi.a] = SIGN_EXTEND20(inst.movi.imm);//符号扩展
                break;
            }
            case HINAVM_MOV: {
                int32_t val = regs[inst.mov.b];
                regs[inst.mov.a] = val;
                break;
            }
            case HINAVM_ADD: {
                int32_t lhs = regs[inst.binop.b];
                int32_t rhs = regs[inst.binop.c];
                regs[inst.binop.a] = lhs + rhs;
                break;
            }
            case HINAVM_SUB: {
                int32_t lhs = regs[inst.binop.b];
                int32_t rhs = regs[inst.binop.c];
                regs[inst.binop.a] = lhs - rhs;
                break;
            }
            case HINAVM_MUL: {
                int32_t lhs = regs[inst.binop.b];
                int32_t rhs = regs[inst.binop.c];
                regs[inst.binop.a] = lhs * rhs;
                break;
            }
            case HINAVM_DIV: {
                int32_t lhs = regs[inst.binop.b];
                int32_t rhs = regs[inst.binop.c];
                regs[inst.binop.a] = lhs / rhs;
                break;
            }
            case HINAVM_MOD: {
                int32_t lhs = regs[inst.binop.b];
                int32_t rhs = regs[inst.binop.c];
                regs[inst.binop.a] = lhs % rhs;
                break;
            }
            case HINAVM_SHR: {
                uint32_t lhs = regs[inst.binop.b];
                uint32_t rhs = regs[inst.binop.c];
                regs[inst.binop.a] = lhs >> rhs;
                break;
            }
            case HINAVM_SHL: {
                uint32_t lhs = regs[inst.binop.b];
                uint32_t rhs = regs[inst.binop.c];
                regs[inst.binop.a] = lhs << rhs;
                break;
            }
            case HINAVM_AND: {
                uint32_t lhs = regs[inst.binop.b];
                uint32_t rhs = regs[inst.binop.c];
                regs[inst.binop.a] = lhs & rhs;
                break;
            }
            case HINAVM_OR: {
                uint32_t lhs = regs[inst.binop.b];
                uint32_t rhs = regs[inst.binop.c];
                regs[inst.binop.a] = lhs | rhs;
                break;
            }
            case HINAVM_XOR: {
                uint32_t lhs = regs[inst.binop.b];
                uint32_t rhs = regs[inst.binop.c];
                regs[inst.binop.a] = lhs ^ rhs;
                break;
            }
            case HINAVM_EQ: {
                int32_t lhs = regs[inst.binop.b];
                int32_t rhs = regs[inst.binop.c];
                regs[inst.binop.a] = lhs == rhs;
                break;
            }
            case HINAVM_NE: {
                int32_t lhs = regs[inst.binop.b];
                int32_t rhs = regs[inst.binop.c];
                regs[inst.binop.a] = lhs != rhs;
                break;
            }
            case HINAVM_LT: {
                int32_t lhs = regs[inst.binop.b];
                int32_t rhs = regs[inst.binop.c];
                regs[inst.binop.a] = lhs < rhs;
                break;
            }
            case HINAVM_LE: {
                int32_t lhs = regs[inst.binop.b];
                int32_t rhs = regs[inst.binop.c];
                regs[inst.binop.a] = lhs <= rhs;
                break;
            }
            case HINAVM_LABEL: {
                //没做什么
                break;
            }
            case HINAVM_JMP: {
                next_pc = labels[inst.jmp.l];
                break;
            }
            case HINAVM_JMP_IF: {
                int32_t cond = regs[inst.jmp.a];
                if (cond) {
                    next_pc = labels[inst.jmp.l];
                }
                break;
            }
            case HINAVM_LDM8: {
                int32_t offset = SIGN_EXTEND20(inst.msg.imm);//符号扩展
                uint8_t val = *(uint8_t *) (m + offset);
                regs[inst.msg.a] = val;
                break;
            }
            case HINAVM_LDM16: {
                int32_t offset = SIGN_EXTEND20(inst.msg.imm);//符号扩展
                uint16_t val = *(uint16_t *) (m + offset);
                regs[inst.msg.a] = val;
                break;
            }
            case HINAVM_LDM32: {
                int32_t offset = SIGN_EXTEND20(inst.msg.imm);//符号扩展
                uint32_t val = *(uint32_t *) (m + offset);
                regs[inst.msg.a] = val;
                break;
            }
            case HINAVM_STM8: {
                int32_t offset = SIGN_EXTEND20(inst.msg.imm);//符号扩展
                uint8_t val = regs[inst.msg.a];
                *(uint8_t *) (m + offset) = val;
                break;
            }
            case HINAVM_STM16: {
                int32_t offset = SIGN_EXTEND20(inst.msg.imm);//符号扩展
                uint16_t val = regs[inst.msg.a];
                *(uint16_t *) (m + offset) = val;
                break;
            }
            case HINAVM_STM32: {
                int32_t offset = SIGN_EXTEND20(inst.msg.imm);//符号扩展
                uint32_t val = regs[inst.msg.a];
                *(uint32_t *) (m + offset) = val;
                break;
            }
            case HINAVM_PRINT: {
                int32_t val = regs[inst.print.a];
                DBG("%s: pc=%u: %d", current->name, pc, val);
                break;
            }
            case HINAVM_PRINT_HEX: {
                uint32_t val = regs[inst.print.a];
                DBG("%s: pc=%u: 0x%x", current->name, pc, val);
                break;
            }
            case HINAVM_SEND: {
                task_t dst_tid = regs[inst.ipc.b];
                struct task *dst = task_find(dst_tid);
                if (dst == NULL) {
                    WARN("%s: SEND: invalid task ID %d at pc=%u", current->name,
                         dst_tid, pc);
                    task_exit(EXP_ILLEGAL_EXCEPTION);
                }

                error_t err = ipc(dst, 0, (__user struct message *) &current->m,
                                  IPC_SEND | IPC_KERNEL);
                regs[inst.ipc.a] = err;
                break;
            }
            case HINAVM_REPLY: {
                task_t dst_tid = regs[inst.ipc.b];
                struct task *dst = task_find(dst_tid);
                if (dst == NULL) {
                    WARN("%s: REPLY: invalid task ID %d at pc=%u",
                         current->name, dst_tid, pc);
                    task_exit(EXP_ILLEGAL_EXCEPTION);
                }

                error_t err = ipc(dst, 0, (__user struct message *) &current->m,
                                  IPC_SEND | IPC_NOBLOCK | IPC_KERNEL);
                regs[inst.ipc.a] = err;
                break;
            }
            case HINAVM_RECV: {
                task_t src = regs[inst.ipc.b];
                error_t err = ipc(0, src, (__user struct message *) &current->m,
                                  IPC_RECV | IPC_KERNEL);
                regs[inst.ipc.a] = err;
                break;
            }
            case HINAVM_EXIT: {
                task_exit(EXP_GRACE_EXIT);
                break;
            }
            case HINAVM_NOP: {
                break;
            }
            default: {
                ERROR("%s: illegal instruction at pc=%u", current->name, pc);
                task_exit(EXP_ILLEGAL_EXCEPTION);
                break;
            }
        }

        pc = next_pc;
    }
}
