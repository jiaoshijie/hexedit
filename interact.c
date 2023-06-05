/* hexedit -- Hexadecimal Editor for Binary Files
   Copyright (C) 1998 Pixel (Pascal Rigaux)

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2, or (at your option)
   any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.*/
#include "hexedit.h"


static void goto_char(void);
static void goto_sector(void);
static void save_buffer(void);
static void help(void);
static void short_help(void);

static void insert_mode(int key);
static void normal_mode(int key);
static void command_mode(void);
static int get_and_run_command(void);

static void delete_char(void);
static void delete_chars(void);

/*******************************************************************************/
/* interactive functions */
/*******************************************************************************/

static void forward_char(void)
{
  if (!hexOrAscii || cursorOffset)
    move_cursor(+1);
  if (hexOrAscii) cursorOffset = (cursorOffset + 1) % 2;
}

static void backward_char(void)
{
  if (!hexOrAscii || !cursorOffset)
    move_cursor(-1);
  if (hexOrAscii) cursorOffset = (cursorOffset + 1) % 2;
}

static void next_line(void)
{
  move_cursor(+lineLength);
}

static void previous_line(void)
{
  move_cursor(-lineLength);
}

static void forward_chars(void)
{
  move_cursor(+blocSize);
}

static void backward_chars(void)
{
  move_cursor(-blocSize);
}

static void next_lines(void)
{
  move_cursor(+lineLength * blocSize);
}

static void previous_lines(void)
{
  move_cursor(-lineLength * blocSize);
}

static void beginning_of_line(void)
{
  cursorOffset = 0;
  move_cursor(-(cursor % lineLength));
}

static void end_of_line(void)
{
  cursorOffset = 0;
  if (!move_cursor(lineLength - 1 - cursor % lineLength))
    move_cursor(nbBytes - cursor);
}

static void scroll_up(void)
{
  move_base(+page);

  if (mark_set)
    updateMarked();
}

static void scroll_down(void)
{
  move_base(-page);

  if (mark_set)
    updateMarked();
}

static void beginning_of_buffer(void)
{
  cursorOffset = 0;
  set_cursor(0);
}

static void end_of_buffer(void)
{
  INT s = getfilesize();
  cursorOffset = 0;
  if (mode == bySector) set_base(myfloor(s, page));
  set_cursor(s);
}

static void suspend(void) { kill(getpid(), SIGTSTP); }
static void undo(void) { discardEdited(); readFile(); }
static void quoted_insert(void) { setTo(getch()); }
static void toggle(void) { hexOrAscii = (hexOrAscii + 1) % 2; }

static void recenter(void)
{
  if (cursor) {
    base = base + cursor;
    cursor = 0;
    readFile();
  }
}

static void find_file(void)
{
  if (!ask_about_save_and_redisplay()) return;
  if (!findFile()) { displayMessageAndWaitForKey("No such file or directory"); return; }
  openFile();
  readFile();
}

static void redisplay(void) { clear(); }

static void delete_backward_char(void)
{
  backward_char();
  removeFromEdited(base + cursor, 1);
  readFile();
  cursorOffset = 0;
  if (!tryloc(base + cursor)) end_of_buffer();
}

static void delete_backward_chars(void)
{
  backward_chars();
  removeFromEdited(base + cursor, blocSize);
  readFile();
  cursorOffset = 0;
  if (!tryloc(base + cursor)) end_of_buffer();
}

static void truncate_file(void)
{
  displayOneLineMessage("Really truncate here? (y/N)");
  if (tolower(getch()) == 'y') {
    if (biggestLoc > base+cursor && ftruncate(fd, base+cursor) == -1)
      displayMessageAndWaitForKey(strerror(errno));
    else {
      removeFromEdited(base+cursor, lastEditedLoc - (base+cursor));
      if (mark_set) {
	if (mark_min >= base + cursor || mark_max >= base + cursor)
	  unmarkAll();
      }
      if (biggestLoc > base+cursor)
	biggestLoc = base+cursor;
      readFile();
    }
  }
}

static void firstTimeHelp(void)
{
  static int firstTime = TRUE;

  if (firstTime) {
    firstTime = FALSE;
    short_help();
  }
}

static void set_mark_command(void)
{
  unmarkAll();
  if ((mark_set = not(mark_set))) {
    markIt(cursor);
    mark_min = mark_max = base + cursor;
  }
}


