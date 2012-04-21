#include <stdio.h>
#include <stdarg.h>
#include <errno.h>

#include "ds.h"
#include "d_main.h"
#include "z_zone.h"
#include "r_local.h"

static char msg[256];

//
// I_Error
//

void I_Error(const char *s, ...)
{
    va_list v;

    va_start(v, s);
    vsprintf(msg, s, v);
    va_end(v);

    consoleDemoInit();
    iprintf(msg);

    while (1) { swiWaitForVBlank(); }
}

//
// I_Printf
//

void I_Printf(const char *s, ...)
{
    va_list v;

    va_start(v, s);
    vsprintf(msg, s, v);
    va_end(v);

    iprintf(msg);
}

//
// I_PrintWait
//

void I_PrintWait(const char *s, ...)
{
    va_list v;
    int keys = 0;

    va_start(v, s);
    vsprintf(msg, s, v);
    va_end(v);

    iprintf(msg);

    while(!(keys & KEY_START))
    {
        scanKeys();
        keys = keysDown();
        swiWaitForVBlank();
    }
}

//
// I_FilePath
//

const char* I_FilePath(const char* file)
{
    static char fname[256];

    sprintf(fname, "fat0:/%s", file);
    return fname;
}

//
// I_ReadFile
//

int I_ReadFile(char const* name, byte** buffer)
{
    FILE *fp;

    errno = 0;
    
    if((fp = fopen(I_FilePath(name), "rb")))
    {
        size_t length;

        fseek(fp, 0, SEEK_END);
        length = ftell(fp);
        fseek(fp, 0, SEEK_SET);

        *buffer = Z_Malloc(length, PU_STATIC, 0);
      
        if(fread(*buffer, 1, length, fp) == length)
        {
            fclose(fp);
            return length;
        }
        
        fclose(fp);
   }
   
   return -1;
}

//
// I_FileLength
//

long I_FileLength(FILE *handle)
{ 
    long savedpos;
    long length;

    // save the current position in the file
    savedpos = ftell(handle);
    
    // jump to the end and find the length
    fseek(handle, 0, SEEK_END);
    length = ftell(handle);

    // go back to the old location
    fseek(handle, savedpos, SEEK_SET);

    return length;
}

//
// I_FileExists
//

int I_FileExists(char *filename)
{
    FILE *fstream;

    fstream = fopen(I_FilePath(filename), "r");

    if (fstream != NULL)
    {
        fclose(fstream);
        return 1;
    }
    else
    {
        // If we can't open because the file is a directory, the 
        // "file" exists at least!

        if(errno == 21)
            return 2;
    }

    return 0;
}

//
// I_Init
//

void I_Init(void)
{
    glInit();
    fatInitDefault();
    irqInit();
    irqEnable(IRQ_VBLANK);
    irqSet(IRQ_VBLANK, 0);

    REG_POWERCNT    = POWER_3D_CORE | POWER_MATRIX | POWER_LCD | POWER_2D_B | POWER_SWAP_LCDS;
    REG_DISPCNT     = MODE_0_3D;
    REG_DISPCNT_SUB = MODE_0_2D | DISPLAY_BG1_ACTIVE;
    VRAM_A_CR       = VRAM_ENABLE | VRAM_A_TEXTURE;
    VRAM_B_CR       = VRAM_ENABLE | VRAM_B_TEXTURE;
    VRAM_C_CR       = VRAM_ENABLE | VRAM_C_TEXTURE;
    VRAM_D_CR       = VRAM_ENABLE | VRAM_D_TEXTURE;
    VRAM_E_CR       = VRAM_ENABLE | VRAM_E_TEX_PALETTE;
    VRAM_H_CR       = VRAM_ENABLE | VRAM_H_SUB_BG;
    TIMER0_CR       = TIMER_ENABLE | TIMER_DIV_1024;
    TIMER1_CR       = TIMER_ENABLE | TIMER_CASCADE;

    consoleInit(NULL, 0, BgType_Text4bpp, BgSize_T_256x256, 11, 8, false, true);
    consoleClear();
}

//
// I_ClearFrame
//

void I_ClearFrame(void)
{
    while(GFX_BUSY);

    GFX_CONTROL         = GL_FOG | GL_BLEND | GL_TEXTURE_2D;
    GFX_ALPHA_TEST      = 0;
    GFX_CUTOFF_DEPTH    = GL_MAX_DEPTH;
    GFX_CLEAR_COLOR     = 0x1F0000;
    GFX_CLEAR_DEPTH     = GL_MAX_DEPTH;
    GFX_VIEWPORT        = 0xBFFF0000;
    GFX_TEX_FORMAT      = 0;
    GFX_PAL_FORMAT      = 0;
    GFX_POLY_FORMAT     = 0;

    glGlob->activeTexture = -1;
    glGlob->activePalette = -1;

    //
    // make sure there are no push/pops that haven't executed yet
    //
    while(GFX_STATUS & BIT(14))
        GFX_STATUS |= 1 << 15;  // clear push/pop errors or push/pop busy bit never clears

    //
    // pop the projection stack to the top; poping 0 off an empty stack causes an error... weird?
    //
    if((GFX_STATUS & (1 << 13)) != 0)
    {
        MATRIX_CONTROL  = GL_PROJECTION;
        MATRIX_POP      = 1;
    }

    //
    // 31 deep modelview matrix; 32nd entry works but sets error flag
    //
    MATRIX_CONTROL      = GL_MODELVIEW;
    MATRIX_POP          = (GFX_STATUS >> 8) & 0x1F;

    //
    // load identity to all the matrices
    //
    MATRIX_CONTROL      = GL_PROJECTION;
    MATRIX_IDENTITY     = 0;
    MATRIX_CONTROL      = GL_MODELVIEW;
    MATRIX_IDENTITY     = 0;
    MATRIX_CONTROL      = GL_TEXTURE;
    MATRIX_IDENTITY     = 0;
}
//
// I_StartTic
//

void I_StartTic(void)
{
    u16 keys;
    event_t ev;
    
    scanKeys();

    if((keys = keysDown()))
    {
        ev.type = ev_btndown;
        ev.data = keys;
        D_PostEvent(&ev);
    }

    if((keys = keysUp()))
    {
        ev.type = ev_btnup;
        ev.data = keys;
        D_PostEvent(&ev);
    }

    /*if((keys = keysHeld()))
    {
        ev.type = ev_btnheld;
        ev.data = keys;
        D_PostEvent(&ev);
    }*/
}

//
// I_GetTimeTicks
//

int I_GetTimeTicks(void)
{
    return ((TIMER1_DATA * FRACUNIT) + TIMER0_DATA) / 32;
}

//
// I_GetTimeMS
//

static int basetime = 0;

int I_GetTime(void)
{
    uint32 ticks;

    ticks = I_GetTimeTicks();

    if(basetime == 0)
        basetime = ticks;

    ticks -= basetime;

    return (ticks * TICRATE) / 1000;
}

//
// I_GetTimeMS
//
// Same as I_GetTime, but returns time in milliseconds
//

int I_GetTimeMS(void)
{
    uint32 ticks;
    
    ticks = I_GetTimeTicks();
    
    if (basetime == 0)
        basetime = ticks;
    
    return ticks - basetime;
}

//
// I_Sleep
//

void I_Sleep(uint32 ms)
{
    uint32 now;

    now = I_GetTimeTicks();
    while(I_GetTimeTicks() < now + ms);
}

int main(void)
{
    defaultExceptionHandler();
    D_DoomMain();
	return 0;
}
