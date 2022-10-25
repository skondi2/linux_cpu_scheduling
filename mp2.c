#define LINUX

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/proc_fs.h>
#include <linux/slab.h>
#include <linux/sched.h>
#include <linux/spinlock.h>
#include <linux/kthread.h>
#include <uapi/linux/sched/types.h>
#include "mp2_given.h"
#include "linux/list.h"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Group_ID");
MODULE_DESCRIPTION("CS-423 MP2");

#define DEBUG 1

struct process_list {
        struct list_head list;
        int pid;
        struct mp2_task_struct* mp2_task;
};
struct process_list* registered_processes;

enum task_state { RUNNING, READY, SLEEPING };
struct mp2_task_struct {
        struct task_struct* linux_task; // represents the PCB
        struct timer_list wakeup_timer;
        struct list_head list;
        int period;
        int processing_time;
        enum task_state state;
        unsigned long deadline_jiff; // my next period
};
struct task_struct* dispatch_thread;
struct mp2_task_struct* current_mp2_task;
struct kmem_cache *mp2_cache;
spinlock_t lock;

struct mp2_task_struct* get_mp2_struct(int pid) {
        struct process_list* tmp;
        struct list_head* curr_pos;
        struct mp2_task_struct* res = NULL;

        spin_lock_irq(&lock);
        list_for_each(curr_pos, &(registered_processes->list)) {
                tmp = list_entry(curr_pos, struct process_list, list);
                if (tmp->pid == pid) {
                        res = tmp->mp2_task;
                        break;
                }
        }
        spin_unlock_irq(&lock);
        return res;
}

struct mp2_task_struct* _get_shortest_ready_task(void) {
        //printk("MP2(): entering get_shortest_ready_task()");
        struct list_head* curr_pos;
        struct mp2_task_struct* next_task = NULL;
        int shortest_period = -1;

        list_for_each(curr_pos, &(registered_processes->list)) {
                struct process_list* tmp = list_entry(curr_pos, struct process_list, list);
                int period = tmp->mp2_task->period;
                
                if (tmp->mp2_task->state == READY) {
                        if (shortest_period == -1 || period < shortest_period) {
                                shortest_period = period;
                                next_task = tmp->mp2_task;
                        }
                }
        }
        if (current_mp2_task != NULL) {
                if (next_task != NULL) {
                        if (current_mp2_task->period <= next_task->period) {
                                return current_mp2_task;
                        }
                }
                /*else {
                        printk("MP2(): get_shortest_ready_task(): next_task is null");
                }*/
        }
        /*if (next_task != NULL) {
               printk("MP2(): get_shortest_ready_task(): found next_task"); 
        }*/
        return next_task;
}

void wakeup_timer_callback(struct timer_list* timer_list_) {
        // find calling task and set it as READY
        spin_lock_irq(&lock);
        struct mp2_task_struct* expired_task = container_of(timer_list_, struct mp2_task_struct, wakeup_timer);
        if (expired_task != NULL) {
                expired_task->state = READY;
        }
        /*else {
                printk("MP2(): timer callback: expired task was NULL");
        }*/
        spin_unlock_irq(&lock);

        // wakeup dispatch thread
        wake_up_process(dispatch_thread);
}

int dispatch_callback(void* arguments) {
        while (!kthread_should_stop()) {
                // find task in list with READY state and shortest period
                spin_lock_irq(&lock);
                struct mp2_task_struct* next_task = _get_shortest_ready_task();

                // if there are no READY tasks || the next shortest period is longer than current
                if (next_task == NULL) { 
                        //printk("MP2(): dispatch callback(): next task was NULL");
                        if (current_mp2_task != NULL) {
                                // preempt old task
                                struct sched_attr attr;
                                memset(&attr, 0, sizeof(attr));
                                attr.sched_policy = SCHED_NORMAL;
                                attr.sched_priority = 0;
                                sched_setattr_nocheck(current_mp2_task->linux_task, &attr);
                        }
                        /*else {
                                printk("MP2(): dispatch callback(): wait, current task is NULL too");
                        }*/
                }
                else { 
                        //printk("MP2(): dispatch callback(): gonna do context switch, pid = %d\n", next_task->linux_task->pid);
                        // old task set to READY if it was RUNNING 
                        if (current_mp2_task != NULL) {
                                current_mp2_task->state = (current_mp2_task->state == RUNNING) ? READY : current_mp2_task->state;
                        }
                        next_task->state = RUNNING;

                        // prioritize new task
                        struct sched_attr attr;
                        memset(&attr, 0, sizeof(attr));
                        wake_up_process(next_task->linux_task);
                        attr.sched_policy = SCHED_FIFO;
                        attr.sched_priority = 99;
                        sched_setattr_nocheck(next_task->linux_task, &attr);
                        
                        // preempt old task
                        if (current_mp2_task != NULL) {
                                struct sched_attr attr;
                                memset(&attr, 0, sizeof(attr));
                                attr.sched_policy = SCHED_NORMAL;
                                attr.sched_priority = 0;
                                sched_setattr_nocheck(current_mp2_task->linux_task, &attr);
                        }
                        /*else {
                                printk("MP2(): dispatch callback(): current task was NULL");
                        }*/

                        // reset what current_mp2_task points to
                        current_mp2_task = next_task;
                        current_mp2_task->deadline_jiff = jiffies + msecs_to_jiffies(current_mp2_task->period);
                        //printk("MP2(): dispatch callback(): finished doing context switch");
                }
                spin_unlock_irq(&lock);

                // put dispatch_thread to sleep
                set_current_state(TASK_INTERRUPTIBLE);
                schedule();
        }

        return 0;
}

