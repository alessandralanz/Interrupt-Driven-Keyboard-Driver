//user side

#include <stdio.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <stdlib.h>
#include <unistd.h>

/* attribute structures */
struct ioctl_test_t {
  //int field1; //don't need
  char character;
};
struct ioctl_test_t ioc = { .character = '\0' };

#define IOCTL_TEST _IOR(0, 6, struct ioctl_test_t)
//event tokens from ioc.character
#define RECORD(character) ((character) == '\x01')
#define PLAYBACK(character) ((character) == '\x02')
#define FLATTEN(character) ((character) == '\x03')
#define REVERSE(character) ((character) == '\x04')
#define ESC(character) ((unsigned char)(character) == '\e')
#define BACKSPACE(character) ((character) == '\b')

//backspace + space + backspace results in the erasure of one char on the terminal
static void erase(void){
  fputs("\b \b", stdout);
  fflush(stdout); //flush to make sure the erase sequence is written right away
}

//calls ioctl() to ask the kernel driver for the next available character/event
//sleeps until the kernel wakes us when a key is pressed
//returns that char as an int which is later casted to unsigned char for printing
static int my_getchar(int fd) {
  struct ioctl_test_t msg = { .character = '\0' };
  if (ioctl(fd, IOCTL_TEST, &msg) < 0) {
     return -1;
  }
  return (unsigned char)msg.character;
}

int main (void) {
  int fd = open ("/proc/ioctl_test", O_RDONLY);
  if (fd < 0) {
    perror("open /proc/ioctl_test");
    return 1;
  }

  //wait for one byte per ioctl
  //kernel wakes up when a char or event is ready
  while (1) {
    //block until kernel driver delivers a char
    int ch = my_getchar(fd);
    if (ch < 0) {
      perror("my_getchar");
      break;
    }

    unsigned char character = (unsigned char) ch;
    if (ESC(character)){
      break; //exit on esc and restore IRQ1
    }
    if (BACKSPACE(character)){
      erase();
      continue;
    }

    //log for bonus events
    if (RECORD(character)){
      fprintf(stderr, "[record start]\n"); 
      continue; 
    }
    if (FLATTEN(character)) {
      fprintf(stderr, "[flatten on]\n");
      continue; 
    }
    if (REVERSE(character)) { 
      fprintf(stderr, "[reverse+flatten]\n");
      continue; 
    }
    if (PLAYBACK(character)) { 
      fprintf(stderr, "[playback]\n");       
      continue; 
    }

    //writes the single char to stdout (the terminal)
    putchar(character);
    //forces any buffered output to appear immediately
    fflush(stdout);
  }

  close(fd);
  return 0;
}