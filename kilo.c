#include <unistd.h>
#include <termios.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <string.h>

#define KILO_VERSION "0.0.1"

#define CTRL_KEY(k) ((k) & 0x1f)

/*** data ***/
struct editor_config {
  int current_x;
  int current_y;
  int screen_rows;
  int screen_cols;
  struct termios original_attrs;
};
struct editor_config E;

enum editor_key {
  ARROW_LEFT = 1000,
  ARROW_RIGHT,
  ARROW_UP,
  ARROW_DOWN
};

/*** append buffer ***/

#define ABUF_INIT {NULL, 0}

struct abuf {
  char *b;  // pointer to buffer in memory
  int len;  // length of the buffer
};

void ab_append(struct abuf *ab, const char *s, int len) {
  char *new = realloc(ab->b, ab->len + len);  // allocate an additional 'len' bytes of memory

  if (new == NULL) {
    return;
  }
  memcpy(&new[ab->len], s, len);  // Copy the new string into the newly allocated memory
  ab->b = new;  // realloc may have moved the buffer elsewhere in memory
  ab->len += len;
}

void ab_free(struct abuf *ab) {
  free(ab->b);
}

/*** output ***/
void clear_screen_raw() {
  write(STDOUT_FILENO, "\x1b[2J", 4);
}

void clear_screen(struct abuf *ab) {
  ab_append(ab, "\x1b[2J", 4);  // Erase In Display escape sequence
}

void cursor_to_top_left(struct abuf *ab) {
  ab_append(ab, "\x1b[H", 3);  // Cursor Position escape sequence
}

void hide_cursor(struct abuf *ab) {
  ab_append(ab, "\x1b[?25l", 6);
}

void unhide_cursor(struct abuf *ab) {
  ab_append(ab, "\x1b[?25h", 6);
}

void erase_in_line(struct abuf *ab) {
  ab_append(ab, "\x1b[K", 3);  // Erase line to right of cursor (default K command arg)
}

int cursor_to_bottom_right() {
  if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B]", 12) == 12) { // Cursor right and cursor down
    return 0;
  }
  return -1;
}

int query_cursor_position() {
  if (write(STDOUT_FILENO, "\x1b[6n", 4) == 4) {
    return 0;
  }
  return -1;
}

void refresh_screen(struct abuf *ab) {
  cursor_to_top_left(ab);
}

void draw_welcome_message(struct abuf *ab) {
  char msg[80];
  int msg_len = snprintf(msg, sizeof(msg),
    "Kilo Editor -- Version %s", KILO_VERSION);
  if (msg_len > E.screen_cols) {
    msg_len = E.screen_cols;
  }
  int padding = (E.screen_cols - msg_len) / 2;  // calculate padding required to center message
  if (padding) {
    ab_append(ab, "~", 1);
    padding--;
  }
  while (padding--) {  // add enough spaces to center
    ab_append(ab, " ", 1);
  }
  ab_append(ab, msg, msg_len);
}

void draw_rows(struct abuf *ab) {
  int i;
  for (i = 0; i < E.screen_rows; i++) {
    if (i == E.screen_rows / 3) {
      draw_welcome_message(ab);
    } else {
      ab_append(ab, "~", 1);
    }

    erase_in_line(ab);
    if (i < E.screen_rows - 1) {
      ab_append(ab, "\r\n", 2);
    }
  }
}

void full_repaint() {
  struct abuf ab = ABUF_INIT;

  hide_cursor(&ab);
  refresh_screen(&ab);
  draw_rows(&ab);

  char buf[32];
  snprintf(buf, sizeof(buf), "\x1b[%d;%dH", E.current_y + 1, E.current_x +1);
  ab_append(&ab, buf, strlen(buf));

  unhide_cursor(&ab);

  write(STDOUT_FILENO, ab.b, ab.len);
  ab_free(&ab);
}

/*** terminal ***/

void die(const char *s) {
  clear_screen_raw();
  perror(s);
  exit(1);
}

int is_escape_sequence(char buf[]) {
  return buf[0] == '\x1b' && buf[1] == '[';
}