int setTo(int c)
{
  int val;

  if (cursor > nbBytes) return FALSE;
  if (hexOrAscii) {
      if (!isxdigit(c)) return FALSE;
      val = hexCharToInt(c);
      val = cursorOffset ? setLowBits(buffer[cursor], val) : setHighBits(buffer[cursor], val);
  }
  else val = c;

  if (isReadOnly) {
    displayMessageAndWaitForKey("File is read-only!");
  } else {
    setToChar(cursor, val);
    forward_char();
  }
  return TRUE;
}


/****************************************************
 ask_about_* or functions that present a prompt
****************************************************/


int ask_about_save(void)
{
  if (edited) {
    displayOneLineMessage("Save changes (Yes/No/Cancel) ?");

    switch (tolower(getch()))
      {
      case 'y': save_buffer(); break;
      case 'n': discardEdited(); break;

      default:
	return FALSE;
      }
    return TRUE;
  }
  return -TRUE;
}

int ask_about_save_and_redisplay(void)
{
  int b = ask_about_save();
  if (b == TRUE) {
    readFile();
    display();
  }
  return b;
}

void ask_about_save_and_quit(void)
{
  if (ask_about_save()) quit();
}

static void goto_char(void)
{
  INT i;

  displayOneLineMessage("New position ? ");
  ungetstr("0x");
  if (!get_number(&i) || !set_cursor(i)) displayMessageAndWaitForKey("Invalid position!");
}

static void goto_sector(void)
{
  INT i;

  displayOneLineMessage("New sector ? ");
  if (get_number(&i) && set_base(i * SECTOR_SIZE))
    set_cursor(i * SECTOR_SIZE);
  else
    displayMessageAndWaitForKey("Invalid sector!");
}



static void save_buffer(void)
{
  int displayedmessage = FALSE;
  typePage *p, *q;
  for (p = edited; p; p = q) {
    if (LSEEK_(fd, p->base) == -1 || write(fd, p->vals, p->size) == -1)
      if (!displayedmessage) {  /* It would be annoying to display lots of error messages when we can't write. */
	displayMessageAndWaitForKey(strerror(errno));
	displayedmessage = TRUE;
      }
    q = p->next;
    freePage(p);
  }
  edited = NULL;
  if (lastEditedLoc > fileSize) fileSize = lastEditedLoc;
  lastEditedLoc = 0;
  memset(bufferAttr, A_NORMAL, page * sizeof(*bufferAttr));
  if (displayedmessage) {
    displayMessageAndWaitForKey("Unwritten changes have been discarded");
    readFile();
    if (cursor > nbBytes) set_cursor(getfilesize());
  }
  if (mark_set) markSelectedRegion();
}

static void help(void)
{
  char *args[3];
  int status;

  args[0] = "man";
  args[1] = "hexedit";
  args[2] = NULL;
  endwin();
  if (fork() == 0) {
    execvp(args[0], args);
    exit(1);
  }
  wait(&status);
  refresh();
  raw();
}

static void short_help(void)
{
  displayMessageAndWaitForKey("Unknown command, press F1 for help");
}



static void insert_mode(int key)
{
  switch (key) {
    case CTRL('C'):
    case CTRL('j'):
    case CTRL('['):  // also works with 'ESC'
      current_mode = 0;
      break;
    case KEY_BACKSPACE:
    case CTRL('H'):
      delete_backward_char();
      break;
    case CTRL('W'):
      delete_backward_chars();
      break;
    default:
      if ((key >= 256 || !setTo(key))) firstTimeHelp();
  }
}

