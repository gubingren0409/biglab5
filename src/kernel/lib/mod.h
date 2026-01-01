#pragma once
#include "type.h"
#include "method.h"
#include "../arch/mod.h"
#include "../lock/mod.h"
#include "../mem/mod.h"   // [新增] 引入内存管理模块 (解决 uvm_copyin/out 报错)
#include "../proc/mod.h"