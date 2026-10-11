# SDK symbols the game links against that the port does not implement yet

Generated from `build/link_stubs.c` (written by `tools/link_resolve.py` on every Xbox build). Each entry is
currently bound to a logging catch-all; implementing it in `xbox/port/` removes it from the list on the next build.

Total: 214 (207 functions, 7 data). Snapshot 2026-10-11.

## AX (audio) (49)

`AIInit`, `AIReset`, `AXARTInit`, `AXARTQuit`, `AXARTServiceSounds`, `AXAcquireVoice`, `AXFreeVoice`, `AXGetDspCycles`, `AXGetMaxDspCycles`, `AXInitEx`, `AXQuit`, `AXRegisterAuxACallback`, `AXRegisterAuxBCallback`, `AXRegisterCallback`, `AXSetCompressor`, `AXSetMode`, `AXSetVoiceAddr`, `AXSetVoiceAdpcm`, `AXSetVoiceAdpcmLoop`, `AXSetVoiceEndAddr`, `AXSetVoiceLoop`, `AXSetVoiceLoopAddr`, `AXSetVoiceLpf`, `AXSetVoiceLpfCoefs`, `AXSetVoicePriority`, `AXSetVoiceSrc`, `AXSetVoiceSrcRatio`, `AXSetVoiceSrcType`, `AXSetVoiceState`, `AXSetVoiceType`, `MIXInit`, `MIXInitChannel`, `MIXQuit`, `MIXReleaseChannel`, `MIXSetAuxA`, `MIXSetAuxB`, `MIXSetInput`, `MIXSetPan`, `MIXSetSPan`, `MIXSetSoundMode`, `MIXUpdateSettings`, `SYNGetActiveNotes`, `SYNInit`, `SYNInitSynth`, `SYNMidiInput`, `SYNQuit`, `SYNQuitSynth`, `SYNRunAudioFrame`, `SYNSetMasterVolume`

## OS (43)

`DBIsDebuggerPresent`, `OSCalendarTimeToTicks`, `OSCancelThread`, `OSCreateThread`, `OSExitThread`, `OSGetConsoleSimulatedMemSize`, `OSGetConsoleType`, `OSGetCurrentThread`, `OSGetFontEncode`, `OSGetFontTexture`, `OSGetProgressiveMode`, `OSGetResetButtonState`, `OSGetSoundMode`, `OSGetTick`, `OSGetTime`, `OSInit`, `OSInitAlarm`, `OSInitFont`, `OSInitSemaphore`, `OSInitStopwatch`, `OSInitThreadQueue`, `OSResetStopwatch`, `OSResetSystem`, `OSResumeThread`, `OSSetProgressiveMode`, `OSSetSaveRegion`, `OSSetSoundMode`, `OSSignalSemaphore`, `OSSleepThread`, `OSStartStopwatch`, `OSStopStopwatch`, `OSSuspendThread`, `OSTicksToCalendarTime`, `OSWaitSemaphore`, `OSWakeupThread`, `PPCMfpmc1`, `PPCMtmmcr0`, `PPCMtmmcr1`, `PPCMtpmc1`, `PPCMtpmc2`, `PPCMtpmc3`, `PPCMtpmc4`, `PPCSync`

## game-side (declaration/definition mismatch, dead code, debug) (26)

`?Em32_fs_tbl@@3UFootShadowTbl@@A`, `?EmCatchSubSet@@YAXPAVcEm@@0MKMMMP6AXPAVcSubChar@@@Z@Z`, `?ExceptionInit@@YAXXZ`, `?QuakeKill@@YAXE@Z`, `?isTrans@cModel@@QAEHXZ`, `?r222_k212@@3MB`, `?r224_zero@@3MB`, `?setRoomEtcBreakDisp@@YAHHHH@Z`, `?subCharLiveCheck@cPlayer@@QAEXXZ`, `LCEnable`, `PCclose`, `PCcreat`, `PCinit`, `PClseek`, `PCopen`, `PCread`, `PCwrite`, `PSMTX44Identity`, `SEQInit`, `SEQQuit`, `SEQRunAudioFrame`, `TEXGet`, `em21_trap_cam`, `em2a_rescue_cam`, `r222_k212`, `r224_zero`

## AXFX (audio effects) (21)

`AXFXChorusCallback`, `AXFXChorusInit`, `AXFXChorusSettings`, `AXFXChorusShutdown`, `AXFXDelayCallback`, `AXFXDelayInit`, `AXFXDelaySettings`, `AXFXDelayShutdown`, `AXFXReverbHiCallback`, `AXFXReverbHiCallbackDpl2`, `AXFXReverbHiInit`, `AXFXReverbHiInitDpl2`, `AXFXReverbHiSettings`, `AXFXReverbHiSettingsDpl2`, `AXFXReverbHiShutdown`, `AXFXReverbHiShutdownDpl2`, `AXFXReverbStdCallback`, `AXFXReverbStdInit`, `AXFXReverbStdSettings`, `AXFXReverbStdShutdown`, `AXFXSetHooks`

## CARD (memory card) (17)

`CARDCheckAsync`, `CARDClose`, `CARDCreateAsync`, `CARDDeleteAsync`, `CARDFormatAsync`, `CARDFreeBlocks`, `CARDGetResultCode`, `CARDGetSerialNo`, `CARDGetStatus`, `CARDInit`, `CARDMountAsync`, `CARDOpen`, `CARDProbeEx`, `CARDReadAsync`, `CARDSetStatusAsync`, `CARDUnmount`, `CARDWriteAsync`

## CRI movies (16)

`ADXGC_SetupDvdFs`, `ADXM_ExecMain`, `ADXM_SetCbErr`, `ADXT_SetOutputMono`, `mwPlyCalcWorkCprmSfd`, `mwPlyCreateSofdec`, `mwPlyFxCnvFrmARGB8888`, `mwPlyFxCnvFrmY84C44`, `mwPlyFxSetOutBufPitchHeight`, `mwPlyFxSetOutBufSize`, `mwPlyGetCurFrm`, `mwPlyGetHdrInf`, `mwPlyGetNumSkipDec`, `mwPlyGetNumSkipDisp`, `mwPlyInitSfdFx`, `mwPlyRelCurFrm`

## DVD (15)

`DVDCancelAll`, `DVDCancelAsync`, `DVDChangeDiskAsync`, `DVDClose`, `DVDCompareDiskID`, `DVDConvertPathToEntrynum`, `DVDFastOpen`, `DVDGenerateDiskID`, `DVDGetCommandBlockStatus`, `DVDGetCurrentDiskID`, `DVDGetDriveStatus`, `DVDGetFSTLocation`, `DVDGetTransferredSize`, `DVDOpen`, `DVDReadAsyncPrio`

## VI (video) (10)

`VIConfigure`, `VIFlush`, `VIGetDTVStatus`, `VIGetNextField`, `VIGetTvFormat`, `VIInit`, `VISetBlack`, `VISetNextFrameBuffer`, `VISetPostRetraceCallback`, `VIWaitForRetrace`

## PAD (7)

`PADClamp`, `PADControlMotor`, `PADInit`, `PADRead`, `PADRecalibrate`, `PADReset`, `PADSetAnalogMode`

## ARAM (6)

`ARAlloc`, `ARGetBaseAddress`, `ARInit`, `ARQFlushQueue`, `ARQInit`, `ARQPostRequest`

## GX (graphics) (4)

`GXInit`, `GXSetCurrentGXThread`, `GXSetDrawSync`, `GXSetDrawSyncCallback`
