/*
 * libmsettings for the spruce build: the same functions, touching nothing.
 *
 * NextUI's UI code calls libmsettings to apply NextUI's brightness, volume and
 * colour settings when it starts, and to change them from its own controls. On
 * spruce those belong to spruce - its own settings file and its own scripts set
 * them - so the app must leave the hardware exactly as spruce left it. Getters
 * report neutral values and every setter does nothing.
 *
 * Defined without msettings.h: the tg5040 and h700 headers differ (h700 makes
 * the mute group static inline), and this file covers both.
 */

void InitSettings(void) {}
void QuitSettings(void) {}
int InitializedSettings(void) { return 1; }

int GetBrightness(void) { return 5; }
int GetColortemp(void) { return 20; }
int GetContrast(void) { return 0; }
int GetSaturation(void) { return 0; }
int GetExposure(void) { return 0; }
int GetDisplayCalEnabled(void) { return 0; }
int GetDisplayCalRedGain(void) { return 100; }
int GetDisplayCalGreenGain(void) { return 100; }
int GetDisplayCalBlueGain(void) { return 100; }
int GetVolume(void) { return 10; }

void SetRawBrightness(int value) { (void)value; }
void SetRawColortemp(int value) { (void)value; }
void SetRawContrast(int value) { (void)value; }
void SetRawSaturation(int value) { (void)value; }
void SetRawExposure(int value) { (void)value; }
void SetRawDisplayCal(int enabled, int red, int green, int blue) { (void)enabled; (void)red; (void)green; (void)blue; }
void SetRawVolume(int value) { (void)value; }

void SetBrightness(int value) { (void)value; }
void SetColortemp(int value) { (void)value; }
void SetContrast(int value) { (void)value; }
void SetSaturation(int value) { (void)value; }
void SetExposure(int value) { (void)value; }
void SetDisplayCalEnabled(int value) { (void)value; }
void SetDisplayCalRedGain(int value) { (void)value; }
void SetDisplayCalGreenGain(int value) { (void)value; }
void SetDisplayCalBlueGain(int value) { (void)value; }
void SetVolume(int value) { (void)value; }

int GetJack(void) { return 0; }
void SetJack(int value) { (void)value; }
int GetAudioSink(void) { return 0; }
void SetAudioSink(int value) { (void)value; }
int GetHDMI(void) { return 0; }
void SetHDMI(int value) { (void)value; }
int GetMute(void) { return 0; }
void SetMute(int value) { (void)value; }

/* "No change" for the muted values, as the h700 header's inline versions say. */
int GetMutedBrightness(void) { return -69; }
int GetMutedColortemp(void) { return -69; }
int GetMutedContrast(void) { return -69; }
int GetMutedSaturation(void) { return -69; }
int GetMutedExposure(void) { return -69; }
int GetMutedVolume(void) { return -69; }
int GetMuteDisablesDpad(void) { return 0; }
int GetMuteEmulatesJoystick(void) { return 0; }
int GetMuteTurboA(void) { return 0; }
int GetMuteTurboB(void) { return 0; }
int GetMuteTurboX(void) { return 0; }
int GetMuteTurboY(void) { return 0; }
int GetMuteTurboL1(void) { return 0; }
int GetMuteTurboL2(void) { return 0; }
int GetMuteTurboR1(void) { return 0; }
int GetMuteTurboR2(void) { return 0; }

void SetMutedBrightness(int value) { (void)value; }
void SetMutedColortemp(int value) { (void)value; }
void SetMutedContrast(int value) { (void)value; }
void SetMutedSaturation(int value) { (void)value; }
void SetMutedExposure(int value) { (void)value; }
void SetMutedVolume(int value) { (void)value; }
void SetMuteDisablesDpad(int value) { (void)value; }
void SetMuteEmulatesJoystick(int value) { (void)value; }
void SetMuteTurboA(int value) { (void)value; }
void SetMuteTurboB(int value) { (void)value; }
void SetMuteTurboX(int value) { (void)value; }
void SetMuteTurboY(int value) { (void)value; }
void SetMuteTurboL1(int value) { (void)value; }
void SetMuteTurboL2(int value) { (void)value; }
void SetMuteTurboR1(int value) { (void)value; }
void SetMuteTurboR2(int value) { (void)value; }
