/*
 * Reads the device's buttons straight from evdev while the user holds the
 * quick menu's new button combination (see chordcap.c).
 */

#ifndef CHORDCAP_H
#define CHORDCAP_H

#include <stdbool.h>

void CHORD_open(void);
void CHORD_close(void);
void CHORD_poll(void);                  /* read what is pending */
int  CHORD_held(int* codes, int max);   /* evdev key codes held now */
/* A d-pad hat or an analog trigger is pressed: buttons with no key code. */
bool CHORD_axisHeld(void);
/* Power, volume and the menu key belong to the system. */
bool CHORD_isSystemKey(int code);

#endif
