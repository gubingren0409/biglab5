#pragma once

/* 中断控制 */

void push_off();
void pop_off();

/* 自旋锁 */

void spinlock_init(spinlock_t *lk, char *name);
bool spinlock_holding(spinlock_t *lk);
void spinlock_acquire(spinlock_t *lk);
void spinlock_release(spinlock_t *lk);
