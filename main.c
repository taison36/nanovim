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
#include <sys/ioctl.h>

#define CTRL_KEY(k) ((k) & 0x1f)
#define MAXLINES 1000
#define SIZELINE 2000
#define DEL 127
#define BACKSPACE 8


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
  int x;
  int y;
  int wantedX;
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
static struct CursorCoordinates cursor = {1,1,1};

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

//CURSOR
void cursorRefresh(){
  char* x = makeStringFromInt(cursor.x);
  char* y = makeStringFromInt(cursor.y);
  int len =5+strlen(x) + strlen(y);
  char* str = malloc(len);

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

int getScreenLinesForString(const char* str) {
  int len = strlen(str);
  if (len >= 2 && str[len - 2] == '\r' && str[len - 1] == '\n') len -= 2;
  return (len / ws.terminalWidth) + (len % ws.terminalWidth > 0 ? 1 : 0);
}

void cursorGoUp(struct TextBuffer *buffer) {
  int goUp = 0;
  int maxX = ws.terminalWidth;

  // учёт текущей строки (строка ниже по экрану)
  goUp += cursor.x / maxX;

  // если на самой верхней строке — выход
  if (buffer->curY == 0) return;

  // получаем предыдущую строку
  char *prevLine = buffer->lines[buffer->curY - 1];
  int lenLine = strlen(prevLine);

  // исключаем \r\n из длины
  if (lenLine >= 2 && prevLine[lenLine - 2] == '\r' && prevLine[lenLine - 1] == '\n') {
    lenLine -= 2;
  }

  int targetX = lenLine < cursor.wantedX ? lenLine : cursor.wantedX;
  int screenLines = getScreenLinesForString(prevLine); // можно тоже обрезать здесь, если нужно

  goUp += screenLines - (targetX / maxX);

  cursor.y -= goUp;
  cursor.x = targetX;
}

void cursorGoDown(struct TextBuffer *buffer) {
  int goDown = 0;
  int maxX = ws.terminalWidth;

  // учёт текущей строки
  int screenLinesOfCurLine = getScreenLinesForString(buffer->curLine);
  int curYPosition = cursor.x/maxX + (cursor.x%maxX>0 ? 1 : 0);
  goDown += screenLinesOfCurLine - curYPosition;

  if (buffer->curY >= buffer->numlines || buffer->lines[buffer->curY] == NULL) {
    goDown += 1;
    cursor.x = 1;
  } else {
    char *nextLine = buffer->lines[buffer->curY];
    int lenLine = strlen(nextLine);

    // исключаем \r\n из длины
    if (lenLine >= 2 && nextLine[lenLine - 2] == '\r' && nextLine[lenLine - 1] == '\n') {
      lenLine -= 2;
    }

    int targetX = lenLine < cursor.wantedX ? lenLine : cursor.wantedX;
    int screenLines = getScreenLinesForString(nextLine); // можно тоже обрезать здесь, если нужно

    goDown += screenLines - (targetX / maxX);
    cursor.x = targetX;
  }

  cursor.y += goDown;
}

void cursorGoRight(struct TextBuffer *buffer){
  int len = strlen(buffer->curLine);
  // Только если курсор не в конце строки
  if (cursor.x <= len) {
    cursor.x++;
  }

  cursor.wantedX=cursor.x;
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

            if (appended + lineLength + 2 >= size) {
                size *= 2;
                screenBuffer = realloc(screenBuffer, size);
                if (!screenBuffer) die("editorPrepareBufferForScree: realloc failed");
            }

            memcpy(&screenBuffer[appended], &line[offset], lineLength);
            appended += lineLength;

            if(remain>maxX){
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
  //save cursor pos
  //write(STDOUT_FILENO, "\x1b[s", 3);

  //clear the terminal
  write(STDOUT_FILENO, "\x1b[2J", 4);
  //put cursor to the top
  write(STDOUT_FILENO, "\x1b[H", 3);

  char* screenBuffer = editorPrepareBufferForScreen(buffer);

  write(STDOUT_FILENO, screenBuffer, strlen(screenBuffer));

  //put the cursor back
  //write(STDOUT_FILENO, "\x1b[u", 3);
  free(screenBuffer);
}

void editorOutputBufferText(struct TextBuffer *buffer) {
  write(STDOUT_FILENO, "p pressed!", 10);
  sleep(1);
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
}


//INPUT

void bufferPutCurLine(struct TextBuffer *buffer){
  copyLine(buffer->curLine, buffer->lines[buffer->curY]);
}

void curLineDeleteChar(struct TextBuffer *buffer){
  if(buffer->curX==0) return;
  moveCharsLeft(buffer->curLine, buffer->curX, 1);
  buffer->curX--;
}


void bufferWriteCurrentLine(struct TextBuffer *buffer){
  int size = strlen(buffer->curLine);

  if(buffer->lines[buffer->curY] != NULL) free(buffer->lines[buffer->curY]);
  buffer->lines[buffer->curY] = malloc(size+1);

  if (buffer->lines[buffer->curY] == NULL) {
    die("writeCurrentLineToBuffer: malloc failed");
  }
  //check I am at the last line
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
  //после каждого записанного char ставить в конце \0
  //перед записью нового символа вычислять длину строки и тем самым определять находиться ли cur_x в конце
  //если в конце, то просто писать дальше
  //если нет, двигать поинеты вперед и писать между ними
  if (buffer->curX > SIZELINE - 1) die("bufferWriteChar: SIZELINE is exceeded");

  int len = strlen(buffer->curLine);


  buffer->curLine[buffer->curX] = c;

  if(len == buffer->curX){
    buffer->curLine[buffer->curX+1] = '\0';
  }
  buffer->curX++;
}

void bufferHandleIncreaseCurY(struct TextBuffer *buffer){
  if(buffer->curY==buffer->numlines){
    buffer->numlines++;
  }
  buffer->curY++;
}
void curLineClear(struct TextBuffer *buffer){
  buffer->curX=0;
  buffer->curLine[buffer->curX] = '\0'; // clear current line;
}

void bufferHandleEditorNextLine(struct TextBuffer *buffer){
  curLineWriteChar(buffer, '\r');
  curLineWriteChar(buffer, '\n');
  bufferWriteCurrentLine(buffer);

  bufferHandleIncreaseCurY(buffer);
  cursorGoDown(buffer);
  curLineClear(buffer);
}

void bufferHandleEscapeSequence(struct TextBuffer *buffer){
  if(!isInputAvailable()) return;
  char c = editorReadKey();
  if(c!='[') return;
  if(!isInputAvailable()) return;
  c = editorReadKey();
  switch (c) {
    case 'A':
      if(buffer->curY == 0) return;
      cursorGoUp(buffer);
      buffer->curY--;
      bufferPutCurLine(buffer);
      break;
    case 'B':
      if(buffer->curY == buffer->numlines) return;
      cursorGoDown(buffer);
      buffer->curY++;
      bufferPutCurLine(buffer);
      break;
    case 'C':
      cursorGoRight(buffer);
      break;
    case 'D':
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
          bufferHandleEditorNextLine(buffer);
          break;
        case ('\n'):
          bufferHandleEditorNextLine(buffer);
          break;
        case CTRL_KEY('p'):
          editorOutputBufferText(buffer);
          break;
        case DEL:
          curLineDeleteChar(buffer);
          bufferWriteCurrentLine(buffer);
          break;
        case BACKSPACE:
          curLineDeleteChar(buffer);
          bufferWriteCurrentLine(buffer);
          break;
        case ('\x1b'):
          bufferHandleEscapeSequence(buffer);
          break;
        default:
          //Только при дефолте происходит реальная запись
          curLineWriteChar(buffer, c);
          bufferWriteCurrentLine(buffer);
          cursorGoRight(buffer);
          break;
      }
    editorRefreshScreen(buffer);
    cursorRefresh();
  }


//INIT
int main(){
  enableRawMode();
  struct TextBuffer buffer = textBuffer();
  ws = windowSettings();
  cursor.y=ws.topOffset+1;
  editorRefreshScreen(&buffer);
  while(1)
    {
      editorProcessKeypress(&buffer);
    }
  return 0;
}
