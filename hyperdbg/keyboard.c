/*
  Copyright notice
  ================
  
  Copyright (C) 2010
      Lorenzo  Martignoni <martignlo@gmail.com>
      Roberto  Paleari    <roberto.paleari@gmail.com>
      Aristide Fattori    <joystick@security.dico.unimi.it>
  
  This program is free software: you can redistribute it and/or modify it under
  the terms of the GNU General Public License as published by the Free Software
  Foundation, either version 3 of the License, or (at your option) any later
  version.
  
  HyperDbg is distributed in the hope that it will be useful, but WITHOUT ANY
  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
  A PARTICULAR PURPOSE.  See the GNU General Public License for more details.
  
  You should have received a copy of the GNU General Public License along with
  this program. If not, see <http://www.gnu.org/licenses/>.
  
*/

/* 
  Useful references:
  * http://www.beyondlogic.org/keyboard/keybrd.htm
  * http://www.win.tue.nl/~aeb/linux/kbd/scancodes-1.html
*/

#include <ntddk.h>
#include "keyboard.h"
#include "scancode.h"
#include "vmmstring.h"

/* ################ */
/* #### MACROS #### */
/* ################ */

#define POLL_STATUS_ITERATIONS 12000

/* 
   8042 Status Register (port 64h read)

	|7|6|5|4|3|2|1|0|  8042 Status Register
	 | | | | | | | `---- output register (60h) has data for system
	 | | | | | | `----- input register (60h/64h) has data for 8042
	 | | | | | `------ system flag (set to 0 after power on reset)
	 | | | | `------- data in input register is command (1) or data (0)
	 | | | `-------- 1=keyboard enabled, 0=keyboard disabled (via switch)
	 | | `--------- 1=transmit timeout (data transmit not complete)
	 | `---------- 1=receive timeout (data transmit not complete)
	 `----------- 1=even parity rec'd, 0=odd parity rec'd (should be odd)
 */

/* Status register bits */   
#define KEYB_STATUS_OBUFFER_FULL        (1 << 0)
#define KEYB_STATUS_IBUFFER_FULL        (1 << 1)
#define KEYB_STATUS_TRANSMIT_TIMEOUT    (1 << 5)
#define KEYB_STATUS_PARITY_ERROR        (1 << 7)

/* i8042 Commands */
#define KEYB_COMMAND_WRITE_OUTPUT      0xd2
#define KEYB_COMMAND_DISABLE_KEYBOARD  0xad
#define KEYB_COMMAND_ENABLE_KEYBOARD   0xae
#define KEYB_COMMAND_DISABLE_MOUSE     0xa7
#define KEYB_COMMAND_ENABLE_MOUSE      0xa8

/* Scancode flags */
#define IS_SCANCODE_RELEASE(c) (c & 0x80)
#define SCANCODE_RELEASE_FLAG  0x80

/* ################# */
/* #### GLOBALS #### */
/* ################# */

/* Current keyboard status. It is exported by keyboard.h */
KEYBOARD_STATUS keyboard_status;

/* ########################## */
/* #### LOCAL PROTOTYPES #### */
/* ########################## */

static NTSTATUS i8042ReadKeyboardData(PUCHAR pc, PBOOLEAN pisMouse);
static BOOLEAN  i8042WriteKeyboardData(PUCHAR addr, UCHAR data);

/* ################ */
/* #### BODIES #### */
/* ################ */

static NTSTATUS i8042ReadKeyboardData(PUCHAR pc, PBOOLEAN pisMouse)
{
  UCHAR port_status;

  port_status = READ_PORT_UCHAR(KEYB_REGISTER_STATUS);

  if (port_status & KEYB_STATUS_OBUFFER_FULL) {
    /* Data is available */
    *pc = READ_PORT_UCHAR(KEYB_REGISTER_DATA);

    /* Check if data is valid (i.e., no timeout, no parity error) */
    if ((port_status & KEYB_STATUS_PARITY_ERROR) == 0) {
      /* Check if this is a mouse event or not */
      *pisMouse = (port_status & KEYB_STATUS_TRANSMIT_TIMEOUT) != 0;
      return STATUS_SUCCESS;
    }
  }

  return STATUS_UNSUCCESSFUL;
}

static BOOLEAN i8042WriteKeyboardData(PUCHAR addr, UCHAR data)
{
  ULONG counter;

  counter = POLL_STATUS_ITERATIONS;
  while ((KEYB_STATUS_IBUFFER_FULL & READ_PORT_UCHAR(KEYB_REGISTER_STATUS)) &&
	 (counter--)) {
    KeStallExecutionProcessor(1);
  }

  if (counter) {
    WRITE_PORT_UCHAR(addr, data);
    return TRUE;
  }

  return FALSE;
}

