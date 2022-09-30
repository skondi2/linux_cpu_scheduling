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
// https://hammertux.github.io/slab-allocator
// https://tldp.org/LDP/lki/lki-2.html

struct mp2_task_struct {
        struct task_struct* linux_task;
        long period;
        long processing_time;
        struct timer_list wakeup_timer;
};
struct kmem_cache *mp2_cache;

struct process_list {
        struct list_head list;
        long pid;
        struct mp2_task_struct* mp2_task;
};
struct process_list* registered_processes;
spinlock_t lock;

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
                long pid = tmp->pid;
                struct mp2_task_struct* task = tmp->mp2_task;
                long period = task->period;
                long processing_time = task->processing_time;

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
                //printk("MP2 process sent R\n");
                char* pid = strsep(&temp, ",");
                char* period = strsep(&temp, ",");
                char* processing_time = temp;
                
                // allocate new struct mp2_task_struct using cache
                struct mp2_task_struct* task = kmem_cache_alloc(mp2_cache, GFP_KERNEL);
                // initialize task in SLEEPING state, set state to TASK_UNINTERRUPTIBLE
                set_current_state(TASK_UNINTERRUPTIBLE);
                kstrtol(period, 10, &(task->period));
                kstrtol(processing_time, 10, &(task->processing_time));

                // create new linked list node
                struct process_list* tmp = (struct process_list*) kmalloc(sizeof(struct process_list), GFP_KERNEL);
                tmp->mp2_task = task;
                kstrtol(pid, 10, &(tmp->pid));
                INIT_LIST_HEAD(&(tmp->list));
                
                // insert task into list of tasks
                spin_lock_irq(&lock);
                list_add_tail(&(tmp->list), &(registered_processes->list));
                spin_unlock_irq(&lock);
        }
        else if (operation == 'Y') { // Y,<pid>
                //printk("MP2 process sent Y\n");
                char* pid = temp;
                //printk("MP2 pid = %s\n", pid);
        }
        else if (operation == 'D') { // D,<pid>
                long pid = 0;
                kstrtol(temp, 10, &(pid));

                // kmem_cache_free() frees the memory allocated to the mp2_task_struct previously allocated
                // remove node from list
                struct process_list *tmp;
                struct list_head *curr_pos, *q;
                spin_lock_irq(&lock);
                list_for_each_safe(curr_pos, q, &(registered_processes->list)) {
                        tmp = list_entry(curr_pos, struct process_list, list);
                        long curr_pid = tmp->pid;
                        struct mp2_task_struct* curr_task = tmp->mp2_task;
                
                        if (curr_pid == pid) { 
                                list_del(curr_pos);
                                kfree(tmp);
                                kmem_cache_free(mp2_cache, curr_task);
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
        list_for_each_safe(pos, q, &(registered_processes->list)) {
                tmp = list_entry(pos, struct process_list, list);
                printk("MP2 mp2_exit(): DELETING NODE");
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
