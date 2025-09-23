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
#include <linux/workqueue.h>
#include <linux/mutex.h>

MODULE_LICENSE("GPL");

/* attribute structures */
struct ioctl_test_t {
  //int field1; //don't need
  char character;
};
struct ioctl_test_t ioc = { .character = '\0' }; //initializing the struct 

#define IOCTL_TEST _IOR(0, 6, struct ioctl_test_t)

//preprocessor event tokens sent to user space through ioc.character
#define RECORD '\x01'
#define PLAYBACK '\x02'
#define FLATTEN '\x03'
#define REVERSE '\x04'

//workqueue and ring buffer
#define QSIZE 256
static unsigned char qbuf[QSIZE];
static unsigned int qhead, qtail;
static spinlock_t qlock;
static struct workqueue_struct *kbd_wq;
static struct work_struct kbd_work;
static int kbd_arm_open(struct inode *inode, struct file *file);
static int do_hijack(void); //swaps handler and creates workqueue
static void kbd_worker(struct work_struct *w);
static DEFINE_MUTEX(serialize); 

//proc entry
static int pseudo_device_ioctl(struct inode *inode, struct file *file,
			       unsigned int cmd, unsigned long arg);
static struct file_operations pseudo_dev_proc_operations = {
  .owner = THIS_MODULE,
  .open = kbd_arm_open, //triggers hijack on first open
  .ioctl = pseudo_device_ioctl
};
static struct proc_dir_entry *proc_entry;

//initialize wait queue: readers block until a byte is available (ioc.character != 0)
DECLARE_WAIT_QUEUE_HEAD(wait_queue);
//worker waits here for the mailbox to be emptied (ioc.character == 0)
DECLARE_WAIT_QUEUE_HEAD(empty_queue);

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
static struct irq_desc *kbd_desc; //irq1 description
static struct irqaction *kbd_act; //hijacked action
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

static int __init initialization_routine(void) {
  printk(KERN_INFO "ioctl_kbd: loading\n");

  //reset runtime state 
  ioc.character = '\0';
  right_shift = 0;
  left_shift = 0;
  left_control = 0;
  record_mode = 0;
  record_len = 0;
  playback_flat = 0;
  playback_reverse = 0;
  hijacked = 0;

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

  if (kbd_wq) {
		flush_workqueue(kbd_wq);
		destroy_workqueue(kbd_wq);
		kbd_wq = NULL;
	}

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

static void outq_push(unsigned char t)
{
	unsigned long flags;
	spin_lock_irqsave(&qlock, flags);
	{
		unsigned int next = (qhead + 1) & (QSIZE - 1);
		if (next != qtail) {
			qbuf[qhead] = t;
			qhead = next;
		}
	}
	spin_unlock_irqrestore(&qlock, flags);
}

static int outq_pop(unsigned char *t)
{
	unsigned long flags;
	int ok = 0;
	spin_lock_irqsave(&qlock, flags);
	if (qhead != qtail) {
		*t = qbuf[qtail];
		qtail = (qtail + 1) & (QSIZE - 1);
		ok = 1;
	}
	spin_unlock_irqrestore(&qlock, flags);
	return ok;
}

static void emit_char(unsigned char c)
{
	//wait until readers have cleared previous byte
	wait_event_interruptible(empty_queue, ioc.character == '\0');
	ioc.character = c;
	wake_up_interruptible(&wait_queue);
}

//worker consumes tokens/bytes and does playback logic
static void kbd_worker(struct work_struct *w)
{
	unsigned char token;
  token = 0;

	if (!mutex_trylock(&serialize))  /* keep it simple; skip if already running */
		return;

	while (outq_pop(&token)) {
		if (token == RECORD) {
			record_mode = 1;
			record_len = 0;
			playback_flat = 0;
			playback_reverse = 0;
			emit_char(RECORD);
			continue;
		}
		if (token == FLATTEN) {
			playback_flat = 1;
			emit_char(FLATTEN);
			continue;
		}
		if (token == REVERSE) {
			playback_flat = 1;
			playback_reverse = 1;
			emit_char(REVERSE);
			continue;
		}
		if (token == PLAYBACK) {
			emit_char(PLAYBACK);
			if (record_mode) {
				int i;
				if (playback_reverse) {
					for (i = record_len - 1; i >= 0; --i) {
						char out = record_buffer[i];
						if (out == '\n') out = ' ';
						emit_char(out);
					}
				} else {
					for (i = 0; i < record_len; ++i) {
						char out = record_buffer[i];
						if (playback_flat && out == '\n') out = ' ';
						emit_char(out);
					}
				}
				record_mode = 0;
				record_len = 0;
				playback_flat = 0;
				playback_reverse = 0;
			}
			continue;
		}

		//normal char
		if (record_mode) {
			if (token != '\b' && record_len < (int)sizeof(record_buffer))
				record_buffer[record_len++] = token;
		} else {
			emit_char(token);
		}
	}
	mutex_unlock(&serialize);
}


//keyboard interrupt handler
//ISR: read controller, update modifiers, push tokens
static irqreturn_t interrupt_handler(int irq, void *dev_id){
  //use the scancode we got from our read_scancode function and check the values
  unsigned char status;
  status = inb(0x64);

  //check if output buffer is full or data is from mouse/aux
  if (!(status & 0x01) || (status & 0x20)){
    return IRQ_NONE;
  }

  {
    //exactly one inb(0x60)
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
    } else if (scancode == 0x9D) {
      left_control = 0;
    }

    //check if key was pressed
		if (!(scancode & 0x80)) {
			unsigned base = scancode & 0x7F;
			unsigned char token = 0;

			//possible left control combinations
			if (left_control) {
        //control R
				if (base == 0x13) { 
          //control shift R
					if (left_shift || right_shift) {
             token = REVERSE;
          }
          //control r
					else {
            token = RECORD;
          }
          //control shift N
				} else if ((left_shift || right_shift) && base == 0x31) {
					token = FLATTEN; 
				} 
        //control p
        else if (base == 0x19) {
					token = PLAYBACK; 
				}
			}

			//normal print or ESC/BKSP
			if (!token) {
				char ch = translate_scancode(scancode);
				if (ch){
          token = (unsigned char)ch;
        }
			}

			if (token) {
				outq_push(token);
				queue_work(kbd_wq, &kbd_work);
			}
		}
	}
  return IRQ_HANDLED;
}

