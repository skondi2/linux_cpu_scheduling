#define LINUX

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/proc_fs.h>
#include <linux/slab.h>
#include <linux/sched.h>
#include <linux/spinlock.h>
#include "mp2_given.h"
#include "linux/list.h"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Group_ID");
MODULE_DESCRIPTION("CS-423 MP2");

#define DEBUG 1
// NOTES:
// https://hammertux.github.io/slab-allocator
// https://tldp.org/LDP/lki/lki-2.html

struct process_list {
        struct list_head list;
        unsigned long pid;
        struct mp2_task_struct* mp2_task;
};
struct process_list* registered_processes;

enum task_state { RUNNING, READY, SLEEPING };
struct mp2_task_struct {
        struct task_struct* linux_task; // represents the PCB
        struct timer_list* timer;
        struct list_head list;
        unsigned long period;
        unsigned long processing_time;
        unsigned long deadline_jiff; // need to know whether to put it to sleep or not?
        enum task_state state;
};

struct kmem_cache *mp2_cache;
spinlock_t lock;

void timer_callback(struct timer_list* data) {

}

ssize_t proc_read_callback(struct file* file, char __user *buf, size_t size, loff_t* pos) {
        char* data = kmalloc(size, GFP_KERNEL);
        memset(data, 0, size);

        // write to data the linked list data
        int bytes_read = 0;
        struct process_list* tmp;
        struct list_head* curr_pos;

        // <pid 1>: <period 1>, <processing time 1>
        // <pid 2>: <period 2>, <processing time 2>
        spin_lock_irq(&lock);
        list_for_each(curr_pos, &(registered_processes->list)) {
                tmp = list_entry(curr_pos, struct process_list, list);
                unsigned long pid = tmp->pid;
                struct mp2_task_struct* task = tmp->mp2_task;
                unsigned long period = task->period;
                unsigned long processing_time = task->processing_time;

                if (pid > *pos) {
                        int curr_bytes_read = sprintf(data + bytes_read, "%lu: %lu, %lu\n", pid, period, processing_time);
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

ssize_t proc_write_callback(struct file* file, const char __user *buf, size_t size, loff_t* pos) {
        if (*pos != 0) {
                return 0; // CHECK: that this is correct?
        }

        char* buf_cpy = kmalloc(size+1, GFP_KERNEL);
        memset(buf_cpy, 0, size+1);
        int success = copy_from_user(buf_cpy, buf, size+1);
        buf_cpy[size-1] = '\0';
        
        char* temp = kmalloc(size+1, GFP_KERNEL);
        strcpy(temp, buf_cpy);
        char* original = temp;

        char operation = buf_cpy[0];
        temp = temp + 2; // remove the first comma
        if (operation == 'R') { // R,<pid>,<period>,<processing time>
                char* pid = strsep(&temp, ",");
                char* period = strsep(&temp, ",");
                char* processing_time = temp;
                
                // allocate new struct mp2_task_struct using cache
                struct mp2_task_struct* task = kmem_cache_alloc(mp2_cache, GFP_KERNEL);
                
                // initialize task SLEEPING state, period, and processing time
                task->state = SLEEPING;
                kstrtoul(period, 10, &(task->period));
                kstrtoul(processing_time, 10, &(task->processing_time));

                // initialize linked list node pid and task
                struct process_list* tmp = (struct process_list*) kmalloc(sizeof(struct process_list), GFP_KERNEL);
                tmp->mp2_task = task;
                kstrtoul(pid, 10, &(tmp->pid));
                INIT_LIST_HEAD(&(tmp->list));

                // initialize task list head
                task->list = tmp->list;

                // initialize task timer
                struct timer_list* timer = kmalloc(sizeof(struct timer_list), GFP_KERNEL);
                timer_setup(timer, timer_callback, 0); // initializes the callback function and data
                task->timer = timer;

                // TODO: initialize task deadline_jiff
                // initialize task task_struct
                task->linux_task = find_task_by_pid(pid);

                // TODO: start task timer

                // insert task into list of tasks
                spin_lock_irq(&lock);
                list_add_tail(&(tmp->list), &(registered_processes->list));
                spin_unlock_irq(&lock);
        }
        else if (operation == 'Y') { // Y,<pid>
                char* pid = temp;
        }
        else if (operation == 'D') { // D,<pid>
                unsigned long pid = 0;
                kstrtoul(temp, 10, &(pid));

                // kmem_cache_free() frees the memory allocated to the mp2_task_struct previously allocated
                // remove node from list
                struct process_list *tmp;
                struct list_head *curr_pos, *q;
                struct mp2_task_struct* curr_task;
                spin_lock_irq(&lock);
                list_for_each_safe(curr_pos, q, &(registered_processes->list)) {
                        tmp = list_entry(curr_pos, struct process_list, list);
                        unsigned long curr_pid = tmp->pid;
                
                        if (curr_pid == pid) { 
                                // deallocation of mp2_task_struct
                                curr_task = tmp->mp2_task;
                                del_timer_sync(curr_task->timer); // deactivate timer and ensure handler has finished
                                kfree(curr_task->timer);
                                kmem_cache_free(mp2_cache, curr_task);

                                // deallocation of node
                                list_del(curr_pos);
                                kfree(tmp);

                                break;
                        }
                }
                
                spin_unlock_irq(&lock);
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
        printk(KERN_ALERT "MP2 MODULE LOADING\n");
        #endif
        
        proc_dir = proc_mkdir("mp2", NULL);
        proc_file = proc_create("status", 0666, proc_dir, &proc_fops);

        // create new cache of size sizeof(mp2_task_struct)
        mp2_cache = KMEM_CACHE(mp2_task_struct, SLAB_PANIC|SLAB_ACCOUNT);

        registered_processes = kmalloc(sizeof(struct mp2_task_struct), GFP_KERNEL);
        INIT_LIST_HEAD(&(registered_processes->list));

        spin_lock_init(&lock);

        printk(KERN_ALERT "MP2 MODULE LOADED\n");
        return 0;
}

// mp2_exit - Called when module is unloaded
void __exit mp2_exit(void)
{
        #ifdef DEBUG
        printk(KERN_ALERT "MP2 MODULE UNLOADING\n");
        #endif

        struct process_list *tmp;
        struct list_head *pos, *q;
        struct mp2_task_struct* curr_task;
        list_for_each_safe(pos, q, &(registered_processes->list)) {
                tmp = list_entry(pos, struct process_list, list);

                printk("MP2 mp2_exit(): DELETING NODE");

                // deallocation related to mp2_task_struct
                curr_task = tmp->mp2_task;
                del_timer_sync(curr_task->timer);
                kfree(curr_task->timer);

                kmem_cache_free(mp2_cache, curr_task);

                // deallocation of node
                list_del(pos);
                kfree(tmp);
        }
        
        kmem_cache_destroy(mp2_cache);

        remove_proc_entry("status", proc_dir);
        remove_proc_entry("mp2", NULL);

        printk(KERN_ALERT "MP2 MODULE UNLOADED\n");
}

// Register init and exit funtions
module_init(mp2_init);
module_exit(mp2_exit);
