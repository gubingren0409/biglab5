#pragma once

/* proc.c */
void proc_init();
pgtbl_t proc_pgtbl_init(uint64 tf_va);
proc_t *proc_alloc();
void proc_free(proc_t *p);
void proc_make_first();
int proc_fork();
void proc_sched();
void proc_yield();
void proc_wakeup(void *chan);
void proc_sleep(void *chan, spinlock_t *lk);
void proc_scheduler();
void proc_exit(int code);
int proc_wait(uint64 addr);

/* [新增] exec.c */
int proc_exec(char *path, char **argv);