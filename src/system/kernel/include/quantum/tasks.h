#pragma once

#include <quantum/stddef.h>
#include <quantum/tasks_types.h>
#include <arch/x86/tasks.h>

int multitasking_init();
int create_kernel_task(tid_t* id, entry_point_t ep, void* args, uint8_t priority);
void reschedule();
void NORETURN leave_kernel_task();