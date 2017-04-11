#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <unistd.h>
#include <termios.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <string.h>
#include <sys/types.h>
#include <time.h>
#include <stdarg.h>

#define KILO_VERSION "0.0.1"
#define KILO_TAB_STOP 8

#define CTRL_KEY(k) ((k) & 0x1f)

/*** data ***/
typedef struct erow {
  int size;  // number of bytes in the row
  int render_size;
  char *chars;  // characters in the row
  char *render;
} erow;

struct editor_config {
  int current_x;  // cursor coordinates
  int current_y;
  int render_x;  // coordinates in render buffer
  int render_y;
  int row_offset;  // how many rows have we scrolled by (i.e. that are now off screen)?
  int col_offset;  // how many columns have we scrolled by?
  int screen_rows;  // the width and height of the terminal window
  int screen_cols;
  int num_rows;  // the number of rows in the file with content
  erow *row;  // an array of erows, the internal representation of a row
  char *filename;  // the name of the file that is currently open
  char statusmsg[80];
  time_t statusmsg_time;
  struct termios original_attrs;  // the original terminal attributes to reset on end
};
struct editor_config E;

enum editor_key {
  ARROW_LEFT = 1000,
  ARROW_RIGHT,
  ARROW_UP,
  ARROW_DOWN,
  PAGE_UP,
  PAGE_DOWN,
  HOME,
  END,
  DELETE
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

void editor_scroll() {

  if (E.current_y < E.row_offset) {  // Scrolling up
    E.row_offset = E.current_y;
  }
  if (E.current_y - E.row_offset >= E.screen_rows) {  // Scrolling down
    E.row_offset = E.current_y - E.screen_rows + 1;
  }
  if (E.render_x < E.col_offset) {  // Scrolling left
    E.col_offset = E.render_x;
  }
  if (E.render_x - E.col_offset > E.screen_cols) {  // Scrolling right
    E.col_offset = E.render_x - E.screen_cols + 1;
  }
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

int row_current_x_to_render_x(erow *row, int current_x) {
  int render_x = 0;
  for (int i = 0; i < current_x; i++) {
    if (row->chars[i] == '\t') {
        render_x += (KILO_TAB_STOP - 1) - (render_x % KILO_TAB_STOP);
    }
    render_x++;
  }
  return render_x;
}

void draw_rows(struct abuf *ab) {
  int y;
  for (y = 0; y < E.screen_rows; y++) {
    int file_row = y + E.row_offset;

    if (file_row >= E.num_rows) {  // no content on the row
      if (E.num_rows == 0 && y == E.screen_rows / 3) {
        draw_welcome_message(ab);
      } else {
        ab_append(ab, "~", 1);
      }
    } else {  // drawing a row that has content
      int len = E.row[file_row].render_size - E.col_offset;
      if (len < 0) len = 0;
      if (len > E.screen_cols) {
        len = E.screen_cols;  // truncate line if required
      }
      ab_append(ab, &E.row[file_row].render[E.col_offset], len);  // copy the characters of that row into the append buffer
    }

    erase_in_line(ab);
    ab_append(ab, "\r\n", 2);
  }
}

void draw_status_bar(struct abuf *ab) {
  ab_append(ab, "\x1b[7m", 4);
  
  char status[80], rstatus[80]; 
  int len = snprintf(status, sizeof(status), "%.20s - %d lines",
    E.filename ? E.filename : "[No Name]", E.num_rows);
  int rlen = snprintf(rstatus, sizeof(rstatus), "%d/%d", E.current_y + 1, E.num_rows);

  if (len > E.screen_cols) {
    len = E.screen_cols;
  }
  ab_append(ab, status, len);

  while (len < E.screen_cols) {
    if (E.screen_cols - len == rlen) {  // right justify
      ab_append(ab, rstatus, rlen);
      break;
    }
    ab_append(ab, " ", 1);
    len++;
  }
  ab_append(ab, "\x1b[m", 3);
  ab_append(ab, "\r\n", 2);
}

void set_status_message(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(E.statusmsg, sizeof(E.statusmsg), fmt, ap);
  va_end(ap);
  E.statusmsg_time = time(NULL);
}

void draw_message_bar(struct abuf *ab) {
  erase_in_line(ab);  // clear out the message bar
  int msg_len = strlen(E.statusmsg);
  if (msg_len > E.screen_cols) {
    msg_len = E.screen_cols;
  }
  if (msg_len && time(NULL) - E.statusmsg_time < 5) {
    ab_append(ab, E.statusmsg, msg_len);
  }
}

void full_repaint() {
  E.render_x = 0;
  if (E.current_y < E.num_rows) {
    E.render_x = row_current_x_to_render_x(&E.row[E.current_y], E.current_x);
  }

  editor_scroll();  // update the offset based on current cursor position etc.

  struct abuf ab = ABUF_INIT;

  hide_cursor(&ab);
  refresh_screen(&ab);
  draw_rows(&ab);
  draw_status_bar(&ab);
  draw_message_bar(&ab);

  char buf[32];
  // e.g. we're at line 4 of file, but have scrolled 5 lines in, so cursor should be rendered at row idx 0
  snprintf(buf, sizeof(buf), "\x1b[%d;%dH", E.current_y - E.row_offset + 1, 
                                            E.render_x - E.col_offset + 1);  // position cursor
  ab_append(&ab, buf, strlen(buf));

  unhide_cursor(&ab);

  write(STDOUT_FILENO, ab.b, ab.len);
  ab_free(&ab);
}

/*** terminal ***/

void die(const char *s) {
  clear_screen_raw();
  perror(s);
  exit(EXIT_FAILURE);
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

    // Parse the VT100 control sequence
    if (seq[0] == '[') {
      if (seq[1] >= '0' && seq[1] <= '9') {
        if (read(STDIN_FILENO, &seq[2], 1) != 1) {
          return '\x1b';
        }
        if (seq[2] == '~') {
          switch (seq[1]) {
            case '1': return HOME;
            case '3': return DELETE;
            case '4': return END;
            case '5': return PAGE_UP;
            case '6': return PAGE_DOWN;
            case '7': return HOME;
            case '8': return END;
          }
        }
      } else if (seq[0] == 'O') {
        switch (seq[1]) {
          case 'H': return HOME;
          case 'F': return END;
        }
      } else {
        switch (seq[1]) {
          case 'A': return ARROW_UP;
          case 'B': return ARROW_DOWN;
          case 'C': return ARROW_RIGHT;
          case 'D': return ARROW_LEFT;
          case 'H': return HOME;
          case 'F': return END;
        }
      }
    }
    return '\x1b';
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

/*** row operations ***/

/* Copies row into render buffer, and applies any transformations 
 * required before rendering the text to the terminal
 */
void editor_update_row(erow *row) {
  // Count number of tabs in the row, so that we know how much space to allocate
  int num_tabs = 0;
  for (int i = 0; i < row->size; i++) {
    if (row->chars[i] == '\t') {
      num_tabs++;
    }
  }

  free(row->render);
  row->render = malloc(row->size + num_tabs*(KILO_TAB_STOP - 1) + 1);
  int idx = 0;
  for (int i = 0; i < row->size; i++) {
    // Copy the data into the render buffer, rendering tabs as multiple spaces
    if (row->chars[i] == '\t') {
      row->render[idx++] = ' ';
      while (idx % KILO_TAB_STOP != 0) {
        row->render[idx++] = ' ';
      }
    } else {
      row->render[idx++] = row->chars[i];
    }
  }
  row->render[idx] = '\0';
  row->render_size = idx;
}

/* Construction and initialisation of new editor rows
 */
void editor_append_row(char *line, size_t line_length) {
  E.row = realloc(E.row, sizeof(erow) * (E.num_rows + 1));

  int at = E.num_rows;  // write to the next line
  E.row[at].size = line_length;
  E.row[at].chars = malloc(line_length + 1);  // enough for the line plus terminator
  memcpy(E.row[at].chars, line, line_length);  // copy the line into newly allocated memory
  E.row[at].chars[line_length] = '\0';  // we still have the 1 byte left over for the terminator

  E.row[at].render = NULL;
  E.row[at].render_size = 0;

  editor_update_row(&E.row[at]);

  E.num_rows++;  // we've just copied the line into a new 
}

/*** file i/o ***/
void file_open(char *filename) {
  FILE *fp = fopen(filename, "r");
  if (!fp) {
    die("fopen");
  }

  // Read a single line into the editor buffer to display it in the editor
  char *line = NULL;
  size_t line_cap = 0;
  ssize_t line_length = 0;

  while ((line_length = getline(&line, &line_cap, fp)) != -1) {
    if (line_length > 0 && (line[line_length - 1] == '\n' || line[line_length - 1] == '\r')) {
      line_length--;
    }
    editor_append_row(line, line_length);
  }
  free(line);
  fclose(fp);
}

/*** input ***/

void move_cursor(int key) {
  // Get the row if the cursor is actually on a line
  erow *row = (E.current_y >= E.num_rows) ? NULL : &E.row[E.current_y];

  switch (key) {
    case ARROW_LEFT:
      if (E.current_x != 0) {
        E.current_x--;
      } else if (E.current_y > 0) {
        E.current_y--;
        E.current_x = E.row[E.current_y].size;
      }
      break;
    case ARROW_RIGHT:
      if (E.row && E.current_x < row->size) {
        E.current_x++;
      } else if (row && E.current_x == row->size) {
        E.current_y++;
        E.current_x = 0;
      }
      break;
    case ARROW_UP:
      if (E.current_y != 0) {
        E.current_y--;
      }
      break;
    case ARROW_DOWN:
      if (E.current_y < E.num_rows) {  // can only go down to bottom of file
        E.current_y++;
      }
      break;
  }
  // Get the line again since current_y could have changed an thus
  // we may be referring to a different row
  row = (E.current_y >= E.num_rows) ? NULL : &E.row[E.current_y];
  int row_length = row ? row->size : 0;
  if (E.current_x > row_length) {
    E.current_x = row_length;
  }
}

void read_and_process_key() {
  int c = read_key();
  switch (c) {
    case CTRL_KEY('q'):  // If pressed ctrl+q
      clear_screen_raw();
      exit(0);
      break;

    case HOME:
      E.current_x = 0;
      break;

    case END:
      if (E.current_y < E.num_rows) {
        E.current_x = E.row[E.current_y].size;
      }
      break;

    case PAGE_UP:
    case PAGE_DOWN: {
      if (c == PAGE_UP) {
        E.current_y = E.row_offset; 
      } else if (c == PAGE_DOWN) {
        E.current_y = E.row_offset + E.screen_rows - 1;
        if (E.current_y > E.num_rows) {
          E.current_y = E.num_rows;
        }
      }
      int times = E.screen_rows;
      while (times--) {
        move_cursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
      }
    }
    break;
    
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
  E.current_y = 0;
  E.render_x = 0;
  E.render_y = 0;
  E.num_rows = 0;
  E.row_offset = 0;
  E.col_offset = 0;
  E.row = NULL;
  E.filename = NULL;
  E.statusmsg[0] = '\0';
  E.statusmsg_time = 0;
  if (get_window_size(&E.screen_rows, &E.screen_cols) == -1) {
    die("get_window_size");
  }
  E.screen_rows -= 2;  // leave room for the status bar & msg bar
}

int main(int argc, char *argv[]) {
  enable_raw_mode();
  init_editor();

  if (argc >= 2) {
    file_open(argv[1]);
  }

  set_status_message("Quit: Ctrl+Q");

  while (1) {
    full_repaint();
    read_and_process_key();
  }

  return 0;
}
