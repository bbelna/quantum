#pragma once

#include <quantum/stddef.h>

void switch_context(size_t** stack);
int create_default_frame(task_t* task, entry_point_t ep, void* arg);