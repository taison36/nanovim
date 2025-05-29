#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/_types/_ucontext.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#define CTRL_KEY(k) ((k) & 0x1f)
#define SIZELINE 2000
#define DEL 127
#define BACKSPACE 8
#define INITIAL_LINES_CAPACITY 10

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))

struct termios orig_termios;

// Forward declarationsstruct TextBuffer;
struct WindowSettings;
struct ScreenSettings;
struct TextBuffer;

void bufferLoadCurLine(struct TextBuffer *buffer);
void curLineClearAndResetX(struct TextBuffer *buffer);
void curLineWriteChar(struct TextBuffer *buffer, char c);
void bufferSaveCurrentLine(struct TextBuffer *buffer);
void editorUpdateCursorCoordinates(struct TextBuffer *buffer,
                                   struct WindowSettings *ws,
                                   struct ScreenSettings *screen_settings);
void editorRefreshScreen(struct TextBuffer *buffer, struct WindowSettings *ws, struct ScreenSettings *screen_settings);
void editorRefreshCursor(struct ScreenSettings *screen_settings);
void freeTextBuffer(struct TextBuffer *buffer);
void moveCursorDown(struct TextBuffer *buffer,
                    struct ScreenSettings *screen_settings);
void die(const char *s);
void cleanEditor();
void curLineWriteChars(struct TextBuffer *buffer, const char *chars);
void bufferHandleNewLineInput(struct TextBuffer *buffer,
                              struct ScreenSettings *screen_settings);
int countNewLineChars(const char *str);
void editorEnsureLineCapacity(struct TextBuffer *buffer, int required_idx);
char *addNewLineChar(char *str);

// INIT
struct TextBuffer {
  char **lines;
  int numlines;
  int linesCapacity;
  int curX;
  int curY;
  char curLine[SIZELINE];
};

struct WindowSettings {
  int terminal_width;
  int terminal_height;
  int top_offset;
  int bottom_offset;
  int left_offset;
  int max_x;
  int max_y;
};

struct ScreenSettings {
  int cursor_x;
  int cursor_y;
  int logical_wanted_x;
  int first_textbuffer_line_to_print;
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
  struct WindowSettings ws;

  ws.top_offset = 0;
  ws.bottom_offset = 0; // NOTE for status bar later
  ws.left_offset = 0;

  struct winsize w;
  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == -1) {
    die("windowSettingsInit: ioctl. Cannot proceed without terminal size");
  }
  ws.terminal_width = w.ws_col - ws.left_offset;
  ws.terminal_height = w.ws_row;
  ws.max_x = ws.terminal_width;
  ws.max_y = ws.terminal_height - (ws.bottom_offset + ws.top_offset);
  return ws;
}

// TERMINAL
void die(const char *s) {
  cleanEditor();
  perror(s);
  exit(1);
}

void disableRawMode() {
  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios) == -1)
    die("tcssetattr");
}

void enableRawMode() {
  if (tcgetattr(STDIN_FILENO, &orig_termios) == -1)
    die("tcgetattr");
  atexit(disableRawMode);

  struct termios raw = orig_termios;
  raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
  raw.c_oflag &= ~(OPOST);

  raw.c_cflag |= (CS8);
  raw.c_cc[VMIN] = 0;
  raw.c_cc[VTIME] = 1;

  raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);

  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1)
    die("tcsetattr");
}

// HELPER
char *addNewLineChar(char *str) {
  if (str == NULL) {
    return NULL; // Return NULL if input is NULL.
  }

  size_t str_len = strlen(str);

  // Use a temporary pointer to avoid the realloc pitfall.
  char *temp = realloc(str, str_len + 3); // +2 for \r\n, +1 for \0
  if (temp == NULL) {
    die("addNewLineChar: realloc failed");
  }

  // Now it's safe to update the pointer.
  str = temp;

  // Append the newline characters.
  str[str_len] = '\r';
  str[str_len + 1] = '\n';
  str[str_len + 2] = '\0';
  return str;
}

char **splitLine(const char *str, size_t n) {
  if (str == NULL) {
    return NULL;
  }

  size_t str_len = strlen(str);
  if (n > str_len) {
    return NULL;
  }

  char **res = malloc(2 * sizeof(*res));
  if (res == NULL) {
    die("splitLine: failed malloc");
  }

  size_t second_part_len = str_len - n;

  res[0] = malloc(n + 1);
  res[1] = malloc(second_part_len + 1);

  if (res[0] == NULL || res[1] == NULL) {
    free(res[0]);
    free(res[1]);
    free(res);
    die("splitLine: failed to allocate memory for a string");
  }

  memcpy(res[0], str, n);
  res[0][n] = '\0';

  memcpy(res[1], str + n, second_part_len);
  res[1][second_part_len] = '\0';

  return res;
}