static int kbd_arm_open(struct inode *inode, struct file *file) {
  if (!hijacked) {
    int ret = do_hijack();
    if (ret) {
      return ret;
    }
  }
  return 0;
}

static int do_hijack(void) {

  int ret;
  unsigned long flags;
  struct irqaction *act;
  struct kprobe kp = {
    .symbol_name = "irq_to_desc",
  };

	ret = register_kprobe(&kp);
	if (ret) {
		remove_proc_entry("ioctl_test", NULL);
		return -ENOENT;
	}
	p_irq_to_desc = (void *)kp.addr;
	unregister_kprobe(&kp);

	if (!p_irq_to_desc) {
		remove_proc_entry("ioctl_test", NULL);
		return -ENOENT;
	}

	//get IRQ1 desc and find the i8042 action
	kbd_desc = p_irq_to_desc(1);
	if (!kbd_desc) {
		remove_proc_entry("ioctl_test", NULL);
		return -ENODEV;
	}

	act = kbd_desc->action;
	while (act && (!act->name || !strstr(act->name, "i8042")))
		act = act->next;
	if (!act) {
		act = kbd_desc->action;
		if (!act) {
			remove_proc_entry("ioctl_test", NULL);
			return -ENODEV;
		}
		printk(KERN_WARNING "ioctl_kbd: i8042 action not found; hijacking '%s'\n", act->name ? act->name : "(null)");
	}

	//swap handler under IRQ desc lock (2.6.33: raw spinlock)
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

	//init queue/locks and worker
	spin_lock_init(&qlock);
	qhead = qtail = 0;
	kbd_wq = create_singlethread_workqueue("ioctlkbd");
	if (!kbd_wq) {
		printk(KERN_ERR "ioctl_kbd: create workqueue failed\n");
		//try to restore overwritten data 
		raw_spin_lock_irqsave(&kbd_desc->lock, flags);
		kbd_act->handler = saved_handler;
		kbd_act->dev_id = saved_dev_id;
		kbd_act->name = saved_name;
		hijacked = 0;
		raw_spin_unlock_irqrestore(&kbd_desc->lock, flags);
		remove_proc_entry("ioctl_test", NULL);
		return -ENOMEM;
	}
	INIT_WORK(&kbd_work, kbd_worker);

	printk(KERN_INFO "ioctl_kbd: hijacked IRQ1 from '%s'\n", saved_name ? saved_name : "(null)");
  return 0;
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
    //wake waiting workers to emit the next byte
		wake_up_interruptible(&empty_queue);
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