#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <poll.h>
#include <errno.h>

#define CTRL_KEY(k) ((k) & 0x1f)
#define SIZELINE 2000
#define DEL 127
#define BACKSPACE 8
#define INITIAL_LINES_CAPACITY 10

#define MIN(a, b) ((a) < (b) ? (a) : (b))

struct termios orig_termios;

// Forward declarationsstruct TextBuffer;
struct WindowSettings;
struct CursorCoordinates;
struct TextBuffer;

void bufferLoadCurLine(struct TextBuffer *buffer);
void curLineClearAndResetX(struct TextBuffer *buffer);
void curLineWriteChar(struct TextBuffer *buffer, char c);
void bufferSaveCurrentLine(struct TextBuffer *buffer);
void updateScreenCursorCoordinates(struct TextBuffer *buffer, struct WindowSettings *ws, struct CursorCoordinates *cursor);
void editorRefreshScreen(struct TextBuffer *buffer, struct WindowSettings *ws);
void cursorRefresh(struct CursorCoordinates *cursor);
void freeTextBuffer(struct TextBuffer *buffer);
void moveCursorDown(struct TextBuffer *buffer, struct CursorCoordinates *cursor);
void die(const char *s);
void cleanEditor();
void curLineWriteChars(struct TextBuffer *buffer,const char* chars);

struct TextBuffer {
    char **lines;
    int numlines;
    int linesCapacity;
    int curX;
    int curY;
    char curLine[SIZELINE];
};

struct WindowSettings {
    int terminalWidth;
    int terminalHeight;
    int topOffset;
    int bottomOffset;
};

struct CursorCoordinates {
    int screenX;
    int screenY;
    int logicalWantedX;
};

static struct TextBuffer global_buffer_for_cleanup;
static int global_buffer_initialized = 0;


struct TextBuffer textBufferInit() {
    struct TextBuffer buffer;
    buffer.curX = 0;
    buffer.curY = 0;
    buffer.numlines = 0;
    buffer.linesCapacity = INITIAL_LINES_CAPACITY;
    buffer.lines = malloc(buffer.linesCapacity * sizeof(char *));
    if (buffer.lines == NULL) {
        die("textBufferInit: malloc for lines failed");
    }
    for (int i = 0; i < buffer.linesCapacity; i++) {
        buffer.lines[i] = NULL;
    }
    buffer.curLine[0] = '\0';

    // For atexit cleanup
    global_buffer_for_cleanup = buffer;
    global_buffer_initialized = 1;
    return buffer;
}

struct WindowSettings windowSettingsInit() {
    struct WindowSettings ws_local;
    struct winsize w;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == -1) {
        perror("ioctl for windowSettingsInit");
        exit(1); // Cannot proceed without terminal size
    }
    ws_local.terminalWidth = w.ws_col;
    ws_local.terminalHeight = w.ws_row;
    ws_local.topOffset = 0;
    ws_local.bottomOffset = 0; // NOTE for status bar later
    return ws_local;
}

