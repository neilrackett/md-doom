/**
 * File: emul.h
 * Author: Diego Parrilla Santamaría
 * Date: January 20205, February 2026
 * Copyright: 2025-2026 - GOODDATA LABS SL
 * Description: Header for the ROM emulator core and setup features
 */

#ifndef EMUL_H
#define EMUL_H

/**
 * @brief
 *
 * Launches the ROM emulator application. Initializes terminal interfaces
 * and storage systems, and loads the ROM data from SD. Manages the main
 * loop which includes firmware bypass, user interaction and potential
 * system resets.
 */
/* The user quit and the cartridge code area has been reclaimed, so
 * there is nothing to hand the machine back to: reset the ST, ask its
 * next boot to skip the autostart so it lands on the desktop, put the
 * cartridge image back and restart. Does not return. */
void emul_quit_to_desktop(void);

/* Ask the m68k to reset itself -- repeatedly, for long enough that it
 * cannot miss the sentinel -- then stop core 1 and the audio refill.
 * Shared by both routes out of a session: the quit to the desktop and
 * the jump to the Booster. */
void emul_request_st_reset(void);

void emul_start();

#endif  // EMUL_H
