//kernel side

#include <linux/module.h>
#include <linux/init.h>
#include <linux/errno.h> /* error codes */
#include <linux/proc_fs.h>
#include <asm/uaccess.h>
#include <linux/tty.h>
#include <linux/sched.h>
#include <linux/interrupt.h>
#include <linux/wait.h>

MODULE_LICENSE("GPL");

/* attribute structures */
struct ioctl_test_t {
  int field1; //why do I need this field?
  char character;
};
struct ioctl_test_t ioc = { .character = '\0' }; //initializing the struct 

#define IOCTL_TEST _IOR(0, 6, struct ioctl_test_t)

//function declarations 
static int pseudo_device_ioctl(struct inode *inode, struct file *file,
			       unsigned int cmd, unsigned long arg);

//initialize wait queue
DECLARE_WAIT_QUEUE_HEAD(wait_queue);

//interrupt handler (ISR) which is called automatically by the kernel whenver the device associated with irq generates an interrupt
//limits the visibility of the function to the current file
//int irq: interrupt number being handled
//void *dev_id: pointer to a device specific identifier
static irqreturn_t interrupt_handler(int irq, void *dev_id)
void my_printk(char *string);

static struct file_operations pseudo_dev_proc_operations;

static struct proc_dir_entry *proc_entry;

static int __init initialization_routine(void) {
  printk("<1> Loading module\n");

  pseudo_dev_proc_operations.ioctl = pseudo_device_ioctl;

  /* Start create proc entry */
  proc_entry = create_proc_entry("ioctl_test", 0444, NULL);
  if(!proc_entry)
  {
    printk("<1> Error creating /proc entry.\n");
    return 1;
  }

  //proc_entry->owner = THIS_MODULE; <-- This is now deprecated
  proc_entry->proc_fops = &pseudo_dev_proc_operations;

  //registering the IRQ handler using the kernel API
  //need to initialize the keyboard interrupt by using interrupt_hander() function
  //IRQF_SHARED flag tells kernel the IRQ can be shared (important bc IRQ1 could be in use by the stock keyboard driver (i8042))
  //IRQ 1 = keyboard controller
  if (request_irq(1, interrupt_handler, IRQF_SHARED, "keyboard_interrupt", &interrupt_handler)){
    printk("<1> Cannot register IRQ 1 \n");
    return -EINVAL; //invalid argument
  }
  return 0;
}

/* 'printk' version that prints to active tty. */
void my_printk(char *string)
{
  struct tty_struct *my_tty;

  my_tty = current->signal->tty;

  if (my_tty != NULL) {
    (*my_tty->driver->ops->write)(my_tty, string, strlen(string));
    (*my_tty->driver->ops->write)(my_tty, "\015\012", 2);
  }
} 


static void __exit cleanup_routine(void) {

  printk("<1> Dumping module\n");
  remove_proc_entry("ioctl_test", NULL);
  free_irq(1, &interrupt_handler);

  return;
}


/***
 * ioctl() entry point...
 * called when processes try to open device files (eg. ioctl_test)
 */
static int pseudo_device_ioctl(struct inode *inode, struct file *file,
				unsigned int cmd, unsigned long arg)
{
  struct ioctl_test_t ioc;
  
  switch (cmd){

  case IOCTL_TEST:
    //wait until there is a new character
    //macro that puts a process to sleep until specification is met or we receive a signal
    //takes in a wait queue and a condition (not white space)
    wait_event_interruptible(wait_queue, ioc.character != '\0');
    if (ioc.character != '\0') {
      my_printk("Character received in kernel: ");
      //copies 
      //takes in the destination address in user space, source address in kernel space, and # of bytes to copy
      copy_to_user((struct ioctl_test_t *)arg, &ioc, sizeof(struct ioctl_test_t)) //explain?? the inputs
    }

    //need to reset ioc so that we do not continuously send the same character over and over unless it is being pressed
    ioc.character = '\0';

    //why do we copy from user after the kernel receives it??
    copy_from_user(&ioc, (struct ioctl_test_t *)arg, 
		   sizeof(struct ioctl_test_t));
    printk("<1> ioctl: call to IOCTL_TEST (%d,%c)!\n", 
	   ioc.field1, ioc.field2);

    my_printk ("Got msg in kernel\n");
    break;
  
  default:
    return -EINVAL;
    break;
  }
  
  return 0;
}

module_init(initialization_routine); 
module_exit(cleanup_routine); 