// TERMINAL
void die(const char *s){
  cleanEditor();
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
void cleanEditor(){
  if(global_buffer_initialized){
    freeTextBuffer(&global_buffer_for_cleanup);
    global_buffer_initialized=0;
  }
}

void freeTextBuffer(struct TextBuffer *buffer) {
    if (buffer == NULL || buffer->lines == NULL) return;
    for (int i = 0; i < buffer->numlines; i++) {
        if (buffer->lines[i] != NULL) {
            free(buffer->lines[i]);
            buffer->lines[i] = NULL;
        }
    }
    for (int i = buffer->numlines; i < buffer->linesCapacity; i++) {
        if (buffer->lines[i] != NULL) {
             free(buffer->lines[i]);
             buffer->lines[i] = NULL;
        }
    }
    free(buffer->lines);
    buffer->lines = NULL;
    buffer->numlines = 0;
    buffer->linesCapacity = 0;
}

void copyLine(char *from, char *to) {
  if(from==NULL){
    *to='\0';
    return;
  }

  while (*from != '\0') {
    *to++ = *from++;
  }
  *to='\0';
}

char* appendTwoLines(const char *str1, const char *str2) {
    if (str1 == NULL) str1 = "";
    if (str2 == NULL) str2 = "";

    size_t len_str1 = strlen(str1);
    size_t len_str2 = strlen(str2);
    size_t total_len = len_str1 + len_str2 + 1; // +1 for the final null terminator

    char *new_line = malloc(total_len);

    if (new_line == NULL) {
        die("appendTwoLines: malloc failed");
    }

    memcpy(new_line, str1, len_str1);

    memcpy(new_line + len_str1, str2, len_str2);

    new_line[total_len - 1] = '\0';

    return new_line;
}

int isInputAvailable() {
  struct pollfd pfd;
  pfd.fd = STDIN_FILENO;
  pfd.events = POLLIN;
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

  if(cur_pos < n) return;

  for(int i = cur_pos; i <= len; i++) {
      str[i - n] = str[i];
  }
}

void moveRowsUp(char **lines,int num_lines, int row_to_delete){
  // so cur_row disappears
  if(lines==NULL || row_to_delete < 0) return;

  for(int i = row_to_delete; i<=num_lines; i++){
    lines[i] = lines[i+1];
  }
}

char* makeStringFromInt(int n){
  char* str = malloc(12);
  if(!str) die("makeStringFromInt: malloc failed");
  snprintf(str,12, "%d", n);
  return str;
}

int getScreenLinesForString(const char* str, struct WindowSettings *ws) {
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

    if (ws->terminalWidth == 0) return 1;
    return (stringLength / ws->terminalWidth) + ((stringLength % ws->terminalWidth != 0) ? 1 : 0);
}

// DYNAMIC ARRAY MANAGEMENT for buffer->lines
void editorEnsureLineCapacity(struct TextBuffer *buffer, int required_idx) {
    if (required_idx >= buffer->linesCapacity) {
        int newCapacity = buffer->linesCapacity == 0 ? INITIAL_LINES_CAPACITY : buffer->linesCapacity * 2;
        while (newCapacity <= required_idx) {
            newCapacity *= 2;
        }
        char **new_lines = realloc(buffer->lines, newCapacity * sizeof(char *));
        if (!new_lines) die("editorEnsureLineCapacity: realloc lines failed");

        for (int i = buffer->linesCapacity; i < newCapacity; i++) {
            new_lines[i] = NULL;
        }
        buffer->lines = new_lines;
        buffer->linesCapacity = newCapacity;
    }
}

//CURSOR
void updateScreenCursorCoordinates(struct TextBuffer *buffer,  struct WindowSettings *ws, struct CursorCoordinates *cursor){
    cursor->screenY = ws->topOffset + 1;

    for (int i = 0; i < buffer->curY; i++) {
        if (i < buffer->numlines && buffer->lines[i] != NULL) {
            cursor->screenY += getScreenLinesForString(buffer->lines[i], ws);
        } else {
            cursor->screenY += 1;
        }
    }

    if (ws->terminalWidth > 0) {
        cursor->screenX = (buffer->curX % ws->terminalWidth) + 1;
        cursor->screenY += (buffer->curX / ws->terminalWidth);
    } else {
        cursor->screenX = buffer->curX + 1;
    }
}

void cursorRefresh(struct CursorCoordinates *cursor){
  char* x = makeStringFromInt(cursor->screenX);
  char* y = makeStringFromInt(cursor->screenY);
  int len = 5 + strlen(x) + strlen(y);
  char* str = malloc(len + 1);

  if(!str){
    free(x);
    free(y);
    die("cursorRefresh: malloc failed");
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

void moveCursorRight(struct TextBuffer *buffer, struct CursorCoordinates *cursor) {
    int stringLength = strlen(buffer->curLine);

    if (stringLength >= 2 && buffer->curLine[stringLength - 2] == '\r' && buffer->curLine[stringLength - 1] == '\n') {
        stringLength -= 2;
    } else if (stringLength >= 1 && (buffer->curLine[stringLength - 1] == '\r' || buffer->curLine[stringLength - 1] == '\n')) {
        stringLength -=1;
    }

    if (buffer->curX < stringLength) {
      buffer->curX++;
    } else if (buffer->curY < buffer->numlines) {
        bufferSaveCurrentLine(buffer);
        moveCursorDown(buffer, cursor);
    }
    cursor->logicalWantedX = buffer->curX;
}

void moveCursorLeft(struct TextBuffer *buffer, struct CursorCoordinates *cursor) {
    if (buffer->curX > 0) {
        buffer->curX--;
    } else if (buffer->curY > 0) {
        bufferSaveCurrentLine(buffer);
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
    cursor->logicalWantedX = buffer->curX;
}

void moveCursorUp(struct TextBuffer *buffer, struct CursorCoordinates *cursor) {
    if (buffer->curY == 0) return;

    bufferSaveCurrentLine(buffer);

    buffer->curY--;

    bufferLoadCurLine(buffer);

    int lineLen = strlen(buffer->curLine);
    if (lineLen >= 2 && buffer->curLine[lineLen - 2] == '\r' && buffer->curLine[lineLen - 1] == '\n'){
      lineLen -= 2;
    }else if(lineLen >= 1 && (buffer->curLine[lineLen - 1] == '\r' || buffer->curLine[lineLen - 1] == '\n')){
      lineLen -=1;
    }

    buffer->curX = MIN(lineLen, cursor->logicalWantedX);
}

void moveCursorDown(struct TextBuffer *buffer, struct CursorCoordinates *cursor) {
    if (buffer->curY < buffer->numlines) {
        bufferSaveCurrentLine(buffer);

        buffer->curY++;

        //Hitting the virtual line
        if(buffer->curY == buffer->numlines){

            buffer->curY--;

            bufferLoadCurLine(buffer);

            int len = strlen(buffer->curLine);
            // i need to handle go to the virtual line also as the 'enter' press. so put to the of the line \r\n;
            if(!(len >= 2 && buffer->curLine[len-2] == '\r' && buffer->curLine[len-1]== '\n')){
                buffer->curX = len;
                curLineWriteChar(buffer, '\r');
                curLineWriteChar(buffer, '\n');
                bufferSaveCurrentLine(buffer);
            }

            buffer->curY++;
            curLineClearAndResetX(buffer);
        } else {
            bufferLoadCurLine(buffer);
        }

        int lenCurrentLine = strlen(buffer->curLine);
        if (buffer->curY < buffer->numlines) {
            if (lenCurrentLine >= 2 && buffer->curLine[lenCurrentLine - 2] == '\r' && buffer->curLine[lenCurrentLine - 1] == '\n') {
                lenCurrentLine -= 2;
            } else if (lenCurrentLine >=1 && (buffer->curLine[lenCurrentLine-1] == '\r' || buffer->curLine[lenCurrentLine-1] == '\n')) {
                lenCurrentLine -=1;
            }
        }
        buffer->curX = MIN(lenCurrentLine, cursor->logicalWantedX);
    }
}

// OUTPUT
//TODO Добавить логику, когда есть скроллинг. То есть показывать не buffer->lines[i], а со строки ниже где находишься
char* editorPrepareBufferForScreen(struct TextBuffer *buffer, struct WindowSettings *ws) {
  int size = 2500;
  char* screenBuffer = malloc(size);
  if (!screenBuffer) die("editorPrepareBufferForScreen: malloc failed");
  int appended = 0;

  int maxX = ws->terminalWidth;
  int maxY = ws->terminalHeight - (ws->bottomOffset + ws->topOffset);
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


void editorRefreshScreen(struct TextBuffer *buffer, struct WindowSettings *ws) {
  //clear the terminal
  write(STDOUT_FILENO, "\x1b[2J", 4);
  //put cursor to the top
  write(STDOUT_FILENO, "\x1b[H", 3);

  char* screenBuffer = editorPrepareBufferForScreen(buffer, ws);

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
  if (buffer->curY < buffer->numlines) {
    copyLine(buffer->lines[buffer->curY], buffer->curLine);
  } else {
    //die("bufferLoadCurLine: trying to add in the buffer a line, that doesnt exist");
    curLineClearAndResetX(buffer);
  }
}

void curLineDeleteChar(struct TextBuffer *buffer, struct CursorCoordinates *cursor){
  if(buffer->curX==0 && buffer->curY > 0){
    bufferSaveCurrentLine(buffer);

    char* prev_str = buffer->lines[buffer->curY-1];
    int len_prev_str = strlen(prev_str);

    if(len_prev_str >= 2 && prev_str[len_prev_str-2] == '\r' && prev_str[len_prev_str-1] == '\n'){
      prev_str[len_prev_str-2] = '\0';
      len_prev_str-=2;
    }else{
      prev_str[len_prev_str-1] = '\0';
      len_prev_str-=1;
    }

    char* appended_line = appendTwoLines(buffer->lines[buffer->curY-1], buffer->lines[buffer->curY]);
    if(appended_line==NULL){
      die("curLineDeleteChar: appending of two lines is failed");
    }

    free(buffer->lines[buffer->curY]); // ЧТО-ТО неправильно сдвинул
    moveRowsUp(buffer->lines, buffer->numlines, buffer->curY);
    //free(buffer->lines[buffer->curY-1]);

    buffer->curY--;
    curLineClearAndResetX(buffer);
    curLineWriteChars(buffer,appended_line);
    free(appended_line);

    bufferSaveCurrentLine(buffer);
    buffer->numlines--;

    buffer->curX = len_prev_str;
    cursor->logicalWantedX=buffer->curX;
  }else if(buffer->curX > 0){
    moveCharsLeft(buffer->curLine, buffer->curX, 1);
    buffer->curX--;
  }
}


void bufferSaveCurrentLine(struct TextBuffer *buffer){
  editorEnsureLineCapacity(buffer, buffer->curY);

  int size = strlen(buffer->curLine);

  if(buffer->lines[buffer->curY] != NULL) {
      free(buffer->lines[buffer->curY]); // Free existing memory
  }
  buffer->lines[buffer->curY] = malloc(size+1);

  if (buffer->lines[buffer->curY] == NULL) {
    die("writeCurrentLineToBuffer: malloc failed");
  }
  copyLine(buffer->curLine, buffer->lines[buffer->curY]);
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
  if (buffer->curX >= SIZELINE - 1) die("bufferWriteChar: SIZELINE is exceeded");

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

void curLineWriteChars(struct TextBuffer *buffer, const char* chars) {
    size_t add_len = strlen(chars);
    if (add_len == 0) {
        return; // Nothing to do.
    }

    size_t current_len = strlen(buffer->curLine);

    if (current_len + add_len >= SIZELINE) {
        die("curLineWriteChars: New text exceeds SIZELINE limit");
    }

    if (buffer->curX < (int)current_len) {
        moveCharsRight(buffer->curLine, buffer->curX, add_len);
    }

    memcpy(&buffer->curLine[buffer->curX], chars, add_len);

    buffer->curX += add_len;
}

void curLineClearAndResetX(struct TextBuffer *buffer){
  buffer->curX=0;
  buffer->curLine[buffer->curX] = '\0'; // clear current line;
}


void bufferHandleEscapeSequence(struct TextBuffer *buffer, struct CursorCoordinates *cursor){
  if(!isInputAvailable()) return;
  char c = editorReadKey();
  if(c!='[') return;
  if(!isInputAvailable()) return;
  c = editorReadKey();
  switch (c) {
    case 'A':
      moveCursorUp(buffer, cursor);
      break;
    case 'B':
      moveCursorDown(buffer, cursor);
      break;
    case 'C':
      moveCursorRight(buffer, cursor);
      break;
    case 'D':
      moveCursorLeft(buffer, cursor);
      break;
  }
}

void editorProcessKeypress(struct TextBuffer *buffer, struct WindowSettings *ws, struct CursorCoordinates *cursor)
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
          bufferSaveCurrentLine(buffer);
          curLineClearAndResetX(buffer);
          buffer->curY++;
          cursor->logicalWantedX=buffer->curX;
          break;
        case CTRL_KEY('p'):
          editorOutputBufferText(buffer);
          break;
        case DEL:
        case BACKSPACE:
          curLineDeleteChar(buffer, cursor);
          bufferSaveCurrentLine(buffer);
          break;
        case ('\x1b'):
          bufferHandleEscapeSequence(buffer, cursor);
          break;
        default:
          if (buffer->curY == buffer->numlines) {
            buffer->numlines++;
          }
          curLineWriteChar(buffer, c);
          bufferSaveCurrentLine(buffer);
          cursor->logicalWantedX=buffer->curX;
          break;
      }

    editorRefreshScreen(buffer, ws);
    updateScreenCursorCoordinates(buffer, ws, cursor);
    cursorRefresh(cursor);
  }


//INIT
int main(){
  enableRawMode();
  struct TextBuffer buffer = textBufferInit();
  struct WindowSettings ws = windowSettingsInit();
  struct CursorCoordinates cursor = {1,1,0};
  cursor.screenY=ws.topOffset+1; // Initial cursor Y position
  editorRefreshScreen(&buffer, &ws);
  while(1)
    {
      editorProcessKeypress(&buffer, &ws, &cursor);
    }
  cleanEditor();
  return 0;
}
