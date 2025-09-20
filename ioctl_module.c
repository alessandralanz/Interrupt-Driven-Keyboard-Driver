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
  //int field1; //don't need
  char character;
};
struct ioctl_test_t ioc = { .character = '\0' }; //initializing the struct 

#define IOCTL_TEST _IOR(0, 6, struct ioctl_test_t)

//function declarations 
static int pseudo_device_ioctl(struct inode *inode, struct file *file,
			       unsigned int cmd, unsigned long arg);

//initialize wait queue
DECLARE_WAIT_QUEUE_HEAD(wait_queue);

//read only scancode to ASCII tables
//read only because we don't want them to be accidentally modified by kernel code
static char keymap[128] = "\0\e1234567890-=\177\tqwertyuiop[]\n\0asdfghjkl;'\0\\zxcvbnm,./\0*\0 \0\0\0\0\0\0\0\0\0\0\0\0\000789-456+1230.\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0"; 
static char keymap_shift[128] = "\0\e!@#$%^&*()_+\177\tQWERTYUIOP{}\n\0ASDFGHJKL:\"~\0|ZXCVBNM<>?\0*\0 \0\0\0\0\0\0\0\0\0\0\0\0\000789-456+1230.\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0";

//keyboard states
//need a state for the right shift, left shift, ctrl
static int right_shift = 0;
static int left_shift = 0;
static int left_control = 0;
//need to define record mode, playback mode


//interrupt handler (ISR) which is called automatically by the kernel whenver the device associated with irq generates an interrupt
//limits the visibility of the function to the current file
//int irq: interrupt number being handled
//void *dev_id: pointer to a device specific identifier
static irqreturn_t interrupt_handler(int irq, void *dev_id);
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

//calls inline assmebly routine to read byte from I/O port address
static inline unsigned char inb( unsigned short usPort ) {
    unsigned char uch;
   
    asm volatile( "inb %1,%0" : "=a" (uch) : "Nd" (usPort) );
    return uch;
}

//get scancode from the keyboard at data port 0x60
//can read with inb
unsigned char read_scancode(void){
  unsigned char scancode = inb(0x60);
  return scancode;
}

//convert the scancode to characters
//returns ASCII char for scancode (0 if no printable char)
static char translate_scancode(unsigned char scancode) {
  //scan is the raw scancode byte from the keyboard
  //& 0x7F clears the top bit giving us the base index to match our 128 entry lookup table 
  unsigned int index = scancode & 0x7F; 
  //if bit 7 of scan is set its a break (release)
  //if bit 7 is clear its a make (press)
  int make = !(scancode & 0x80); 
  //ignore releases and only produce characters on press (so that we don't print twice)
  if (!make) {
    return 0;
  }

  if (left_shift || right_shift) {
    return keymap_shift[index];
  } else {
    return keymap[index];
  }
}

//keyboard interrupt handler
static irqreturn_t interrupt_handler(int irq, void *dev_id){
  //use the scancode we got from our read_scancode function and check the values
  unsigned char scancode;
  scancode = read_scancode();

  //left shift
  if (scancode == 0x2A) {
    left_shift = 1;
  } else if (scancode == 0xAA) {
    left_shift = 0;
  } 

  //right shift
  if (scancode == 0x36) {
    right_shift = 1;
  } else if (scancode == 0xB6) {
    right_shift = 0;
  }

  //left control key
  if (scancode == 0x1D) {
    left_control = 1;
    printk("control key pressed \n");
  } else if (scancode == 0x9D) {
    left_control = 0;
  }

  //record mode: left_control + r

  //translate to ASCII
  char ch = translate_scancode(scancode);
  if (ch) {
    ioc.character = ch; //store in global since we cannot directly return a value to user space (store to stash until user space asks for it)
    wake_up_interruptible(&wait_queue); //wake up any sleeping readers when ISR stores a new character into ioc.character
  }

  return 0; //irq handled
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
    break;
  
  default:
    return -EINVAL;
    break;
  }
  
  return 0;
}

module_init(initialization_routine); 
module_exit(cleanup_routine); 