ssize_t proc_read_callback(struct file* file, char __user *buf, size_t size, loff_t* pos) {
        char* data = kmalloc(size, GFP_KERNEL);
        memset(data, 0, size);

        // write to data the linked list data
        int bytes_read = 0;
        struct process_list* tmp;
        struct list_head* curr_pos;
        struct mp2_task_struct* temp_task;

        spin_lock_irq(&lock);
        list_for_each(curr_pos, &(registered_processes->list)) {
                tmp = list_entry(curr_pos, struct process_list, list);
                int pid = tmp->pid;
                temp_task = tmp->mp2_task;
                int period = temp_task->period;
                int processing_time = temp_task->processing_time;

                if (pid > *pos) {
                        int curr_bytes_read = sprintf(data + bytes_read, "%d: %d, %d\n", pid, period, processing_time);
                        if (bytes_read + curr_bytes_read >= size) {
                                break;
                        }
                        *pos = pid;
                        bytes_read += curr_bytes_read;
                }
        }
        data[bytes_read] = '\0';
        spin_unlock_irq(&lock);

        int success = copy_to_user(buf, data, bytes_read + 1);
        if (success != 0) {
                return 0;
        }
        kfree(data);

        return bytes_read;
}

int admission_control(int processing_time, int period) {
        int util_sum = 0;
        
        spin_lock_irq(&lock);
        struct process_list* tmp;
        struct list_head* curr_pos;
        list_for_each(curr_pos, &(registered_processes->list)) {
                tmp = list_entry(curr_pos, struct process_list, list);
                int tmp_processing_time = tmp->mp2_task->processing_time;
                int tmp_period = tmp->mp2_task->period;

                util_sum += (tmp_processing_time * 10000) / tmp_period;
        }
        spin_unlock_irq(&lock);

        util_sum += ((processing_time * 10000) / period);
        
        if (util_sum <= 6930) {
                return 1;
        }
        return 0;
}

