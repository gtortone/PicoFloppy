/*
 * pico2/speaker.c
 *
 * PC speaker/buzzer control: not fitted on the RP2350 build. Its pin
 * (GPIO 15) carries the FF OSD video sync input instead (pico2/osd.c), so
 * step and insert/eject notifications are silent.
 *
 * This is free and unencumbered software released into the public domain.
 * See the file COPYING for more details, or visit <http://unlicense.org>.
 */

void speaker_init(void)
{
}

void speaker_pulse(void)
{
}

void speaker_notify_insert(unsigned int slotnr)
{
}

void speaker_notify_eject(void)
{
}