/* Inspired from ReactOS's i8042 keyboard driver */
NTSTATUS KeyboardReadKeystroke(PUCHAR pc, CHAR unget, PBOOLEAN pisMouse)
{
  ULONG counter;
  UCHAR port_status, scancode;
  NTSTATUS r;

  counter = POLL_STATUS_ITERATIONS;
  while (counter) {
    port_status = READ_PORT_UCHAR(KEYB_REGISTER_STATUS);

    r = i8042ReadKeyboardData(&scancode, pisMouse);

    if (NT_SUCCESS(r)) {
      break;
    }

    KeStallExecutionProcessor(1);    

    counter--;
  }

  if (counter == 0) {
    return STATUS_UNSUCCESSFUL;
  }

  if (unget) {
    /* Echo back the scancode */
    i8042WriteKeyboardData(KEYB_REGISTER_COMMAND, KEYB_COMMAND_DISABLE_KEYBOARD);
    i8042WriteKeyboardData(KEYB_REGISTER_COMMAND, KEYB_COMMAND_WRITE_OUTPUT);
    i8042WriteKeyboardData(KEYB_REGISTER_DATA, scancode);
    i8042WriteKeyboardData(KEYB_REGISTER_COMMAND, KEYB_COMMAND_ENABLE_KEYBOARD);
  }

  *pc = scancode;

  return STATUS_SUCCESS;
}

/* Translate a scancode to the corresponding keycode. Keyboard errors are
   silently ignored. */
UCHAR KeyboardScancodeToKeycode(UCHAR c)
{
  BOOLEAN handled;

  handled = FALSE;

  /* At first we check if we are pressing or releasing one of {lshift, lctrl,
     lalt, rshift}.

     NOTE: rctrl and ralt are omitted, as these are escaped scancodes (e.g.,
     ralt = e0 38) */
  switch(c & ~SCANCODE_RELEASE_FLAG) {
  case 0x1d:
    keyboard_status.lctrl = IS_SCANCODE_RELEASE(c) ? FALSE : TRUE;
    handled = TRUE;
    break;
  case 0x2a:
    keyboard_status.lshift = IS_SCANCODE_RELEASE(c) ? FALSE : TRUE;
    handled = TRUE;
    break;
  case 0x36:
    keyboard_status.rshift = IS_SCANCODE_RELEASE(c) ? FALSE : TRUE;
    handled = TRUE;
    break;
  case 0x38:
    keyboard_status.lalt = IS_SCANCODE_RELEASE(c) ? FALSE : TRUE;
    handled = TRUE;
    break;
  default:
    break;
  }

  if (handled)
    return 0;

  /* No need to do anything else for every other released key */
  if IS_SCANCODE_RELEASE(c)
    return 0;

  /* Else, ignore errors and acks, let the guest OS handle them if needed */
  switch(c) {
  case 0x00: /* KBD ERROR -> ignore */
  case 0xaa: /* BAT OK -> ignore */
  case 0xee: /* ECHO CMD RES -> ignore */
  case 0xfa: /* ACK FROM KBD -> ignore */
  case 0xfc: /* BAT ERROR -> ignore? */
  case 0xfd: /* INTERNAL FAILURE -> ignore? */
  case 0xfe: /* NACK -> ignore? */
  case 0xff: /* KBD ERROR -> ignore */
    handled = TRUE;
  }

  if (handled)
    return 0;

  /* Try  the scancode to the corresponding keycode */
  if((keyboard_status.lshift || keyboard_status.rshift) && scancodes_map[c] > 0x2f && scancodes_map[c] < 0x3a) {
    /* Map special chars above numbers, us-std keymap */
    switch(scancodes_map[c]) {
    case '1':
      return (UCHAR)'!';
    case '2':
      return (UCHAR)'@';
    case '3':
      return (UCHAR)'#';
    case '4':
      return (UCHAR)'$';
    case '5':
      return (UCHAR)'%';
    case '6':
      return (UCHAR)'^';
    case '7':
      return (UCHAR)'&';
    case '8':
      return (UCHAR)'*';
    case '9':
      return (UCHAR)'(';
    case '0':
      return (UCHAR)')';
    }
  }

  if((keyboard_status.lshift || keyboard_status.rshift) && scancodes_map[c] < 0x7b && scancodes_map[c] > 0x60)
    return vmm_toupper(scancodes_map[c]);
  else
    return scancodes_map[c];
}

NTSTATUS KeyboardSetMouse(BOOLEAN enabled)
{
  UCHAR cmd;

  cmd = enabled ? KEYB_COMMAND_ENABLE_MOUSE : KEYB_COMMAND_DISABLE_MOUSE;

  i8042WriteKeyboardData(KEYB_REGISTER_COMMAND, cmd);

  return STATUS_SUCCESS;
}

NTSTATUS KeyboardInit(VOID)
{
  init_scancodes_map();

  return STATUS_SUCCESS;
}
