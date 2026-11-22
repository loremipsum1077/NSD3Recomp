// FMOD with no sound output. Every FMOD function the game calls answers
// FMOD_ERR_OUTPUT_INIT ("could not start the sound device"), which the game treats
// as "sound is unavailable" and carries on silently. This is the whole of audio
// until milestone 6 decides between FMOD's own library and a replacement.
//
// One line per function; a function's arguments are ignored, so no signatures are
// needed. All of them return FMOD_RESULT.

#ifdef __APPLE__
#define FMOD_LINK_NAME(symbol) "_" #symbol
#else
#define FMOD_LINK_NAME(symbol) #symbol
#endif

#define FMOD_OFF(symbol)                                           \
    int d3_fmod_##symbol(void) __asm__(FMOD_LINK_NAME(symbol));    \
    int d3_fmod_##symbol(void)                                     \
    {                                                              \
        return 51; /* FMOD_ERR_OUTPUT_INIT */                      \
    }

FMOD_OFF(FMOD_ChannelGroup_AddDSP)
FMOD_OFF(FMOD_ChannelGroup_SetVolume)
FMOD_OFF(FMOD_Channel_AddDSP)
FMOD_OFF(FMOD_Channel_Get3DAttributes)
FMOD_OFF(FMOD_Channel_GetDSPClock)
FMOD_OFF(FMOD_Channel_GetFrequency)
FMOD_OFF(FMOD_Channel_GetIndex)
FMOD_OFF(FMOD_Channel_GetPosition)
FMOD_OFF(FMOD_Channel_GetUserData)
FMOD_OFF(FMOD_Channel_GetVolume)
FMOD_OFF(FMOD_Channel_IsPlaying)
FMOD_OFF(FMOD_Channel_Set3DAttributes)
FMOD_OFF(FMOD_Channel_Set3DDopplerLevel)
FMOD_OFF(FMOD_Channel_Set3DLevel)
FMOD_OFF(FMOD_Channel_Set3DMinMaxDistance)
FMOD_OFF(FMOD_Channel_Set3DOcclusion)
FMOD_OFF(FMOD_Channel_Set3DSpread)
FMOD_OFF(FMOD_Channel_SetChannelGroup)
FMOD_OFF(FMOD_Channel_SetDelay)
FMOD_OFF(FMOD_Channel_SetFrequency)
FMOD_OFF(FMOD_Channel_SetLoopCount)
FMOD_OFF(FMOD_Channel_SetLoopPoints)
FMOD_OFF(FMOD_Channel_SetMixLevelsOutput)
FMOD_OFF(FMOD_Channel_SetMode)
FMOD_OFF(FMOD_Channel_SetPaused)
FMOD_OFF(FMOD_Channel_SetPosition)
FMOD_OFF(FMOD_Channel_SetReverbProperties)
FMOD_OFF(FMOD_Channel_SetUserData)
FMOD_OFF(FMOD_Channel_SetVolume)
FMOD_OFF(FMOD_Channel_Stop)
FMOD_OFF(FMOD_DSP_GetSystemObject)
FMOD_OFF(FMOD_DSP_Release)
FMOD_OFF(FMOD_DSP_SetBypass)
FMOD_OFF(FMOD_DSP_SetParameterFloat)
FMOD_OFF(FMOD_Memory_GetStats)
FMOD_OFF(FMOD_Memory_Initialize)
FMOD_OFF(FMOD_NX_SetThreadAffinity)
FMOD_OFF(FMOD_Sound_GetDefaults)
FMOD_OFF(FMOD_Sound_GetLength)
FMOD_OFF(FMOD_Sound_GetOpenState)
FMOD_OFF(FMOD_Sound_GetSubSound)
FMOD_OFF(FMOD_Sound_GetUserData)
FMOD_OFF(FMOD_Sound_Release)
FMOD_OFF(FMOD_Sound_SetDefaults)
FMOD_OFF(FMOD_Sound_SetLoopCount)
FMOD_OFF(FMOD_Sound_SetMode)
FMOD_OFF(FMOD_Sound_SetUserData)
FMOD_OFF(FMOD_System_Create)
FMOD_OFF(FMOD_System_CreateChannelGroup)
FMOD_OFF(FMOD_System_CreateDSP)
FMOD_OFF(FMOD_System_CreateDSPByType)
FMOD_OFF(FMOD_System_CreateSound)
FMOD_OFF(FMOD_System_CreateStream)
FMOD_OFF(FMOD_System_GetChannel)
FMOD_OFF(FMOD_System_GetMasterChannelGroup)
FMOD_OFF(FMOD_System_GetNumDrivers)
FMOD_OFF(FMOD_System_GetReverbProperties)
FMOD_OFF(FMOD_System_GetSoftwareFormat)
FMOD_OFF(FMOD_System_GetVersion)
FMOD_OFF(FMOD_System_Init)
FMOD_OFF(FMOD_System_PlaySound)
FMOD_OFF(FMOD_System_Release)
FMOD_OFF(FMOD_System_Set3DListenerAttributes)
FMOD_OFF(FMOD_System_Set3DRolloffCallback)
FMOD_OFF(FMOD_System_Set3DSettings)
FMOD_OFF(FMOD_System_SetCallback)
FMOD_OFF(FMOD_System_SetDriver)
FMOD_OFF(FMOD_System_SetFileSystem)
FMOD_OFF(FMOD_System_SetOutput)
FMOD_OFF(FMOD_System_SetReverbProperties)
FMOD_OFF(FMOD_System_SetSoftwareChannels)
FMOD_OFF(FMOD_System_SetSoftwareFormat)
FMOD_OFF(FMOD_System_SetStreamBufferSize)
FMOD_OFF(FMOD_System_Update)
