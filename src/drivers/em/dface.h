#include "../common/args.h"
#include "../common/config.h"

#include "input.h"

extern CFGSTRUCT DriverConfig[];
extern ARGPSTRUCT DriverArgs[];

void DoDriverArgs(void);

int InitSound();
void WriteSound(int32 *Buffer, int Count);
int KillSound(void);
int GetSoundBufferCount(void);

void SilenceSound(int s); /* DOS and SDL */

#ifndef EMSCRIPTEN
int InitJoysticks(void);
int KillJoysticks(void);
#else
#define InitJoysticks() 1
#define KillJoysticks() 0
#endif
uint32 *GetJSOr(void);

int InitVideo(FCEUGI *gi);
int KillVideo(void);
void BlitScreen(uint8 *XBuf);

int LoadGame(const char *path);

int FCEUD_NetworkConnect(void);