void moveRowsDown(char **lines, int row_to_move, int *num_lines) {
  if (lines == NULL || row_to_move < 0)
    return;

  for (int i = *num_lines; i > row_to_move; i--) {
    lines[i] = lines[i - 1];
  }

  (*num_lines)++;

  lines[row_to_move] = NULL;
}

int countNewLineChars(const char *str) {
  if (str == NULL) {
    return 0;
  }

  size_t len = strlen(str);

  if (len >= 2 && str[len - 2] == '\r' && str[len - 1] == '\n') {
    return 2;
  } else if (len >= 1 && str[len - 1] == '\n') {
    return 1;
  } else if (len >= 1 && str[len - 1] == '\r') {
    return 1;
  }

  return 0;
}

void cleanEditor() {
  if (global_buffer_initialized) {
    freeTextBuffer(&global_buffer_for_cleanup);
    global_buffer_initialized = 0;
  }
}

void freeTextBuffer(struct TextBuffer *buffer) {
  if (buffer == NULL || buffer->lines == NULL)
    return;
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
  if (from == NULL) {
    *to = '\0';
    return;
  }

  while (*from != '\0') {
    *to++ = *from++;
  }
  *to = '\0';
}

char *appendTwoLines(const char *str1, const char *str2) {
  if (str1 == NULL)
    str1 = "";
  if (str2 == NULL)
    str2 = "";

  size_t len_str1 = strlen(str1);
  size_t len_str2 = strlen(str2);
  size_t total_len =
      len_str1 + len_str2 + 1; // +1 for the final null terminator

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

void moveCharsRight(char *str, int cur_pos, int n) {
  int len = strlen(str);

  for (int i = len; i >= cur_pos; i--) {
    str[i + n] = str[i];
  }
}

void moveCharsLeft(char *str, int cur_pos, int n) {
  int len = strlen(str);

  if (cur_pos < n)
    return;

  for (int i = cur_pos; i <= len; i++) {
    str[i - n] = str[i];
  }
}

void moveRowsUp(char **lines, int num_lines, int row_to_delete) {
  // so cur_row disappears
  if (lines == NULL || row_to_delete < 0)
    return;

  for (int i = row_to_delete; i < num_lines; i++) {
    lines[i] = lines[i + 1];
  }
}

char *makeStringFromInt(int n) {
  char *str = malloc(12);
  if (!str)
    die("makeStringFromInt: malloc failed");
  snprintf(str, 12, "%d", n);
  return str;
}

int getScreenLinesForString(const char *str, struct WindowSettings *ws) {
  if (str == NULL) {
    return 1;
  }
  int len = strlen(str);
  int stringLength = len;

  if (stringLength >= 2 && str[stringLength - 2] == '\r' &&
      str[stringLength - 1] == '\n') {
    stringLength -= 2;
  } else if (stringLength >= 1 &&
             (str[stringLength - 1] == '\r' || str[stringLength - 1] == '\n')) {
    stringLength -= 1;
  }

  if (stringLength == 0) {
    return 1;
  }

  if (ws->terminal_width == 0)
    return 1;
  return (stringLength / ws->terminal_width) +
         ((stringLength % ws->terminal_width != 0) ? 1 : 0);
}

// DYNAMIC ARRAY MANAGEMENT for buffer->lines
void editorEnsureLineCapacity(struct TextBuffer *buffer, int required_idx) {
  if (required_idx >= buffer->linesCapacity) {
    int newCapacity = buffer->linesCapacity == 0 ? INITIAL_LINES_CAPACITY
                                                 : buffer->linesCapacity * 2;
    while (newCapacity <= required_idx) {
      newCapacity *= 2;
    }
    char **new_lines = realloc(buffer->lines, newCapacity * sizeof(char *));
    if (!new_lines)
      die("editorEnsureLineCapacity: realloc lines failed");

    for (int i = buffer->linesCapacity; i < newCapacity; i++) {
      new_lines[i] = NULL;
    }
    buffer->lines = new_lines;
    buffer->linesCapacity = newCapacity;
  }
}

// CURSOR
void editorUpdateCursorCoordinates(struct TextBuffer *buffer,
                                   struct WindowSettings *ws,
                                   struct ScreenSettings *screen_settings) {
  screen_settings->cursor_y = ws->top_offset + 1;

  screen_settings->first_textbuffer_line_to_print = 0;

  for (int i = 0; i < buffer->curY; i++) {

    if(screen_settings->cursor_y>ws->max_y){
      screen_settings->first_textbuffer_line_to_print++;
    }

    if (i < buffer->numlines && buffer->lines[i] != NULL) {
      screen_settings->cursor_y += getScreenLinesForString(buffer->lines[i], ws);
    } else {
      screen_settings->cursor_y += 1;
    }
  }

  if (ws->terminal_width > 0) {
    screen_settings->cursor_x = (buffer->curX % ws->terminal_width) + 1;
    screen_settings->cursor_y += (buffer->curX / ws->terminal_width);
  } else {
    screen_settings->cursor_x = buffer->curX + 1;
  }
}

void editorRefreshCursor(struct ScreenSettings *screen_settings) {
  char *x = makeStringFromInt(screen_settings->cursor_x);
  char *y = makeStringFromInt(screen_settings->cursor_y);
  int len = 5 + strlen(x) + strlen(y);
  char *str = malloc(len + 1);

  if (!str) {
    free(x);
    free(y);
    die("screen_settingsRefresh: malloc failed");
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

void moveCursorRight(struct TextBuffer *buffer,
                     struct ScreenSettings *screen_settings) {
  int stringLength = strlen(buffer->curLine);

  if (stringLength >= 2 && buffer->curLine[stringLength - 2] == '\r' &&
      buffer->curLine[stringLength - 1] == '\n') {
    stringLength -= 2;
  } else if (stringLength >= 1 && (buffer->curLine[stringLength - 1] == '\r' ||
                                   buffer->curLine[stringLength - 1] == '\n')) {
    stringLength -= 1;
  }

  if (buffer->curX < stringLength) {
    buffer->curX++;
  } else if (buffer->curY < buffer->numlines) {
    bufferSaveCurrentLine(buffer);
    moveCursorDown(buffer, screen_settings);
  }
  screen_settings->logical_wanted_x = buffer->curX;
}

void moveCursorLeft(struct TextBuffer *buffer,
                    struct ScreenSettings *screen_settings) {
  if (buffer->curX > 0) {
    buffer->curX--;
  } else if (buffer->curY > 0) {
    bufferSaveCurrentLine(buffer);
    buffer->curY--;
    bufferLoadCurLine(buffer);
    int prevLineLen = strlen(buffer->curLine);
    if (prevLineLen >= 2 && buffer->curLine[prevLineLen - 2] == '\r' &&
        buffer->curLine[prevLineLen - 1] == '\n') {
      prevLineLen -= 2;
    } else if (prevLineLen >= 1 && (buffer->curLine[prevLineLen - 1] == '\r' ||
                                    buffer->curLine[prevLineLen - 1] == '\n')) {
      prevLineLen -= 1;
    }
    buffer->curX = prevLineLen;
  }
  screen_settings->logical_wanted_x = buffer->curX;
}

void moveCursorUp(struct TextBuffer *buffer, struct ScreenSettings *screen_settings) {
  if (buffer->curY == 0)
    return;

  bufferSaveCurrentLine(buffer);

  buffer->curY--;

  bufferLoadCurLine(buffer);

  int lineLen = strlen(buffer->curLine);
  if (lineLen >= 2 && buffer->curLine[lineLen - 2] == '\r' &&
      buffer->curLine[lineLen - 1] == '\n') {
    lineLen -= 2;
  } else if (lineLen >= 1 && (buffer->curLine[lineLen - 1] == '\r' ||
                              buffer->curLine[lineLen - 1] == '\n')) {
    lineLen -= 1;
  }

  buffer->curX = MIN(lineLen, screen_settings->logical_wanted_x);
}

void moveCursorDown(struct TextBuffer *buffer,
                    struct ScreenSettings *screen_settings) {
  if (buffer->curY < buffer->numlines) {
    bufferSaveCurrentLine(buffer);

    buffer->curY++;

    // Hitting the virtual line
    if (buffer->curY == buffer->numlines) {

      buffer->curY--;

      bufferLoadCurLine(buffer);

      int len = strlen(buffer->curLine);
      // i need to handle go to the virtual line also as the 'enter' press. so
      // put to the of the line \r\n;
      if (!(len >= 2 && buffer->curLine[len - 2] == '\r' &&
            buffer->curLine[len - 1] == '\n')) {
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
      if (lenCurrentLine >= 2 && buffer->curLine[lenCurrentLine - 2] == '\r' &&
          buffer->curLine[lenCurrentLine - 1] == '\n') {
        lenCurrentLine -= 2;
      } else if (lenCurrentLine >= 1 &&
                 (buffer->curLine[lenCurrentLine - 1] == '\r' ||
                  buffer->curLine[lenCurrentLine - 1] == '\n')) {
        lenCurrentLine -= 1;
      }
    }
    buffer->curX = MIN(lenCurrentLine, screen_settings->logical_wanted_x);
  }
}

// OUTPUT
void screenBufferWriteLine(char *line, int *shownY, struct WindowSettings *ws, int *appended, int *size, char *screenBuffer){
    int len = strlen(line);
    int offset = 0;

    while (offset < len && *shownY < ws->max_y) {
      int remain = len - offset;
      int lineLength = (remain > ws->max_x) ? ws->max_x : remain;

      if (appended + lineLength + 2 >= size) {
        (*size) *= 2;
        screenBuffer = realloc(screenBuffer, *size);
        if (!screenBuffer)
          die("editorPrepareBufferForScree: realloc failed");
      }

      memcpy(&screenBuffer[*appended], &line[offset], lineLength);
      appended += lineLength;

      if (remain > ws->max_x) { // If the line was wrapped, add \r\n for display
        screenBuffer[*appended++] = '\r';
        screenBuffer[*appended++] = '\n';
      }

      offset += lineLength;
      shownY++;
    }
}

char *editorPrepareBufferForScreen(struct TextBuffer *buffer, struct WindowSettings *ws, struct ScreenSettings *screen_settings) {
  int size = 2500;
  char *screenBuffer = malloc(size);

  if (!screenBuffer) die("editorPrepareBufferForScreen: malloc failed");

  int appended = 0;
  int shownY = 0;

  for (int i = screen_settings->first_textbuffer_line_to_print; i <= buffer->numlines && shownY < ws->max_y; i++) { // NOTE i am going from the FIRST line to the last

    char *line = buffer->lines[i];

    if (line == NULL)
      continue;
    screenBufferWriteLine(line, &shownY, ws, &appended, &size, screenBuffer);
  }

  screenBuffer[appended] = '\0';
  return screenBuffer;
}


void editorRefreshScreen(struct TextBuffer *buffer, struct WindowSettings *ws, struct ScreenSettings *screen_settings) {
  // clear the terminal
  write(STDOUT_FILENO, "\x1b[2J", 4);
  // put screen_settings to the top
  write(STDOUT_FILENO, "\x1b[H", 3);

  char *screenBuffer = editorPrepareBufferForScreen(buffer, ws, screen_settings);

  write(STDOUT_FILENO, screenBuffer, strlen(screenBuffer));

  free(screenBuffer);
}

void editorOutputBufferText(struct TextBuffer *buffer) {
  // clear the terminal
  write(STDOUT_FILENO, "\x1b[2J", 4);
  // put screen_settings to the top
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

// INPUT

void bufferLoadCurLine(struct TextBuffer *buffer) {
  if (buffer->curY < buffer->numlines) {
    copyLine(buffer->lines[buffer->curY], buffer->curLine);
  } else {
    // die("bufferLoadCurLine: trying to add in the buffer a line, that doesnt
    // exist");
    curLineClearAndResetX(buffer);
  }
}

void curLineDeleteChar(struct TextBuffer *buffer,
                       struct ScreenSettings *screen_settings) {
  if (buffer->curX == 0 && buffer->curY > 0) {
    bufferSaveCurrentLine(buffer);

    char *prev_str = buffer->lines[buffer->curY - 1];
    int len_prev_str = strlen(prev_str);

    if (len_prev_str >= 2 && prev_str[len_prev_str - 2] == '\r' &&
        prev_str[len_prev_str - 1] == '\n') {
      prev_str[len_prev_str - 2] = '\0';
      len_prev_str -= 2;
    } else if (len_prev_str >= 1 && prev_str[len_prev_str - 1] == '\n') {
      prev_str[len_prev_str - 1] = '\0';
      len_prev_str -= 1;
    }

    char *appended_line = appendTwoLines(prev_str, buffer->lines[buffer->curY]);

    if (appended_line == NULL) {
      die("curLineDeleteChar: appending of two lines is failed");
    }

    free(buffer->lines[buffer->curY]);
    moveRowsUp(buffer->lines, buffer->numlines, buffer->curY);

    buffer->curY--;
    curLineClearAndResetX(buffer);
    curLineWriteChars(buffer, appended_line);
    free(appended_line);

    bufferSaveCurrentLine(buffer);
    buffer->numlines--;

    buffer->curX = len_prev_str;
    screen_settings->logical_wanted_x = buffer->curX;
  } else if (buffer->curX > 0) {
    moveCharsLeft(buffer->curLine, buffer->curX, 1);
    buffer->curX--;
  }
}

void bufferSaveCurrentLine(struct TextBuffer *buffer) {
  editorEnsureLineCapacity(buffer, buffer->curY);

  int size = strlen(buffer->curLine);

  if (buffer->lines[buffer->curY] != NULL) {
    free(buffer->lines[buffer->curY]); // Free existing memory
  }
  buffer->lines[buffer->curY] = malloc(size + 1);

  if (buffer->lines[buffer->curY] == NULL) {
    die("writeCurrentLineToBuffer: malloc failed");
  }
  copyLine(buffer->curLine, buffer->lines[buffer->curY]);
}

char editorReadKey() {
  char nread; // output result code
  char c;
  while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
    if (nread == -1 && errno != EAGAIN)
      die("read");
  }
  return c;
}

void curLineWriteChar(struct TextBuffer *buffer, char c) {
  if (buffer->curX >= SIZELINE - 1)
    die("bufferWriteChar: SIZELINE is exceeded");

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

void curLineWriteChars(struct TextBuffer *buffer, const char *chars) {
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
  buffer->curLine[current_len + add_len] = '\0';
  buffer->curX += add_len;
}

void curLineClearAndResetX(struct TextBuffer *buffer) {
  buffer->curX = 0;
  buffer->curLine[buffer->curX] = '\0'; // clear current line;
}

void bufferHandleNewLineInput(struct TextBuffer *buffer,
                              struct ScreenSettings *screen_settings) {
  editorEnsureLineCapacity(buffer, buffer->numlines + 1);

  char **splitted_lines = splitLine(buffer->curLine, buffer->curX);
  if (splitted_lines == NULL) {
    return;
  }

  char *first_half = splitted_lines[0];
  char *second_half = splitted_lines[1];

  moveRowsDown(buffer->lines, buffer->curY + 1, &buffer->numlines);

  curLineClearAndResetX(buffer);

  first_half = addNewLineChar(first_half);
  if (first_half == NULL) {
    die("bufferHandleNewLineInput: malloc failed -> first_half var");
  }

  curLineWriteChars(buffer, first_half);
  bufferSaveCurrentLine(buffer);

  buffer->curY++;
  curLineClearAndResetX(buffer);
  curLineWriteChars(buffer, second_half);
  bufferSaveCurrentLine(buffer);

  buffer->curX = 0;
  screen_settings->logical_wanted_x = 0;

  free(first_half);
  free(second_half);
  free(splitted_lines);
}

void bufferHandleEscapeSequence(struct TextBuffer *buffer,
                                struct ScreenSettings *screen_settings) {
  if (!isInputAvailable())
    return;
  char c = editorReadKey();
  if (c != '[')
    return;
  if (!isInputAvailable())
    return;
  c = editorReadKey();
  switch (c) {
  case 'A':
    moveCursorUp(buffer, screen_settings);
    break;
  case 'B':
    moveCursorDown(buffer, screen_settings);
    break;
  case 'C':
    moveCursorRight(buffer, screen_settings);
    break;
  case 'D':
    moveCursorLeft(buffer, screen_settings);
    break;
  }
}

void editorProcessKeypress(struct TextBuffer *buffer, struct WindowSettings *ws,
                           struct ScreenSettings *screen_settings) {
  char c = editorReadKey();

  switch (c) {
  case CTRL_KEY('q'):
    exit(0);
    break;
  case ('\r'):
  case ('\n'):
    bufferHandleNewLineInput(buffer, screen_settings);
    break;
  case CTRL_KEY('p'):
    editorOutputBufferText(buffer);
    break;
  case DEL:
  case BACKSPACE:
    curLineDeleteChar(buffer, screen_settings);
    bufferSaveCurrentLine(buffer);
    break;
  case ('\x1b'):
    bufferHandleEscapeSequence(buffer, screen_settings);
    break;
  default:
    if (buffer->curY == buffer->numlines) {
      buffer->numlines++;
    }
    curLineWriteChar(buffer, c);
    bufferSaveCurrentLine(buffer);
    screen_settings->logical_wanted_x = buffer->curX;
    break;
  }

  editorRefreshScreen(buffer, ws, screen_settings);
  editorUpdateCursorCoordinates(buffer, ws, screen_settings);
  editorRefreshCursor(screen_settings);
}

// INIT
int main() {
  enableRawMode();
  struct TextBuffer buffer = textBufferInit();
  struct WindowSettings ws = windowSettingsInit();
  struct ScreenSettings screen_settings = {1, 1, 0, 0};
  editorRefreshScreen(&buffer, &ws, &screen_settings);
  while (1) {
    editorProcessKeypress(&buffer, &ws, &screen_settings);
  }
  cleanEditor();
  return 0;
}
