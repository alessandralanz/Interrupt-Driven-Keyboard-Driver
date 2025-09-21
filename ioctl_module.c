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

MODULE_LICENSE("GPL");

//idk what this is
static char *envp[] = {
  "HOME=/",
  "PATH=/sbin:/bin:/usr/sbin:/usr/bin",
  NULL
};

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


//interrupt handler (ISR) which is called automatically by the kernel whenver the device associated with irq generates an interrupt
//limits the visibility of the function to the current file
//int irq: interrupt number being handled
//void *dev_id: pointer to a device specific identifier
static irqreturn_t interrupt_handler(int irq, void *dev_id);
void my_printk(char *string);

static struct file_operations pseudo_dev_proc_operations;
static struct proc_dir_entry *proc_entry;

//read only scancode to ASCII tables
//don't want them to be accidentally modified by kernel code
static const char keymap[128] = "\0\e1234567890-=\177\tqwertyuiop[]\n\0asdfghjkl;'\0\\zxcvbnm,./\0*\0 \0\0\0\0\0\0\0\0\0\0\0\0\000789-456+1230.\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0"; 
static const char keymap_shift[128] = "\0\e!@#$%^&*()_+\177\tQWERTYUIOP{}\n\0ASDFGHJKL:\"~\0|ZXCVBNM<>?\0*\0 \0\0\0\0\0\0\0\0\0\0\0\0\000789-456+1230.\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0";

static int run_cmd(char **argv){
  //run user space helper and wait for it to finish
  return call_usermodehelper(argv[0], argv, envp, UMH_WAIT_PROC);
}

//apparently this is a stable dev_id so that free_irq() matches request_irq()??
static int kbd_dev_id;

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

  //unload stock keyboard modules so we can use our own IRQ1 exclusively
  {
    char *rmmod_atkbd[] = {"/sbin/rmmod", "atkbd", NULL};
    char *rmmod_i8042[] = {"/sbin/rmmod", "i8042", NULL};

    ret = run_cmd(rmmod_atkbd);
    if (ret)
      printk("<1> rmmod atkbd returned %d (ok if already gone)\n", ret);
    ret = run_cmd(rmmod_i8042);
    if (ret)
      printk("<1> rmmod i8042 returned %d (ok if already gone)\n", ret);
  }

  //registering the IRQ handler using the kernel API
  //need to initialize the keyboard interrupt by using interrupt_hander() function
  //request IRQ1 exclusively 
  ret = request_irq(1, interrupt_handler, IRQF_SHARED, "keyboard_interrupt", &kbd_dev_id);

  if (ret){
    printk("<1> request_irq(1) failed %d\n", ret);
    return ret; //invalid argument
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

  int ret;

  printk("<1> Dumping module\n");
  //free IRQ1 first so stock driver can claim it
  free_irq(1, &kbd_dev_id);

  //restore stock keyboard modules
  {
    char *modprobe_i8042[] = {"/sbin/modprobe", "i8042", NULL};
    char *modprobe_atkbd[] = {"/sbin/modprobe", "atkbd", NULL};

    ret = run_cmd(modprobe_i8042);
    if (ret)
      printk("<1> modprobe i8042 returned %d\n", ret);
    ret = run_cmd(modprobe_atkbd);
    if (ret)
      printk("<1> modprobe atkbd returned %d\n", ret);
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