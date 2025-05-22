#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <termios.h>
#include <stdlib.h>
#include <errno.h>
#include <stdio.h>
#include <unistd.h>
#include <poll.h>
#include <string.h>

#define CTRL_KEY(k) ((k) & 0x1f)
#define MAXLINES 1000
#define SIZELINE 2000
#define DEL 127
#define BACKSPACE 8


struct termios orig_termios;

struct editorBuffer{
  char *lines[MAXLINES];
  int numlines;
  int cur_x;
  int cur_y;
  char cur_line[SIZELINE];
};

struct editorBuffer makeStringBuffer(){
  struct editorBuffer buffer;
  buffer.cur_x=0;
  buffer.cur_y=0;
  buffer.numlines=0;
  for(int i = 0; i<MAXLINES; i++){
    buffer.lines[i] = NULL;
  }
  return buffer;
}


// TERMINAL
void die(const char *s){
  perror(s);
  exit(1);
}

void disableRawMode(){
  if(tcsetattr(STDIN_FILENO, TCSAFLUSH , &orig_termios) == -1) die("tcssetattr");
}

void enableRawMode(){
  if (tcgetattr(STDIN_FILENO, &orig_termios) == -1) die("tcgetattr");
  atexit(disableRawMode);

  struct termios raw = orig_termios;
  raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
  raw.c_oflag &= ~(OPOST);

  raw.c_cflag |= (CS8);
  raw.c_cc[VMIN] = 0;
  raw.c_cc[VTIME] = 1;

  raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);

  if (tcsetattr(STDIN_FILENO, TCSAFLUSH , &raw) == -1) die("tcsetattr");
}

//HELPER
void linecopy(char *to, char *from) {
  while (*from != '\0') {
    *to++ = *from++;
  }
  *to='\0';
}

int isInputAvailable() {
    struct pollfd pfd;
    pfd.fd = STDIN_FILENO;
    pfd.events = POLLIN;
    // timeout = 0 ms, non-blocking
    int ret = poll(&pfd, 1, 0);
    return ret > 0;
}

void moveCharsRight(char *str, int cur_pos, int n){
    int len = strlen(str);

    for (int i = len; i >= cur_pos; i--) {
        str[i + n] = str[i];
    }
}

void moveCharsLeft(char *str, int cur_pos, int n) {
    int len = strlen(str);

    if(cur_pos < n) return;  // нельзя сдвинуть влево, выйдем за пределы

    for(int i = cur_pos; i <= len; i++) {
        str[i - n] = str[i]; 
    }
}
// OUTPUT
void editorRefreshScreen() {
  write(STDOUT_FILENO, "\x1b[2J", 4);
  write(STDOUT_FILENO, "\x1b[H", 3);
}

void editorOutputBufferText(struct editorBuffer *buffer) {
  for (int i = 0; i < buffer->numlines; i++) {
    char *str = buffer->lines[i];
    while (*str != '\0') {
      write(STDOUT_FILENO, str, 1);
      str++;
    }
  }
}

void editorRefreshCurrentLine(struct editorBuffer *buffer){
  write(STDOUT_FILENO, "\x1b[2K", 4); // clear the line
  write(STDOUT_FILENO, "\x1b[H", 3);
  char *str = buffer->lines[buffer->cur_y];

  while (*str != '\0') {
    write(STDOUT_FILENO, str, 1);
    str++;
  }
}

//INPUT
void deleteChar(struct editorBuffer *buffer){
  moveCharsLeft(buffer->cur_line, buffer->cur_x, 1);
  buffer->cur_x--;
}

