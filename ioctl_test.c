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
#define RECORD(character) ((character) == '\x01')
#define PLAYBACK(character) ((character) == '\x02')
#define FLATTEN(character) ((character) == '\x03')
#define REVERSE(character) ((character) == '\x04')
#define ESC(character) ((unsigned char)(character) == '\e')
#define BACKSPACE(character) ((character) == '\b')

static void erase(void){
  fputs("\b \b", stdout);
  fflush(stdout);
}

int main (void) {
  int fd = open ("/proc/ioctl_test", O_RDONLY);
  if (fd < 0) {
    perror("open /proc/ioctl_test");
    return 1;
  }

  //wait for one byte per ioctl
  //kernel wakes up when a char or event is ready
  while (1){
    struct ioctl_test_t message;
    if (ioctl(fd, IOCTL_TEST, &message)) {
      perror("ioctl IOCTL_TEST");
      break;
    }
    unsigned char character = (unsigned char)message.character;
    if (ESC(character)){
      break;
    }
    if (BACKSPACE(character)){
      erase();
      continue;
    }

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

    putchar(character);
    fflush(stdout);
  }

  close(fd);
  return 0;
}