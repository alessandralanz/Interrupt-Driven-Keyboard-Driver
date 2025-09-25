# Custom Interrupt-Driven Keyboard Driver

## Description
Interrupt driven keyboard driver for Linux 2.6.33.3 and a user space test program. The driver disables the default Linux keyboard handler by hijacking IRQ1 and then takes full control of keyboard input.
The driver supports:
- Interrupt-driven input handling
- Keyboard modifiers like shift 
- Backspace for deleting characters
### Record-Replay Modes
In addition to handling normal keystrokes the driver supports macro modes that allow keystrokes to be recorded and replayed with transformations. These are triggered by **Ctrl + key** combinations:
#### Record Mode `(Ctrl + r)`
- Starts recording all subsequent keystrokes into an internal buffer
- Characters do not appear on the screen while recording
- Recording continues until a playback command sent

#### Playback Mode `(Ctrl + p)`
- Replays all recorded keystrokes from the buffer and sends them to user space for display
- Once the playback completes the buffer is cleared and normal typing restarts

#### Flatten Mode `(Ctrl + N)`
- Must be pressed during recording mode
- Marks the buffer so that when it is replayed newlines are replaced with spaces

#### Reverse + Flatten Mode `(Ctrl + R)`
- Similar to Flatten but also reverses the recorded string before playback
- Playback will emit characters from the buffer in reverse order with newlines turned into spaces

## Building and Running the Keyboard Driver
### 1. Build and Clean the kernel module and user program
From the project directory run:
- `make`: compiles the kernel module (`ioctl_module.ko`) and the user test program (`ioctl_test`)
- `make clean`: removes all compiled files

If you only want to build the kernel module or user program individually you can run:
- `make module`: builds only ioctl_module.ko
- `make user`: builds only ioctl_test

### 2. Insert the kernel module
`insmod ./ioctl_module.ko`: loads the custom keyboard driver into the kernel

### 3. Run the user test program
Within 5 seconds of loading the custom keyboard driver you must run the user test program to interact with the driver by doing:
`./ioctl_test`
 so any pending/ buffered keystrokes are handled by the stock handler before we hijack IRQ1

 If you miss the 5 second window you will be unable to receive signals from the keyboard and will have to reboot your virtual machine and repeat the previous steps again.

The user test program: 
- Calls `my_getchar()` (via `ioctl`) to fetch keystrokes
- Prints characters as you type
- Handles backspace in the console
- Shows messages when entering record and playback modes
Press **ESC** to exit. When ESC is pressed the driver will also restore control of IRQ1 back to the stock Linux keyboard handler. This can be verified by looking at `cat /proc/interrupts` which should display i8042 handling IRQ1.

### 4. Remove the kernel module
When finished run `rmmod ioctl_module` to unload the module. This can be checked by running `lsmod` which should show no modules

## Sources
- [OS Primer Writeup](https://www.cs.bu.edu/fac/richwest/cs552_fall_2025/assignments/primer/primer.html)
- [OS Dev: I8042 PS/2 Controller](https://wiki.osdev.org/I8042_PS/2_Controller)
- [Stack Overflow: Linux Module Call Functions Which is in /proc/kallsyms but not Exported](https://stackoverflow.com/questions/6455343/linux-module-call-functions-which-is-in-proc-kallsyms-but-not-exported)
- [An Introduction to KProbes](https://lwn.net/Articles/132196/)
- [Stack Overflow: Keyboard Interrupt Missed in Kernel Module](https://stackoverflow.com/questions/33933802/keyboard-interrupt-missed-in-kernel-module)
- [OS Dev: PS/2 Keyboard](https://wiki.osdev.org/PS/2_Keyboard)
- [Smith College: A Keyboard Interrupt Handler](https://www.science.smith.edu/~nhowe/262/oldlabs/keyboard.html)
- [Bootlin: Linux v2.6.33.3 Source Code](https://elixir.bootlin.com/linux/v2.6.33.3/source)
- [The Linux Kernel Module Programming Guide, Chapter 12](https://tldp.org/LDP/lkmpg/2.4/html/x1210.html)
- [The Linux Kernel Module Programming Guide, Chapter 2](https://tldp.org/LDP/lkmpg/2.6/html/x351.html#AEN374)
- [Kernel Probes (Kprobes)](https://docs.kernel.org/trace/kprobes.html)