ssize_t proc_write_callback(struct file* file, const char __user *buf, size_t size, loff_t* pos) {
        if (*pos != 0) {
                return 0; // CHECK: that this is correct?
        }
        
        char* buf_cpy = kmalloc(size+1, GFP_KERNEL);
        copy_from_user(buf_cpy, buf, size);
        buf_cpy[size] = '\0';
        
        char* temp = kmalloc(size+1, GFP_KERNEL);
        strcpy(temp, buf_cpy);
        char* original = temp;

        char operation = buf_cpy[0];
        temp = temp + 2; // remove the first comma
        if (operation == 'R') { // R,<pid>,<period>,<processing time>
                int pid;
                int period;
                int processing_time;
                kstrtoint(strsep(&temp, ","), 10, &pid);
                kstrtoint(strsep(&temp, ","), 10, &period);
                kstrtoint(temp, 10, &processing_time);

                // admission control
                int should_admit = admission_control(processing_time, period);

                if (should_admit == 1) {
                        // allocate new struct mp2_task_struct using cache
                        struct mp2_task_struct* task = kmem_cache_alloc(mp2_cache, GFP_KERNEL);
                        
                        // initialize task SLEEPING state, period, processing time, deadline
                        task->state = SLEEPING;
                        task->period = period;
                        task->processing_time = processing_time;
                        task->deadline_jiff = 0;

                        // initialize task wakeup_timer
                        timer_setup(&(task->wakeup_timer), wakeup_timer_callback, 0); // initializes the callback function and data

                        // initialize task task_struct
                        task->linux_task = find_task_by_pid(pid);
                        /*if (task->linux_task == NULL) {
                                printk("MP2(): registration, linux_task is NULL");
                        }*/

                        // initialize linked list node pid and task
                        struct process_list* tmp = (struct process_list*) kmalloc(sizeof(struct process_list), GFP_KERNEL);
                        tmp->mp2_task = task;
                        tmp->pid = pid;
                        INIT_LIST_HEAD(&(tmp->list));

                        // initialize task list head
                        task->list = tmp->list; // TODO: might delete?

                        // insert task into list of tasks
                        spin_lock_irq(&lock);
                        list_add_tail(&(tmp->list), &(registered_processes->list));
                        spin_unlock_irq(&lock);
                }
        }
        else if (operation == 'Y') { // Y,<pid>
                int pid;
                kstrtoint(temp, 10, &pid);

                // find yielding task
                struct mp2_task_struct* yielding_task = get_mp2_struct(pid);
                
                /*if (yielding_task == NULL) {
                        printk("MP2(): yield(): couldn't find yielding task");
                }*/
                if (yielding_task != NULL) {
                        if (yielding_task->deadline_jiff == 0 || jiffies >= yielding_task->deadline_jiff) {
                                yielding_task->state = READY;
                                if (yielding_task->deadline_jiff == 0) { // if process just registered and is ready to start
                                        //printk("MP2(): yield(): first time running");
                                        yielding_task->deadline_jiff = yielding_task->period;
                                        mod_timer(&(yielding_task->wakeup_timer), jiffies);
                                }
                                else { // if next period has already started
                                        //printk("MP2(): yield(): next period already started");
                                        yielding_task->deadline_jiff += yielding_task->period;
                                        mod_timer(&(yielding_task->wakeup_timer), jiffies + yielding_task->deadline_jiff);
                                }
                        }
                        else {
                                yielding_task->state = SLEEPING;

                                // set wakeup timer
                                unsigned long sleep_time = yielding_task->deadline_jiff - jiffies;
                                //printk("MP2(): yield(): sleep time = %lu\n", sleep_time);
                                mod_timer(&(yielding_task->wakeup_timer), jiffies + sleep_time);
                                
                                spin_lock_irq(&lock);
                                current_mp2_task = NULL;
                                spin_unlock_irq(&lock);

                                // wakeup dispatch thread
                                wake_up_process(dispatch_thread);

                                // put the task to sleep in TASK_UNINTERRUPTIBLE
                                set_current_state(TASK_UNINTERRUPTIBLE);
                                schedule();
                        }
                }
        }
        else if (operation == 'D') { // D,<pid>
                int pid;
                kstrtoint(temp, 10, &pid);

                // kmem_cache_free() frees the memory allocated to the mp2_task_struct previously allocated
                // remove node from list
                struct process_list *tmp;
                struct list_head *curr_pos, *q;
                struct mp2_task_struct* temp_task;

                spin_lock_irq(&lock);
                list_for_each_safe(curr_pos, q, &(registered_processes->list)) {
                        tmp = list_entry(curr_pos, struct process_list, list);
                        int curr_pid = tmp->pid;
                
                        if (curr_pid == pid) { 
                                // deallocation of mp2_task_struct
                                temp_task = tmp->mp2_task;
                                kmem_cache_free(mp2_cache, temp_task);

                                // deallocation of node
                                list_del(curr_pos);
                                kfree(tmp);

                                break;
                        }
                }
                spin_unlock_irq(&lock);
                //printk("MP2(): Deregistered");

                /*if (current_mp2_task == NULL) {
                        printk("MP2(): deregistration, current task is NULL");
                }*/
                if (current_mp2_task != NULL) {
                        if (current_mp2_task->linux_task->pid == pid) {
                                spin_lock_irq(&lock);
                                current_mp2_task = NULL;
                                spin_unlock_irq(&lock);

                                wake_up_process(dispatch_thread);
                        }
                }
        }
        kfree(original);
        return size;
}

const struct proc_ops proc_fops = {
   .proc_read = proc_read_callback,
   .proc_write = proc_write_callback,
};

struct proc_dir_entry* proc_dir;
struct proc_dir_entry* proc_file;

// mp2_init - Called when module is loaded
int __init mp2_init(void)
{
        #ifdef DEBUG
        printk(KERN_ALERT "MP2(): MODULE LOADING\n");
        #endif
        
        proc_dir = proc_mkdir("mp2", NULL);
        proc_file = proc_create("status", 0666, proc_dir, &proc_fops);

        // create new cache of size sizeof(mp2_task_struct)
        mp2_cache = KMEM_CACHE(mp2_task_struct, SLAB_PANIC);

        registered_processes = kmalloc(sizeof(struct process_list), GFP_KERNEL);
        INIT_LIST_HEAD(&(registered_processes->list));

        spin_lock_init(&lock);

        dispatch_thread = kthread_create(&dispatch_callback, NULL, "dispatch_thread");

        printk(KERN_ALERT "MP2(): MODULE LOADED\n");
        return 0;
}

// mp2_exit - Called when module is unloaded
void __exit mp2_exit(void)
{
        #ifdef DEBUG
        printk(KERN_ALERT "MP2(): MODULE UNLOADING\n");
        #endif

        struct process_list *tmp;
        struct list_head *pos, *q;
        struct mp2_task_struct* temp_task;
        list_for_each_safe(pos, q, &(registered_processes->list)) {
                tmp = list_entry(pos, struct process_list, list);

                // deallocation related to mp2_task_struct
                temp_task = tmp->mp2_task;
                kmem_cache_free(mp2_cache, temp_task);

                // deallocation of node
                list_del(pos);
                kfree(tmp);

                //printk("MP2(): mp2_exit(): AFTER DELETING NODE");
        }
        
        kmem_cache_destroy(mp2_cache);

        kthread_stop(dispatch_thread);

        remove_proc_entry("status", proc_dir);
        remove_proc_entry("mp2", NULL);

        printk(KERN_ALERT "MP2(): MODULE UNLOADED\n");
}

// Register init and exit funtions
module_init(mp2_init);
module_exit(mp2_exit);
