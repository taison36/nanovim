#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <poll.h> // Required for poll
#include <errno.h>  // Required for errno

#define CTRL_KEY(k) ((k) & 0x1f)
#define MAXLINES 1000
#define SIZELINE 2000
#define DEL 127
#define BACKSPACE 8

#define MIN(a, b) ((a) < (b) ? (a) : (b))

struct termios orig_termios;

struct TextBuffer{
  char *lines[MAXLINES];
  int numlines;
  int curX;
  int curY;
  char curLine[SIZELINE];
};

struct WindowSettings{
  int terminalWidth;
  int terminalHeight;
  int topOffset;
  int bottomOffset;
};

struct CursorCoordinates{
  int screenX;
  int screenY;
  int logicalWantedX;
};


struct TextBuffer textBuffer(){
  struct TextBuffer buffer;
  buffer.curX=0;
  buffer.curY=0;
  buffer.numlines=0;
  for(int i = 0; i<MAXLINES; i++){
    buffer.lines[i] = NULL;
  }
  *buffer.curLine='\0';
  return buffer;
}

struct WindowSettings windowSettings(){
  struct WindowSettings ws;
  struct winsize w;
  if(ioctl(STDOUT_FILENO, TIOCGWINSZ, &w)==-1){
    perror("ioctl");
    exit(1);
  }

  ws.terminalWidth = w.ws_col;
  ws.terminalHeight = w.ws_row;
  ws.topOffset = 0;
  ws.bottomOffset = 0;
  return ws;
}

static struct WindowSettings ws;
static struct CursorCoordinates cursor = {1,1,0};

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

void dbgPrint(char *s){
  write(STDOUT_FILENO, s, strlen(s));
  sleep(1);
}

void printInt(int x) {
  char buf[32];
  int len = snprintf(buf, sizeof(buf), "%d\n", x);
  write(STDOUT_FILENO, buf, len);
  sleep(1);
}

