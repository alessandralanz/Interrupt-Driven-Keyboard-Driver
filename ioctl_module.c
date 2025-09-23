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
#include <linux/string.h>
#include <linux/kmod.h>
#include <linux/irq.h>
#include <linux/kprobes.h>
#include <linux/spinlock.h>
#include <linux/delay.h>

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
static struct file_operations pseudo_dev_proc_operations;
static struct proc_dir_entry *proc_entry;

//initialize wait queue
DECLARE_WAIT_QUEUE_HEAD(wait_queue);

//interrupt handler (ISR) which is called automatically by the kernel whenver the device associated with irq generates an interrupt
//limits the visibility of the function to the current file
//int irq: interrupt number being handled
//void *dev_id: pointer to a device specific identifier
static irqreturn_t interrupt_handler(int irq, void *dev_id);
void my_printk(char *string);

//read only scancode to ASCII tables
//don't want them to be accidentally modified by kernel code
static const char keymap[128] = "\0\e1234567890-=\177\tqwertyuiop[]\n\0asdfghjkl;'\0\\zxcvbnm,./\0*\0 \0\0\0\0\0\0\0\0\0\0\0\0\000789-456+1230.\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0"; 
static const char keymap_shift[128] = "\0\e!@#$%^&*()_+\177\tQWERTYUIOP{}\n\0ASDFGHJKL:\"~\0|ZXCVBNM<>?\0*\0 \0\0\0\0\0\0\0\0\0\0\0\0\000789-456+1230.\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0";

//apparently this is a stable dev_id so that free_irq() matches request_irq()??
static int kbd_dev_id;

struct irq_desc;
struct irqaction;
static struct irq_desc *(*p_irq_to_desc)(unsigned int);
//keeping what we overwrite so we can restore it
static struct irq_desc *kbd_desc;
static struct irqaction *kbd_act;
static irq_handler_t saved_handler;
static void *saved_dev_id;
static const char *saved_name;
static int hijacked;

//keyboard states
//need a state for the right shift, left shift, ctrl
static int right_shift = 0;
static int left_shift = 0;
static int left_control = 0;
static char record_buffer[1024];
static int record_len = 0;
static int record_mode = 0;
static int playback_flat = 0;
static int playback_reverse = 0;
//preprocessor event tokens sent to user space through ioc.character
#define RECORD '\x01'
#define PLAYBACK '\x02'
#define FLATTEN '\x03'
#define REVERSE '\x04'


static int __init initialization_routine(void) {
  int ret;
  unsigned long flags;
  struct irqaction *act;
  struct kprobe kp = {
    .symbol_name = "irq_to_desc",
  };

  printk(KERN_INFO "<1> Loading module\n");

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

  //5 sec pause so any pending/ buffered keystrokes are handled by the stock handler before we hijack IRQ1
  msleep(5000); 

  //use kprobe for irq_to_desc
  ret = register_kprobe(&kp);
  if (ret) {
    printk(KERN_ERR "ioctl_module: kprobe resolve irq_to_desc failed: %d\n", ret);
    remove_proc_entry("ioctl_test", NULL);
    return -ENOENT;
  }
  p_irq_to_desc = (void *)kp.addr;
  unregister_kprobe(&kp);

  if (!p_irq_to_desc) {
    printk(KERN_ERR "ioctl_module: irq_to_desc address null\n");
    remove_proc_entry("ioctl_test", NULL);
    return -ENOENT;
  }

  //get IRQ1 descriptor
  kbd_desc = p_irq_to_desc(1);
  if (!kbd_desc) {
    printk(KERN_ERR "ioctl_module: irq_to_desc(1) -> NULL\n");
    remove_proc_entry("ioctl_test", NULL);
    return -ENODEV;
  }

  //find i8042 action on IRQ1
  act = kbd_desc->action;
  while (act && (!act->name || !(strstr(act->name, "i8042")))) {
    act = act->next;
  }
  if (!act) {
    //if there’s only one hijack that
    act = kbd_desc->action;
    if (!act) {
      printk(KERN_ERR "ioctl_module: no irqaction on IRQ1\n");
      remove_proc_entry("ioctl_test", NULL);
      return -ENODEV;
    }
    printk(KERN_WARNING "ioctl_module: i8042 action not found; hijacking first action named '%s'\n", act->name ? act->name : "(null)");
  }

  //swap the handler under the desc lock
  raw_spin_lock_irqsave(&kbd_desc->lock, flags);

  kbd_act = act;
  saved_handler = act->handler;
  saved_dev_id = act->dev_id;
  saved_name = act->name;

  act->handler = interrupt_handler;
  act->dev_id = &kbd_dev_id;
  act->name = "ioctl_kbd";
  hijacked = 1;
  raw_spin_unlock_irqrestore(&kbd_desc->lock, flags);

  printk(KERN_INFO "ioctl_module: hijacked IRQ1 from '%s'\n", saved_name ? saved_name : "(null)");
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
  unsigned long flags;

  if (hijacked && kbd_desc && kbd_act) {
    raw_spin_lock_irqsave(&kbd_desc->lock, flags);
    kbd_act->handler = saved_handler;
    kbd_act->dev_id = saved_dev_id;
    kbd_act->name = saved_name;
    hijacked = 0;
    raw_spin_unlock_irqrestore(&kbd_desc->lock, flags);
    printk(KERN_INFO "ioctl_module: restored IRQ1 action '%s'\n", saved_name ? saved_name : "(null)");
  }
  remove_proc_entry("ioctl_test", NULL);
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
  unsigned int index;
  char ch;
  
  //ignore releases and only produce characters on press (so that we don't print twice)
  if (scancode & 0x80){
    return 0;
  }

  //`& 0x7F` clears the top bit giving us the base index to match our 128 entry lookup table 
  index = scancode & 0x7F; 

  //backspace 
  if (index == 0x0E){
    return '\b'; //use this or return keymap[index]??
  }

  ch = (left_shift || right_shift) ? keymap_shift[index] : keymap[index];

  return ch;
}