void writeCurrentLineToBuffer(struct editorBuffer *buffer){
  int size = strlen(buffer->cur_line);

  if(buffer->lines[buffer->numlines] != NULL){
    write(STDOUT_FILENO, " free memory ", 12);
    sleep(1);
    free(buffer->lines[buffer->numlines]);
  }
  buffer->lines[buffer->numlines] = malloc(size);

  if (buffer->lines[buffer->numlines] == NULL) {
    die("writeCurrentLineToBuffer: malloc failed");
  }
  //check I am at the last line
  linecopy(buffer->lines[buffer->cur_y], buffer->cur_line);
  //TODO if its not the last line of the file, we need to move the rest of the lines in the buffer to be able to make the space
}

char editorReadKey(struct editorBuffer *buffer){
  char nread; // output result code
  char c;
  while((nread = read(STDIN_FILENO, &c, 1)) != 1 ){
    if(nread == -1 && errno != EAGAIN) die("read");
  }
  return c;
}

void bufferWriteChar(struct editorBuffer *buffer, char c){
  //после каждого записанного char ставить в конце \0
  //перед записью нового символа вычислять длину строки и тем самым определять находиться ли cur_x в конце
  //если в конце, то просто писать дальше
  //если нет, двигать поинеты вперед и писать между ними
  if (buffer->cur_x > SIZELINE - 1) die("bufferWriteChar: SIZELINE is exceeded");

  buffer->cur_line[buffer->cur_x++] = c;
  int size = sizeof(buffer->cur_line);
  if(size==buffer->cur_x){
    buffer->cur_line[buffer->cur_x++] = '\0';
  }
}

void handleEscapeSequence(struct editorBuffer *buffer){
  if(!isInputAvailable()) return;
  char c = editorReadKey(buffer);
  if(c!='[') return;
  if(!isInputAvailable()) return;
  c = editorReadKey(buffer);
  switch (c) {
    case 'A':
      write(STDOUT_FILENO, "Arrow Up", 8);
      break;
    case 'B':
      write(STDOUT_FILENO, "Arrow Down", 10);
      break;
    case 'C':
      //TODO go to the next line if the last char of the line;
      if(buffer->cur_line[buffer->cur_x]=='\0') break;
      write(STDOUT_FILENO, "\x1b[1C", 4);
      buffer->cur_x++;
      break;
    case 'D':
      if(buffer->cur_x==0) break;
      write(STDOUT_FILENO, "\x1b[1D", 4);
      buffer->cur_x--;
      break;
    default:
      break;
  }
}

void editorProcessKeypress(struct editorBuffer *buffer)
  {
    char c = editorReadKey(buffer);


    switch(c)
      {
        case CTRL_KEY('q'):
          exit(0);
          break;
        case ('\r'):
          bufferWriteChar(buffer, c);
          bufferWriteChar(buffer, '\n');
          writeCurrentLineToBuffer(buffer);
          write(STDOUT_FILENO, "\x1b[1E", 4);
          buffer->cur_x=0;
          if(buffer->cur_y==buffer->numlines){
            buffer->numlines++;
          }
          buffer->cur_y++;
          break;
        case CTRL_KEY('p'):
          editorRefreshScreen();
          editorOutputBufferText(buffer);
          break;
        case DEL:
          deleteChar(buffer);
          writeCurrentLineToBuffer(buffer);
          editorRefreshCurrentLine(buffer);
          write(STDOUT_FILENO, "\b \b", 3);
          break;
        case BACKSPACE:
          deleteChar(buffer);
          writeCurrentLineToBuffer(buffer);
          editorRefreshCurrentLine(buffer);
          write(STDOUT_FILENO, "\b \b", 3);
          break;
        case ('\x1b'):
          handleEscapeSequence(buffer);
          break;
        default:
          //Только при дефолте происходит реальная запись
          bufferWriteChar(buffer, c);
          write(STDOUT_FILENO, &c, 1);
          break;
      }

  }


//INIT
int main(){
  enableRawMode();
  editorRefreshScreen();
  struct editorBuffer buffer = makeStringBuffer();
  while(1)
    {
      editorProcessKeypress(&buffer);
    }
  editorRefreshScreen();
  return 0;
}
