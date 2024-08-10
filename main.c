/*** Includes ***/
// #include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdbool.h>
#include <termios.h>
#include <string.h>
#include <sys/ioctl.h>
#include "tc.h"

/*** Data and Input ***/
enum Mode {
  Normal = 0,
  Insert = 1, 
};

struct editorConfig {
  enum Mode mode;
  struct termios orig_termios; // will hold original terminal settings to revert back to it
  int cx, cy; // cursor position
  int screenrows, screencols;
};

struct editorConfig state;

void die(const char *s){
  tc_exit_alt_screen();
  perror(s);
  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &state.orig_termios) == -1) // set terminal setting to old settings
    die("tcsettattr"); 
  
  exit(1);
}

enum editorKey {
  PAGE_UP = 1000,
  PAGE_DOWN, 
  HOME_KEY, 
  END_KEY, 
  DELETE_KEY, 
};

int editorReadKey(){
  int nread;
  char c;

  while ((nread = read(STDIN_FILENO, &c, 1)) != 1){
    if (nread == -1 && errno != EAGAIN) die("read");
  }

  if (c == '\x1b') {
    char seq[3];

    if (read(STDIN_FILENO, &seq[0], 1) != 1) return '\x1b';
    if (read(STDIN_FILENO, &seq[1], 1) != 1) return '\x1b';

    if (seq[0] == '['){

      if (seq[1] <= '0' && seq[1] >= '9') {

        if (read(STDIN_FILENO, &seq[2], 1) != 1) return '\x1b';

        if (seq[2] == '~'){
          switch (seq[1]) {
            case '1': return HOME_KEY;
            case '3': return DELETE_KEY;
            case '4': return END_KEY;
            case '5': return PAGE_UP;
            case '6': return PAGE_DOWN;
            case '7': return HOME_KEY;
            case '8': return END_KEY;
          }
        }

      } else {
        switch(seq[1]){
          case 'A': return 'k';
          case 'B': return 'j';
          case 'C': return 'l';
          case 'D': return 'h';
          case 'H': return HOME_KEY;
          case 'F': return END_KEY;
        }
      }

    } else if (seq[0] == 'O') {

      switch(seq[1]) {
        case 'H': return HOME_KEY;
        case 'F': return END_KEY;
      }

    }

    return '\x1b';


  } else return c; 
}

/*** Terminal ***/
void disableRawMode(){
  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &state.orig_termios) == -1) // set terminal setting to old settings
    die("tcsettattr"); 
}

void enableRawMode(){
  struct termios raw; // struct is created that will hold terminal attributes

  if (tcgetattr(STDIN_FILENO, &state.orig_termios) == -1) // get attributes and store in variable
    die("tcgetattr"); 

  raw = state.orig_termios; // structs do NOT behave like objects in Java
  raw.c_iflag &= ~(IXON | ICRNL | BRKINT | INPCK | ISTRIP); // IXON: ctrl-s/q - used for pausing + resuming data transmission
  // ICRNL refers to carriage return and newline - terminal makes them the same ASCII code (no good) -> ctrl-m != ctrl-j
  raw.c_oflag &= ~(OPOST); // turn off all post processing of our input
  raw.c_lflag &= ~(ECHO | ICANON | ISIG | IEXTEN); // bitwise operation to turn off ECHO feature - prints text you type
  // ISIG turns off ctrl-c and ctrl-z functionality in terminal
  // IEXTEN fixes ctrl-v in some terminals where it waits for another character and then sends it, also fixes ctrl-o in Macs
  // Turn off remaining miscellanous flags (including BRKINT, INPCK, ISTRIP):
  raw.c_cflag |= (CS8);

  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) // update terminal attributes
    die("tcsetattr");
}