//keyboard interrupt handler
static irqreturn_t interrupt_handler(int irq, void *dev_id){
  //use the scancode we got from our read_scancode function and check the values
  unsigned char scancode, scan;
  scan = inb(0x64);

  //check if output buffer is full or data is from mouse/aux
  if (!(scan & 0x01) || (scan & 0x20)){
    return IRQ_NONE;
  }

  //exactly one inb(0x60)
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
  } else if (scancode == 0x9D) {
    left_control = 0;
  }

  if (left_control && (!(scancode & 0x80))){
    //record mode: left_control + r
    //check letter's scancode (0x13 for r) and ctrl state
    if ((scancode & 0x7F) == 0x13){
      if (!record_mode){
        record_mode = 1;
        record_len = 0;
        playback_flat = 0;
        playback_reverse = 0;
        ioc.character = RECORD;
        wake_up_interruptible(&wait_queue);
      } 
      return IRQ_HANDLED;
    }

    //Reverse and flatten: left_control + R after left_control + r
    if (((scancode & 0x7F) == 0x13) && (left_shift || right_shift)){
      //check if we are already recording
      if (record_mode){
        playback_flat = 1;
        playback_reverse = 1;
        ioc.character = REVERSE;
        wake_up_interruptible(&wait_queue);
      }
      return IRQ_HANDLED;
    }

    //flatten playback: left_control + N
    if ((scancode & 0x7F) == 0x31 && (left_shift || right_shift)){
      if (record_mode){
        playback_flat = 1;
        ioc.character = FLATTEN;
        wake_up_interruptible(&wait_queue);
      }
      return IRQ_HANDLED;
    }

    //playback: left_control + p 
    //perform playback then clear state
    if ((scancode & 0x7F) == 0x19){
      if (record_mode){
        int i;
        ioc.character = PLAYBACK;
        wake_up_interruptible(&wait_queue);

        if (playback_reverse) {
          for (i = record_len - 1; i >= 0; i--){
            char out = record_buffer[i];
            if (out == '\n') {
              out = ' ';
            }
            ioc.character = out;
            wake_up_interruptible(&wait_queue);
          }
        } else {
          for (i = 0; i < record_len; ++i){
            char out = record_buffer[i];
            if (playback_flat && out == '\n') {
              out = ' ';
            }
            ioc.character = out;
            wake_up_interruptible(&wait_queue);
          }
        }
        record_mode = 0;
        record_len = 0;
        playback_flat = 0;
        playback_reverse = 0;
      }
      return IRQ_HANDLED;
    }
  }
  //translate to ASCII
  if (!(scancode & 0x80)){
    char ch = translate_scancode(scancode); //ignore breaks
    //only continue if we have something to deliver or buffer from translate_scancode (0 means no printable char)
    if (ch) {
      //buffer only and save for playback
      //we do not want to emit while recording
      if (record_mode){
        //skip backspace during recording? 
        //idk instructions say to not worry about embedded modifier sequences
        //prevent buffer overflow; if full drop extra chars
        if (ch != '\b' && record_len < (int)sizeof(record_buffer)){
          record_buffer[record_len++] = ch;
        }
      }
      //normal emit
      //not recording so keystrokes are delivered immediately to user space
      else {
        ioc.character = ch; //store in global since we cannot directly return a value to user space (store to stash until user space asks for it)
        wake_up_interruptible(&wait_queue); //wake up any sleeping readers when ISR stores a new character into ioc.character
      }
    }
  }
  return IRQ_HANDLED; //irq handled
}


/***
 * ioctl() entry point...
 * called when processes try to open device files (eg. ioctl_test)
 */
static int pseudo_device_ioctl(struct inode *inode, struct file *file,
				unsigned int cmd, unsigned long arg)
{
  
  switch (cmd){

  case IOCTL_TEST: {
    int ret;
    //wait until there is a new character
    //macro that puts a process to sleep until specification is met or we receive a signal
    //takes in a wait queue and a condition (not white space)
    ret = wait_event_interruptible(wait_queue, ioc.character != '\0');
    if (ret) {
     return ret;
    }

    if (copy_to_user((struct ioctl_test_t __user *)arg, &ioc, sizeof(ioc))){
      return -EFAULT;
    }

    //need to reset ioc so that we do not continuously send the same character over and over unless it is being pressed
    ioc.character = '\0';
    break;
  }
  
  default:
    return -EINVAL;
    break;
  }
  
  return 0;
}

module_init(initialization_routine); 
module_exit(cleanup_routine); 