void copyLine(char *to, char *from) {
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

char* makeStringFromInt(int n){
  char* str = malloc(12);
  if(!str) die("makeStringFromInt: malloc failed");
  snprintf(str,12, "%d", n);
  return str;
}

int getScreenLinesForString(const char* str) {
    if (str == NULL) {
        return 1;
    }
    int len = strlen(str);
    int stringLength = len;

    if (stringLength >= 2 && str[stringLength - 2] == '\r' && str[stringLength - 1] == '\n') {
        stringLength -= 2;
    } else if (stringLength >= 1 && (str[stringLength - 1] == '\r' || str[stringLength - 1] == '\n')) {
        stringLength -=1;
    }

    if (stringLength == 0) {
        return 1;
    }

    if (ws.terminalWidth == 0) return 1; // Защита от деления на ноль
    return (stringLength / ws.terminalWidth) + ((stringLength % ws.terminalWidth != 0) ? 1 : 0);
}

//CURSOR

void bufferLoadCurLine(struct TextBuffer *buffer); // Forward declaration
void curLineClearAndResetX(struct TextBuffer *buffer);   // Forward declaration
void curLineWriteChar(struct TextBuffer *buffer, char c);
void bufferWriteCurrentLine(struct TextBuffer *buffer);
void moveCursorDown(struct TextBuffer *buffer);
void updateScreenCursorCoordinates(struct TextBuffer *buffer) {
    cursor.screenY = ws.topOffset + 1;

    for (int i = 0; i < buffer->curY; i++) {
        if (i < buffer->numlines && buffer->lines[i] != NULL) {
            cursor.screenY += getScreenLinesForString(buffer->lines[i]);
        } else {
            cursor.screenY += 1;
        }
    }

    if (ws.terminalWidth > 0) {
        cursor.screenX = (buffer->curX % ws.terminalWidth) + 1; // 1-based column
        cursor.screenY += (buffer->curX / ws.terminalWidth);
    } else {
        cursor.screenX = buffer->curX + 1;
    }
}

void cursorRefresh(){
  char* x = makeStringFromInt(cursor.screenX);
  char* y = makeStringFromInt(cursor.screenY);
  int len = 5 + strlen(x) + strlen(y);
  char* str = malloc(len + 1);

  if(!str){
    die("cursorPut: malloc failed");
  }

  strcpy(str, "\x1b[");
  strcat(str, y);
  strcat(str, ";");
  strcat(str, x);
  strcat(str, "H");

  write(STDOUT_FILENO, str, len);

  free(y);
  free(x);
  free(str);
}

void moveCursorRight(struct TextBuffer *buffer) {
    int stringLength = strlen(buffer->curLine);

    if (stringLength >= 2 && buffer->curLine[stringLength - 2] == '\r' && buffer->curLine[stringLength - 1] == '\n') {
        stringLength -= 2;
    } else if (stringLength >= 1 && (buffer->curLine[stringLength - 1] == '\r' || buffer->curLine[stringLength - 1] == '\n')) {
        stringLength -=1;
    }

    if (buffer->curX < stringLength-1) {
      // Cursor is not at the end of the text on the current line
      buffer->curX++;
    } else if (buffer->curY < buffer->numlines) {
        bufferWriteCurrentLine(buffer);
        moveCursorDown(buffer);
    }
    cursor.logicalWantedX = buffer->curX;
}

void moveCursorLeft(struct TextBuffer *buffer) {
    if (buffer->curX > 0) {
        buffer->curX--;
    } else if (buffer->curY > 0) {
        bufferWriteCurrentLine(buffer);
        buffer->curY--;
        bufferLoadCurLine(buffer);
        int prevLineLen = strlen(buffer->curLine);
        if (prevLineLen >= 2 && buffer->curLine[prevLineLen - 2] == '\r' && buffer->curLine[prevLineLen - 1] == '\n') {
            prevLineLen -= 2;
        } else if (prevLineLen >= 1 && (buffer->curLine[prevLineLen - 1] == '\r' || buffer->curLine[prevLineLen - 1] == '\n')) {
            prevLineLen -=1;
        }
        buffer->curX = prevLineLen;
    }
    cursor.logicalWantedX = buffer->curX;
}

void moveCursorUp(struct TextBuffer *buffer) {
    if (buffer->curY > 0) {
        bufferWriteCurrentLine(buffer);
        buffer->curY--;
        bufferLoadCurLine(buffer);
        int lineLen = strlen(buffer->curLine);
        if (lineLen >= 2 && buffer->curLine[lineLen - 2] == '\r' && buffer->curLine[lineLen - 1] == '\n') {
            lineLen -= 2;
        } else if (lineLen >= 1 && (buffer->curLine[lineLen - 1] == '\r' || buffer->curLine[lineLen - 1] == '\n')) {
          lineLen -=1;
        }
        buffer->curX = MIN(lineLen, cursor.logicalWantedX);
    }
}

void moveCursorDown(struct TextBuffer *buffer) {
    if (buffer->curY < buffer->numlines) {
        bufferWriteCurrentLine(buffer);

        buffer->curY++;

        if(buffer->curY == buffer->numlines){

            buffer->curY--;

            bufferLoadCurLine(buffer);

            int len = strlen(buffer->curLine);
            if(!(len >= 2 && buffer->curLine[len-2] == '\r' && buffer->curLine[len-1]== '\n')){
                buffer->curX = len;
                curLineWriteChar(buffer, '\r');
                curLineWriteChar(buffer, '\n');
                bufferWriteCurrentLine(buffer);
            }

            buffer->curY++;
            curLineClearAndResetX(buffer);
        } else {
            bufferLoadCurLine(buffer);
        }

        int nextLineLen = strlen(buffer->curLine);
        int navigationLen = nextLineLen;
        if (buffer->curY < buffer->numlines) {
            if (navigationLen >= 2 && buffer->curLine[navigationLen - 2] == '\r' && buffer->curLine[navigationLen - 1] == '\n') {
                navigationLen -= 2;
            } else if (navigationLen >=1 && (buffer->curLine[navigationLen-1] == '\r' || buffer->curLine[navigationLen-1] == '\n')) {
                navigationLen -=1;
            }
        }
        buffer->curX = MIN(navigationLen, cursor.logicalWantedX);
    }
}

// OUTPUT
//TODO Добавить логику, когда есть скроллинг. То есть показывать не buffer->lines[i], а со строки ниже где находишься
char* editorPrepareBufferForScreen(struct TextBuffer *buffer) {
  int size = 2500;
  char* screenBuffer = malloc(size);
  if (!screenBuffer) die("editorPrepareBufferForScreen: malloc failed");
  int appended = 0;

  int maxX = ws.terminalWidth;
  int maxY = ws.terminalHeight - (ws.bottomOffset + ws.topOffset);
  int shownY = 0;

  for (int i = 0; i <= buffer->numlines && shownY < maxY; i++) {
    char* line = buffer->lines[i];
    if(line==NULL) continue;
    int len = strlen(line); //15
    int offset = 0; // 0

    while (offset < len && shownY < maxY) {
        int remain = len - offset; //15
        int lineLength = (remain > maxX) ? maxX : remain; //10

        if (appended + lineLength + 2 >= size) { // +2 for potential \r\n
            size *= 2;
            screenBuffer = realloc(screenBuffer, size);
            if (!screenBuffer) die("editorPrepareBufferForScree: realloc failed");
        }

        memcpy(&screenBuffer[appended], &line[offset], lineLength);
        appended += lineLength;

        if(remain > maxX){ // If the line was wrapped, add \r\n for display
          screenBuffer[appended++] = '\r';
          screenBuffer[appended++] = '\n';
        }

        offset += lineLength;
        shownY++;
    }
  }

  screenBuffer[appended] = '\0';
  return screenBuffer;
}


void editorRefreshScreen(struct TextBuffer *buffer) {
  //clear the terminal
  write(STDOUT_FILENO, "\x1b[2J", 4);
  //put cursor to the top
  write(STDOUT_FILENO, "\x1b[H", 3);

  char* screenBuffer = editorPrepareBufferForScreen(buffer);

  write(STDOUT_FILENO, screenBuffer, strlen(screenBuffer));

  free(screenBuffer);
}

void editorOutputBufferText(struct TextBuffer *buffer) {
  //clear the terminal
  write(STDOUT_FILENO, "\x1b[2J", 4);
  //put cursor to the top
  write(STDOUT_FILENO, "\x1b[H", 3);
  for (int i = 0; i < buffer->numlines; i++) {
    char *str = buffer->lines[i];
    while (*str != '\0') {
      write(STDOUT_FILENO, str, 1);
      str++;
    }
  }
  write(STDOUT_FILENO, "p pressed!", 10);
  sleep(1);
}


//INPUT

void bufferLoadCurLine(struct TextBuffer *buffer){
  // If curY points to a valid line in the buffer, copy it.
  // Otherwise (e.g., curY is numlines, the virtual line), curLine should be empty.
  if (buffer->curY < buffer->numlines && buffer->lines[buffer->curY] != NULL) {
    copyLine(buffer->curLine, buffer->lines[buffer->curY]);
  } else {
    curLineClearAndResetX(buffer); // Ensure curLine is empty for new or non-existent lines
  }
}

void curLineDeleteChar(struct TextBuffer *buffer){
  if(buffer->curX==0) return;
  moveCharsLeft(buffer->curLine, buffer->curX, 1);
  buffer->curX--;
}


void bufferWriteCurrentLine(struct TextBuffer *buffer){
  // Ensure curY is within the bounds of the lines array
  if (buffer->curY >= MAXLINES-1) {
      die("bufferWriteCurrentLine: Exceeded MAXLINES, cannot write line.");
  }

  int size = strlen(buffer->curLine);

  if(buffer->lines[buffer->curY] != NULL) {
      free(buffer->lines[buffer->curY]); // Free existing memory
  }
  buffer->lines[buffer->curY] = malloc(size+1);

  if (buffer->lines[buffer->curY] == NULL) {
    die("writeCurrentLineToBuffer: malloc failed");
  }
  copyLine(buffer->lines[buffer->curY], buffer->curLine);
  //TODO if its not the last line of the file, we need to move the rest of the lines in the buffer to be able to make the space
}

char editorReadKey(){
  char nread; // output result code
  char c;
  while((nread = read(STDIN_FILENO, &c, 1)) != 1 ){
    if(nread == -1 && errno != EAGAIN) die("read");
  }
  return c;
}

void curLineWriteChar(struct TextBuffer *buffer, char c){
  if (buffer->curX >= SIZELINE - 1) die("bufferWriteChar: SIZELINE is exceeded"); // -1 for null terminator

  int len = strlen(buffer->curLine);

  if (buffer->curX < len) {
      moveCharsRight(buffer->curLine, buffer->curX, 1);
  }

  buffer->curLine[buffer->curX] = c;

  if (buffer->curX == len) {
      buffer->curLine[buffer->curX + 1] = '\0';
  }
  buffer->curX++;
}

void curLineClearAndResetX(struct TextBuffer *buffer){
  buffer->curX=0;
  buffer->curLine[buffer->curX] = '\0'; // clear current line;
}


void bufferHandleEscapeSequence(struct TextBuffer *buffer){
  if(!isInputAvailable()) return;
  char c = editorReadKey();
  if(c!='[') return;
  if(!isInputAvailable()) return;
  c = editorReadKey();
  switch (c) {
    case 'A':
      moveCursorUp(buffer);
      break;
    case 'B':
      moveCursorDown(buffer);
      break;
    case 'C':
      moveCursorRight(buffer);
      break;
    case 'D':
      moveCursorLeft(buffer);
      break;
    default:
      break;
  }
}

void editorProcessKeypress(struct TextBuffer *buffer)
  {
    char c = editorReadKey();


    switch(c)
      {
        case CTRL_KEY('q'):
          exit(0);
          break;
        case ('\r'):
        case ('\n'):
          if (buffer->curY == buffer->numlines) {
            buffer->numlines++;
          }
          curLineWriteChar(buffer, '\r');
          curLineWriteChar(buffer, '\n');
          bufferWriteCurrentLine(buffer);
          curLineClearAndResetX(buffer);
          buffer->curY++;
          cursor.logicalWantedX=buffer->curX;
          break;
        case CTRL_KEY('p'):
          editorOutputBufferText(buffer);
          break;
        case DEL:
        case BACKSPACE:
          curLineDeleteChar(buffer);
          bufferWriteCurrentLine(buffer);
          break;
        case ('\x1b'):
          bufferHandleEscapeSequence(buffer);
          break;
        default:
          if (buffer->curY == buffer->numlines) {
            buffer->numlines++;
          }
          curLineWriteChar(buffer, c);
          bufferWriteCurrentLine(buffer);
          cursor.logicalWantedX=buffer->curX;
          break;
      }

    editorRefreshScreen(buffer);
    updateScreenCursorCoordinates(buffer);
    cursorRefresh();
  }


//INIT
int main(){
  enableRawMode();
  struct TextBuffer buffer = textBuffer();
  ws = windowSettings();
  cursor.screenY=ws.topOffset+1; // Initial cursor Y position
  editorRefreshScreen(&buffer);
  while(1)
    {
      editorProcessKeypress(&buffer);
    }
  return 0;
}
