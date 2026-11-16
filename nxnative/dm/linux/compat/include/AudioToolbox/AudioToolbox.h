// Android port: the AudioToolbox subset the host uses.
//   AudioUnit (an output unit with a render callback) on AAudio: dm/android/compat/audiotoolbox_aaudio.c. Only the output unit, the stream format (interleaved
//   linear PCM) and the render callback are supported, which is all hle_audio.c sets.
//   AudioConverter (AAC → interleaved 16-bit PCM for the SDK's movie library) on MediaCodec: dm/android/compat/audioconverter_mediacodec.c.
#pragma once
#include <stdint.h>
#include <stddef.h>
typedef int32_t OSStatus;
typedef uint32_t UInt32; typedef int32_t SInt32; typedef uint16_t UInt16; typedef int16_t SInt16; typedef uint64_t UInt64; typedef double Float64; typedef float Float32;
typedef uint8_t Boolean;
typedef UInt32 AudioUnitPropertyID, AudioUnitScope, AudioUnitElement, AudioFormatID, AudioFormatFlags, AudioUnitRenderActionFlags, AudioConverterPropertyID;
#define noErr 0
#define kAudioUnitErr_FormatNotSupported (-10868)
#define kAudio_UnimplementedError (-4)

typedef struct { Float64 mSampleRate; AudioFormatID mFormatID; AudioFormatFlags mFormatFlags; UInt32 mBytesPerPacket, mFramesPerPacket, mBytesPerFrame, mChannelsPerFrame, mBitsPerChannel, mReserved; } AudioStreamBasicDescription;
typedef struct { int64_t mStartOffset; UInt32 mVariableFramesInPacket, mDataByteSize; } AudioStreamPacketDescription;
typedef struct { UInt32 mNumberChannels, mDataByteSize; void *mData; } AudioBuffer;
typedef struct { UInt32 mNumberBuffers; AudioBuffer mBuffers[1]; } AudioBufferList;
typedef struct { Float64 mSampleTime; UInt64 mHostTime; Float64 mRateScalar; UInt64 mWordClockTime; UInt32 mFlags, mReserved; } AudioTimeStamp;
typedef struct { UInt32 componentType, componentSubType, componentManufacturer, componentFlags, componentFlagsMask; } AudioComponentDescription;

#define kAudioFormatLinearPCM 0x6C70636D            /* 'lpcm' */
#define kAudioFormatMPEG4AAC 0x61616320             /* 'aac ' */
#define kAudioFormatFlagIsFloat (1u << 0)
#define kAudioFormatFlagIsBigEndian (1u << 1)
#define kAudioFormatFlagIsSignedInteger (1u << 2)
#define kAudioFormatFlagIsPacked (1u << 3)
#define kAudioFormatFlagIsNonInterleaved (1u << 5)
#define kAudioFormatFlagsNativeEndian 0
#define kLinearPCMFormatFlagIsSignedInteger kAudioFormatFlagIsSignedInteger
#define kLinearPCMFormatFlagIsPacked kAudioFormatFlagIsPacked
#define kLinearPCMFormatFlagIsFloat kAudioFormatFlagIsFloat

#define kAudioUnitType_Output 0x61756F75            /* 'auou' */
#define kAudioUnitSubType_RemoteIO 0x72696F63
#define kAudioUnitSubType_DefaultOutput 0x64656620
#define kAudioUnitManufacturer_Apple 0x6170706C
#define kAudioUnitScope_Global 0
#define kAudioUnitScope_Input 1
#define kAudioUnitScope_Output 2
#define kAudioUnitProperty_StreamFormat 8
#define kAudioUnitProperty_SetRenderCallback 23
#define kAudioUnitProperty_MaximumFramesPerSlice 14
#define kAudioDevicePropertyBufferFrameSize 0x6673697A   /* 'fsiz': a requested buffer size in frames, a hint to AAudio */
#define kAudioOutputUnitProperty_EnableIO 2003
#define kAudioConverterDecompressionMagicCookie 0x646D6763   /* 'dmgc' */

typedef struct dm_audio_unit *AudioUnit, *AudioComponentInstance;
typedef struct dm_audio_component *AudioComponent;
typedef OSStatus (*AURenderCallback)(void *ref, AudioUnitRenderActionFlags *flags, const AudioTimeStamp *ts, UInt32 bus, UInt32 frames, AudioBufferList *data);
typedef struct { AURenderCallback inputProc; void *inputProcRefCon; } AURenderCallbackStruct;

AudioComponent AudioComponentFindNext(AudioComponent after, const AudioComponentDescription *d);
OSStatus AudioComponentInstanceNew(AudioComponent c, AudioComponentInstance *out);
OSStatus AudioComponentInstanceDispose(AudioComponentInstance u);
OSStatus AudioUnitSetProperty(AudioUnit u, AudioUnitPropertyID id, AudioUnitScope scope, AudioUnitElement el, const void *data, UInt32 size);
OSStatus AudioUnitInitialize(AudioUnit u);
OSStatus AudioUnitUninitialize(AudioUnit u);
OSStatus AudioOutputUnitStart(AudioUnit u);
OSStatus AudioOutputUnitStop(AudioUnit u);

typedef struct dm_audio_converter *AudioConverterRef;
typedef OSStatus (*AudioConverterComplexInputDataProc)(AudioConverterRef c, UInt32 *packets, AudioBufferList *data, AudioStreamPacketDescription **desc, void *ref);
OSStatus AudioConverterNew(const AudioStreamBasicDescription *in, const AudioStreamBasicDescription *out, AudioConverterRef *c);
OSStatus AudioConverterSetProperty(AudioConverterRef c, AudioConverterPropertyID id, UInt32 size, const void *data);
OSStatus AudioConverterFillComplexBuffer(AudioConverterRef c, AudioConverterComplexInputDataProc proc, void *ref, UInt32 *packets, AudioBufferList *out, AudioStreamPacketDescription *desc);
OSStatus AudioConverterDispose(AudioConverterRef c);