static void normal_mode(int key) {
  int ch;

  switch (key) {
    case CTRL('C'):
      displayMessageAndWaitForKey("Use `:q` to quit hexedit!!!");
      break;

    case 'i':
      current_mode = 1;
      break;
    case 'A':
      end_of_buffer();
      current_mode = 1;
      break;

    case 'h':
      backward_char();
      break;
    case 'j':
      next_line();
      break;
    case 'k':
      previous_line();
      break;
    case 'l':
      forward_char();
      break;
    case 'b':
      backward_chars();
      break;
    case 'w':
      forward_chars();
      break;
    case CTRL('D'):
      next_lines();
      break;
    case CTRL('U'):
      previous_lines();
      break;
    case '0':
      beginning_of_line();
      break;
    case '$':
      end_of_line();
      break;
    case CTRL('F'):
      scroll_up();
      break;
    case CTRL('B'):
      scroll_down();
      break;

    case 'g':
      ch = getch();
      switch (ch) {
        case 'g':
          beginning_of_buffer();
          break;
        default:
          firstTimeHelp();
      }
      break;
    case 'G':
      end_of_buffer();
      break;

    case 'Z':
      ch = getch();
      switch (ch) {
        case 'Q':
          quit();
          break;
        case 'Z':
          ask_about_save_and_quit();
          break;
        default:
          // TODO: maybe change infomation
          firstTimeHelp();
      }
      break;

    case CTRL('Z'):
      suspend();
      break;
    case 'u':
      undo();
      break;

    case 'r':
      quoted_insert();
      break;

    case 'R':
      if (!mark_set) {
        set_mark_command();
      }
      fill_with_string();
      set_mark_command();
      break;

    case CTRL('W'):
      ch = getch();
      switch (ch) {
        case CTRL('W'):
        case 'w':
          toggle();
          break;
        default:
          firstTimeHelp();
      }
      break;

    case '/':
      search_forward();
      break;
    case '?':
      search_backward();
      break;

    case 'f':
      goto_char();
      break;

    case 'z':
      ch = getch();
      switch (ch) {
        case 'z':
          recenter();
          break;
        default:
          firstTimeHelp();
      }
      break;

    case KEY_F(1):
      help();
      break;

    case CTRL('L'):
      redisplay();
      break;

    case 'x':
      delete_char();
      break;

    case 'd':
      ch = getch();
      switch (ch) {
        case 'w':
          delete_chars();
          break;
        case 'G':
          truncate_file();
          break;
        default:
          firstTimeHelp();
      }
      break;

    case 'v':
      set_mark_command();
      break;

    case 'y':
      copy_region();
      break;

    case 'p':
      yank();
      break;

    case '\n':
    case '\r':
    case KEY_ENTER:
      if (mode == bySector) goto_sector(); else goto_char();
      break;

    case ':':
      command_mode();
      break;

    case 'c':
      ch = getch();
      switch (ch) {
        case 'c':
          fill_with_string();
          break;
        default:
          firstTimeHelp();
      }
      break;
    case 'C':
      fill_with_string();
      break;

    default:
  }
}

static void command_mode(void) {
  int ch;
  displayOneLineMessage("Command: ");
  if (get_and_run_command() != 0) displayMessageAndWaitForKey("Invalid command!");
}

static int get_and_run_command(void) {
  char tmp[BLOCK_SEARCH_SIZE];
  echo();
  getnstr(tmp, BLOCK_SEARCH_SIZE - 1);
  noecho();

  if (streq(tmp, "f") || streq(tmp, "find")) find_file();
  else if (streq(tmp, "w") || streq(tmp, "write")) save_buffer();
  else if (streq(tmp, "p") || streq(tmp, "paste")) yank_to_a_file();
  else if (streq(tmp, "q") || streq(tmp, "quit")) ask_about_save_and_quit();
  else if (streq(tmp, "q!") || streq(tmp, "quit!")) quit();
  else if (streq(tmp, "wq")) {save_buffer(); quit();}
  else return -1;

  return 0;
}

int vi_key_to_function(int key) {
  oldcursor = cursor;
  oldcursorOffset = cursorOffset;
  oldbase = base;

  switch (key) {
    case KEY_RESIZE:
      /*Close and refresh window to get new size*/
      endwin();
      refresh();

      /*Reset to trigger recalculation*/
      lineLength = 0;

      clear();
      initDisplay();
      readFile();
      display();
      break;

    case ERR:
      break;

    default:
      if (current_mode == 0) {
        normal_mode(key);
      } else if (current_mode == 1) {
        insert_mode(key);
      } else {
        // NOTE: unreadchable
        firstTimeHelp();
      }
  }

  return TRUE;
}

static void delete_char(void)
{
  removeFromEdited(base + cursor, 1);
  readFile();
  cursorOffset = 0;
  if (!tryloc(base + cursor)) end_of_buffer();
}

static void delete_chars(void)
{
  removeFromEdited(base + cursor, blocSize);
  readFile();
  cursorOffset = 0;
  if (!tryloc(base + cursor)) end_of_buffer();
}