int getCursorPosition(int *rows, int *cols){
  char buf[32];
  unsigned int i = 0;

  if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4) return -1;

  while (i < sizeof(buf) - 1) {
    if (read(STDIN_FILENO, &buf[i], 1) != 1) break;
    if (buf[i] == 'R') break; // Here char array is indexed into like normal
    i++;
  }

  buf[i] = '\0';
  if (buf[0] != '\x1b' || buf[1] != '[') return -1;
  if (sscanf(&buf[2], "%d;%d", rows, cols) != 2) return -1; // & basically makes char arrays behave like a string
  // We are basically saying <- return the items in memory from the third index onward

  return 0;
}

int getWindowSize(int *rows, int *cols){
  struct winsize ws;

  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
    if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B" , 12) != 12) return -1;
    return getCursorPosition(rows, cols);
  } else {
    *cols = ws.ws_col;
    *rows = ws.ws_row;
    return 0;
  }
}

/*** append buffer ***/
struct abuf {
  char *b;
  int len;
};

#define ABUF_INIT {NULL, 0};

void abAppend(struct abuf *ab, const char *s, int len) {
  char *new = realloc(ab->b, ab->len+len);

  if (new == NULL) return;
  memcpy(&new[ab->len], s, len);
  ab->b = new;
  ab->len += len;
}

void abFree(struct abuf *ab){
  free(ab->b);
}

void initEditor(){
  state.cx = 0;
  state.cy = 0;
  state.mode = Normal;
  if (getWindowSize(&state.screenrows, &state.screencols) == -1)
    die("getWindowSize");
}


void editorDrawRows(struct abuf *ab){
  for (int i = 0; i < state.screenrows; i++) {
    abAppend(ab, "~", 1);

    abAppend(ab, "\x1b[K", 3); // "K" erases parts of line; 2 - whole line, 1 - line left of cursor, 0 erases line right of cursor
    // 0 is the default argument if the number is ommitted
    if (i != state.screenrows - 1) {
      abAppend(ab, "\r\n", 2);
    }
  }
}

void editorRefreshScreen(){
  struct abuf ab = ABUF_INIT;

  abAppend(&ab, "\x1b[?25l", 6); // hide cursor
  abAppend(&ab, "\x1b[H", 3); // cursor go to 0, 0

  editorDrawRows(&ab);
  
  char buf[32];
  snprintf(buf, sizeof(buf), "\x1b[%d;%dH", state.cy + 1, state.cx + 1); // cy and cx + 1 because terminal rows and cols start at 1
  abAppend(&ab, buf, strlen(buf));

  abAppend(&ab, "\x1b[?25h", 6); // show cursor

  write(STDOUT_FILENO, ab.b, ab.len);
  free(ab.b);
}

void editorMoveCursor(char key){
  switch (key) {
    case 'h':
      if (state.cx != 0) state.cx--;
      break;
    case 'j':
      if (state.cy != state.screenrows - 1) state.cy++;
      break;
    case 'k':
      if (state.cy != 0) state.cy--;
      break;
    case 'l':
      if (state.cx != state.screencols - 1) state.cx++;
      break;
  }
}

void editorProcessKeypress(){
  int c = editorReadKey();
  switch (c) {
    case 'q':
      disableRawMode();
      tc_exit_alt_screen();
      exit(0);
      break;
    case DELETE_KEY:
      // NOTE: Should have the same functionality as the 'x' key 
      break;
    case HOME_KEY:
      state.cx = 0;
      break;
    case END_KEY:
      state.cx = state.screencols - 1;
      break;
    case PAGE_UP:
    case PAGE_DOWN:
      // NOTE: Have page up and down do something; I think they are to go up and down a page
      break;
    case 'h':
    case 'j':
    case 'k':
    case 'l':
      editorMoveCursor(c);
      break;
  }
}

int main(){

  tc_enter_alt_screen();
  editorRefreshScreen();
  enableRawMode();
  initEditor();

  char c;
  while(1){ 
    editorRefreshScreen();
    editorProcessKeypress();
  }

  return 0;
}