/*
 * Blocks waiting for a single byte of input, returns the character that byte maps to
 */
int read_key() {
  int num_read;
  char c;
  while ((num_read = read(STDIN_FILENO, &c, 1)) != 1) {
    if (errno == EAGAIN) {
      die("read");
    }
  }

  if (c == '\x1b') {
    char seq[3];

    if (read(STDIN_FILENO, &seq[0], 1) != 1) return '\x1b';
    if (read(STDIN_FILENO, &seq[1], 1) != 1) return '\x1b';

    if (seq[0] == '[') {
      switch (seq[1]) {
        case 'A': return ARROW_UP;
        case 'B': return ARROW_DOWN;
        case 'C': return ARROW_RIGHT;
        case 'D': return ARROW_LEFT;
      }

      return '\x1b';
    }
  }
  return c;
}

void disable_raw_mode() {
  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.original_attrs) == -1) {
    die("tcsetattr");
  }
}

void enable_raw_mode() {
  if (tcgetattr(STDIN_FILENO, &E.original_attrs) == -1) {  // Store the original terminal attrs
    die("tcgetattr");
  }
  atexit(disable_raw_mode);  // Register callback function to call on exit - resets terminal attrs
  struct termios terminal_attrs = E.original_attrs;  // Copy attributes to alter them
  terminal_attrs.c_iflag &= ~(IXON | ICRNL);
  terminal_attrs.c_oflag &= ~(OPOST);
  terminal_attrs.c_lflag &= ~(ECHO | ICANON | ISIG | IEXTEN);
  terminal_attrs.c_cc[VMIN] = 0;
  terminal_attrs.c_cc[VTIME] = 1;
  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &terminal_attrs) == -1) {  // Update the terminal attributes
    die("tcsetattr");
  }
}

int get_cursor_position(int *rows, int *cols) {
  char buf[32];
  unsigned int i = 0;

  if (query_cursor_position() == -1) {
    return -1;
  }

  while (i < sizeof(buf) - 1) {
    if (read(STDIN_FILENO, &buf[i], 1) != 1) {
      break;
    }
    if (buf[i] == 'R') {
      break;
    }
    i++;
  }
  buf[i] = '\0';

  if (!is_escape_sequence(buf)) {
    return -1;
  }

  if (sscanf(&buf[2], "%d;%d", rows, cols) != 2) {  // e.g. "25;80"
    return -1;
  }

  return 0;
}

int get_window_size_fallback(int *rows, int *cols) {
  if (cursor_to_bottom_right() == -1) {
    return -1;
  }
  return get_cursor_position(rows, cols);
}

int get_window_size(int *rows, int *cols) {
  struct winsize window_size;

  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &window_size) == -1 || window_size.ws_col == 0) {
    return get_window_size_fallback(rows, cols);
  }
  *cols = window_size.ws_col;
  *rows = window_size.ws_row;
  return 0;
}

/*** input ***/

void move_cursor(int key) {
  switch (key) {
    case ARROW_LEFT:
      E.current_x--;
      break;
    case ARROW_RIGHT:
      E.current_x++;
      break;
    case ARROW_UP:
      E.current_y--;
      break;
    case ARROW_DOWN:
      E.current_y++;
      break;
  }
}

void read_and_process_key() {
  int c = read_key();
  switch (c) {
    case CTRL_KEY('q'):  // If pressed ctrl+q
      clear_screen_raw();
      exit(0);
      break;
    
    // Cursor movement
    case ARROW_UP:
    case ARROW_LEFT:
    case ARROW_DOWN:
    case ARROW_RIGHT:
      move_cursor(c);
      break;
  }
}

void init_editor() {
  E.current_x = 0;
  E.current_y =   0;
  if (get_window_size(&E.screen_rows, &E.screen_cols) == -1) {
    die("get_window_size");
  }
}

int main() {
  enable_raw_mode();
  init_editor();

  while (1) {
    full_repaint();
    read_and_process_key();
  }

  return 0;
}
