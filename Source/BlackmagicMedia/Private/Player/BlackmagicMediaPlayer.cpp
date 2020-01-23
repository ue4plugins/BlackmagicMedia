// Copyright 1998-2019 Epic Games, Inc. All Rights Reserved.

#include "BlackmagicMediaPlayer.h"

#include "Blackmagic.h"
#include "BlackmagicMediaPrivate.h"
#include "BlackmagicMediaSource.h"

#include "HAL/CriticalSection.h"
#include "HAL/PlatformProcess.h"
#include "Misc/ScopeLock.h"
#include "Templates/Atomic.h"

#include "IMediaEventSink.h"
#include "IMediaOptions.h"

#include "MediaIOCoreEncodeTime.h"
#include "MediaIOCoreFileWriter.h"
#include "MediaIOCoreSamples.h"

#include "Engine/GameEngine.h"
#include "Misc/App.h"
#include "Slate/SceneViewport.h"
#include "Stats/Stats2.h"

#include "BlackmagicMediaSource.h"


#define LOCTEXT_NAMESPACE "BlackmagicMediaPlayer"

DECLARE_CYCLE_STAT(TEXT("Blackmagic MediaPlayer Process received frame"), STAT_Blackmagic_MediaPlayer_ProcessReceivedFrame, STATGROUP_Media);


bool bBlackmagicWriteOutputRawDataCmdEnable = false;
static FAutoConsoleCommand BlackmagicWriteOutputRawDataCmd(
	TEXT("Blackmagic.WriteOutputRawData"),
	TEXT("Write Blackmagic raw output buffer to file."),
	FConsoleCommandDelegate::CreateLambda([]() { bBlackmagicWriteOutputRawDataCmdEnable = true;	})
	);

namespace BlackmagicMediaPlayerHelpers
{
	static const int32 ToleratedExtraMaxBufferCount = 2;

	class FBlackmagicMediaPlayerEventCallback : public BlackmagicDesign::IInputEventCallback
	{
	public:
		FBlackmagicMediaPlayerEventCallback(FBlackmagicMediaPlayer* InMediaPlayer, const BlackmagicDesign::FChannelInfo& InChannelInfo)
			: RefCounter(0)
			, ChannelInfo(InChannelInfo)
			, MediaPlayer(InMediaPlayer)
			, MediaState(EMediaState::Closed)
			, PrevousTimespan(FTimespan::Zero())
			, bEncodeTimecodeInTexel(false)
			, LastBitsPerSample(0)
			, LastNumChannels(0)
			, LastSampleRate(0)
			, AudioFrameDropCount(0)
			, MetadataFrameDropCount(0)
			, VideoFrameDropCount(0)
			, LastHasFrameTime(0.0)
			, bReceivedValidFrame(false)
			, bIsTimecodeExpected(false)
			, bHasWarnedMissingTimecode(false)
			, bIsSRGBInput(false)
		{
		}

		bool Initialize(const BlackmagicDesign::FInputChannelOptions& InChannelInfo, bool bInEncodeTimecodeInTexel, int32 InMaxNumAudioFrameBuffer, int32 InMaxNumVideoFrameBuffer, bool bInIsSRGBInput)
		{
			AddRef();

			bEncodeTimecodeInTexel = bInEncodeTimecodeInTexel;
			MaxNumAudioFrameBuffer = InMaxNumAudioFrameBuffer;
			MaxNumVideoFrameBuffer = InMaxNumVideoFrameBuffer;
			bIsTimecodeExpected = InChannelInfo.TimecodeFormat != BlackmagicDesign::ETimecodeFormat::TCF_None;
			bIsSRGBInput = bInIsSRGBInput;

			BlackmagicDesign::ReferencePtr<BlackmagicDesign::IInputEventCallback> SelfRef(this);
			BlackmagicIdendifier = BlackmagicDesign::RegisterCallbackForChannel(ChannelInfo, InChannelInfo, SelfRef);
			MediaState = BlackmagicIdendifier.IsValid() ? EMediaState::Preparing : EMediaState::Error;
			return BlackmagicIdendifier.IsValid();
		}

		void Uninitialize()
		{
			FScopeLock Lock(&CallbackLock);
			MediaPlayer = nullptr;

			if (BlackmagicIdendifier.IsValid())
			{
				MediaState = EMediaState::Stopped;
				BlackmagicDesign::UnregisterCallbackForChannel(ChannelInfo, BlackmagicIdendifier);
				BlackmagicIdendifier = BlackmagicDesign::FUniqueIdentifier();
			}

			Release();
		}

		EMediaState GetMediaState() const { return MediaState; }

		void UpdateAudioTrackFormat(FMediaAudioTrackFormat& OutAudioTrackFormat)
		{
			OutAudioTrackFormat.BitsPerSample = LastBitsPerSample;
			OutAudioTrackFormat.NumChannels = LastNumChannels;
			OutAudioTrackFormat.SampleRate = LastSampleRate;
		}

		void VerifyFrameDropCount_GameThread(const FString& InUrl)
		{
			//Audio buffer
			int32 AudioOverflowCount = FMath::Max(MediaPlayer->Samples->NumAudioSamples() - MaxNumAudioFrameBuffer, 0);
			for (int32 i = 0; i < AudioOverflowCount; ++i)
			{
				MediaPlayer->Samples->PopAudio();
			}

			//Video buffer
			int32 VideoOverflowCount = FMath::Max(MediaPlayer->Samples->NumVideoSamples() - MaxNumVideoFrameBuffer, 0);
			for (int32 i = 0; i < VideoOverflowCount; ++i)
			{
				MediaPlayer->Samples->PopVideo();
			}

			if (MediaPlayer->bVerifyFrameDropCount)
			{
				AudioOverflowCount += FPlatformAtomics::InterlockedExchange(&AudioFrameDropCount, 0);
				if (AudioOverflowCount > 0)
				{
					UE_LOG(LogBlackmagicMedia, Warning, TEXT("Lost %d audio frames on input %s. Frame rate is either too slow or buffering capacity is too small."), AudioOverflowCount, *InUrl);
				}

				VideoOverflowCount += FPlatformAtomics::InterlockedExchange(&VideoFrameDropCount, 0);
				if (VideoOverflowCount > 0)
				{
					UE_LOG(LogBlackmagicMedia, Warning, TEXT("Lost %d video frames on input %s. Frame rate is either too slow or buffering capacity is too small."), VideoOverflowCount, *InUrl);
				}
			}
		}

	private:
		virtual void AddRef() override
		{
			++RefCounter;
		}

		virtual void Release() override
		{
			--RefCounter;
			if (RefCounter == 0)
			{
				delete this;
			}
		}

		virtual void OnInitializationCompleted(bool bSuccess) override
		{
			MediaState = bSuccess ? EMediaState::Playing : EMediaState::Error;
		}

		virtual void OnShutdownCompleted() override
		{
			MediaState = EMediaState::Closed;
		}

		virtual void OnFrameReceived(const BlackmagicDesign::IInputEventCallback::FFrameReceivedInfo& InFrameInfo) override
		{
			SCOPE_CYCLE_COUNTER(STAT_Blackmagic_MediaPlayer_ProcessReceivedFrame);

			FScopeLock Lock(&CallbackLock);

			if (MediaPlayer == nullptr)
			{
				return;
			}

			if (!InFrameInfo.bHasInputSource && InFrameInfo.AudioBuffer == nullptr)
			{
				const double CurrentTime = FApp::GetCurrentTime();
				const double TimeAllowedToConnect = 2.0;
				if (LastHasFrameTime < 0.1)
				{
					LastHasFrameTime = CurrentTime;
				}
				if (bReceivedValidFrame || CurrentTime - LastHasFrameTime > TimeAllowedToConnect)
				{
					UE_LOG(LogBlackmagicMedia, Error, TEXT("There is no video input for '%s'."), *MediaPlayer->GetUrl());
					MediaState = EMediaState::Error;
				}
				return;
			}
			bReceivedValidFrame = bReceivedValidFrame || InFrameInfo.bHasInputSource;

			FTimespan DecodedTime = FTimespan::FromSeconds(MediaPlayer->GetPlatformSeconds());
			FTimespan DecodedTimeF2 = DecodedTime + FTimespan::FromSeconds(MediaPlayer->VideoFrameRate.AsInterval());

			if (MediaState == EMediaState::Playing)
			{
				TOptional<FTimecode> DecodedTimecode;
				TOptional<FTimecode> DecodedTimecodeF2;

				if (InFrameInfo.bHaveTimecode)
				{
					//We expect the timecode to be processed in the library. What we receive will be a "linear" timecode even for frame rates greater than 30.
					const int32 FrameLimit = InFrameInfo.FieldDominance != BlackmagicDesign::EFieldDominance::Interlaced ? FMath::RoundToInt(MediaPlayer->VideoFrameRate.AsDecimal()) : FMath::RoundToInt(MediaPlayer->VideoFrameRate.AsDecimal()) - 1;
					if ((int32)InFrameInfo.Timecode.Frames >= FrameLimit)
					{
						UE_LOG(LogBlackmagicMedia, Warning, TEXT("Input '%s' received an invalid Timecode frame number (%d) for the current frame rate (%s)."), *MediaPlayer->GetUrl(), InFrameInfo.Timecode.Frames, *MediaPlayer->VideoFrameRate.ToPrettyText().ToString());
					}

					DecodedTimecode = FTimecode(InFrameInfo.Timecode.Hours, InFrameInfo.Timecode.Minutes, InFrameInfo.Timecode.Seconds, InFrameInfo.Timecode.Frames, FTimecode::IsDropFormatTimecodeSupported(MediaPlayer->VideoFrameRate));
					DecodedTimecodeF2 = DecodedTimecode;
					++DecodedTimecodeF2->Frames;

					const FTimespan TimecodeDecodedTime = DecodedTimecode->ToTimespan(MediaPlayer->VideoFrameRate);
					if (MediaPlayer->bUseTimeSynchronization)
					{
						DecodedTime = TimecodeDecodedTime;
						DecodedTimeF2 = TimecodeDecodedTime + FTimespan::FromSeconds(MediaPlayer->VideoFrameRate.AsInterval());
					}

					PreviousTimecode = InFrameInfo.Timecode;
					PrevousTimespan = TimecodeDecodedTime;

					if (MediaPlayer->IsTimecodeLogEnabled())
					{
						UE_LOG(LogBlackmagicMedia, Log, TEXT("Input '%s' has timecode : %02d:%02d:%02d:%02d"), *MediaPlayer->GetUrl()
							, InFrameInfo.Timecode.Hours, InFrameInfo.Timecode.Minutes, InFrameInfo.Timecode.Seconds, InFrameInfo.Timecode.Frames);
					}
				}
				else if (!bHasWarnedMissingTimecode && bIsTimecodeExpected)
				{
					bHasWarnedMissingTimecode = true;
					UE_LOG(LogBlackmagicMedia, Warning, TEXT("Input '%s' is expecting timecode but didn't receive any in the last frame. Is your source configured correctly?"), *MediaPlayer->GetUrl());
				}

				if (InFrameInfo.AudioBuffer)
				{
					if (MediaPlayer->Samples->NumAudioSamples() >= MaxNumAudioFrameBuffer * BlackmagicMediaPlayerHelpers::ToleratedExtraMaxBufferCount)
					{
						if (MediaPlayer->bVerifyFrameDropCount)
						{
							FPlatformAtomics::InterlockedIncrement(&AudioFrameDropCount);
						}
					}
					else
					{
						auto AudioSample = MediaPlayer->AudioSamplePool->AcquireShared();
						if (AudioSample->Initialize(reinterpret_cast<int32*>(InFrameInfo.AudioBuffer)
							, InFrameInfo.AudioBufferSize / sizeof(int32)
							, InFrameInfo.NumberOfAudioChannel
							, InFrameInfo.AudioRate
							, DecodedTime
							, DecodedTimecode))
						{
							MediaPlayer->Samples->AddAudio(AudioSample);

							LastBitsPerSample = sizeof(int32);
							LastSampleRate = InFrameInfo.AudioRate;
							LastNumChannels = InFrameInfo.NumberOfAudioChannel;
						}
					}
				}

				if (InFrameInfo.VideoBuffer)
				{
					const bool bIsProgressivePicture = InFrameInfo.FieldDominance != BlackmagicDesign::EFieldDominance::Interlaced;
					const int32 NumVideoSamples = MediaPlayer->Samples->NumVideoSamples() + (!bIsProgressivePicture ? 1 : 0);
					if (NumVideoSamples >= MaxNumVideoFrameBuffer * BlackmagicMediaPlayerHelpers::ToleratedExtraMaxBufferCount)
					{
						if (MediaPlayer->bVerifyFrameDropCount)
						{
							FPlatformAtomics::InterlockedIncrement(&VideoFrameDropCount);
						}
					}
					else
					{
						EMediaTextureSampleFormat SampleFormat = EMediaTextureSampleFormat::CharBGRA;
						EMediaIOCoreEncodePixelFormat EncodePixelFormat = EMediaIOCoreEncodePixelFormat::CharUYVY;
						FString OutputFilename = "";

						switch (InFrameInfo.PixelFormat)
						{
						case BlackmagicDesign::EPixelFormat::pf_8Bits:
							SampleFormat = EMediaTextureSampleFormat::CharUYVY;
							EncodePixelFormat = EMediaIOCoreEncodePixelFormat::CharUYVY;
							OutputFilename = FString::Printf(TEXT("Blackmagic_Output_8_YUV_ch%d"), ChannelInfo.DeviceIndex);
							break;
						case BlackmagicDesign::EPixelFormat::pf_10Bits:
							SampleFormat = EMediaTextureSampleFormat::YUVv210;
							EncodePixelFormat = EMediaIOCoreEncodePixelFormat::YUVv210;
							OutputFilename = FString::Printf(TEXT("Blackmagic_Output_10_YUV_ch%d"), ChannelInfo.DeviceIndex);
							break;
						}

						if (bBlackmagicWriteOutputRawDataCmdEnable)
						{
							MediaIOCoreFileWriter::WriteRawFile(OutputFilename, reinterpret_cast<uint8*>(InFrameInfo.VideoBuffer), InFrameInfo.VideoPitch * InFrameInfo.VideoHeight);
							bBlackmagicWriteOutputRawDataCmdEnable = false;
						}

						if (bIsProgressivePicture)
						{
							if (bEncodeTimecodeInTexel && DecodedTimecode.IsSet())
							{
								FTimecode SetTimecode = DecodedTimecode.GetValue();
								FMediaIOCoreEncodeTime EncodeTime(EncodePixelFormat, InFrameInfo.VideoBuffer, InFrameInfo.VideoPitch, InFrameInfo.VideoWidth, InFrameInfo.VideoHeight);
								EncodeTime.Render(SetTimecode.Hours, SetTimecode.Minutes, SetTimecode.Seconds, SetTimecode.Frames);
							}

							auto TextureSample = MediaPlayer->TextureSamplePool->AcquireShared();
							if (TextureSample->Initialize(InFrameInfo.VideoBuffer
								, InFrameInfo.VideoPitch * InFrameInfo.VideoHeight
								, InFrameInfo.VideoPitch
								, InFrameInfo.VideoWidth
								, InFrameInfo.VideoHeight
								, SampleFormat
								, DecodedTime
								, MediaPlayer->VideoFrameRate
								, DecodedTimecode
								, bIsSRGBInput))
							{
								MediaPlayer->Samples->AddVideo(TextureSample);
							}
						}
						else
						{
							auto TextureSampleEven = MediaPlayer->TextureSamplePool->AcquireShared();
							if (TextureSampleEven->InitializeWithEvenOddLine(true
								, InFrameInfo.VideoBuffer
								, InFrameInfo.VideoPitch * InFrameInfo.VideoHeight
								, InFrameInfo.VideoPitch
								, InFrameInfo.VideoWidth
								, InFrameInfo.VideoHeight
								, SampleFormat
								, DecodedTime
								, MediaPlayer->VideoFrameRate
								, DecodedTimecode
								, bIsSRGBInput))
							{
								MediaPlayer->Samples->AddVideo(TextureSampleEven);
							}

							auto TextureSampleOdd = MediaPlayer->TextureSamplePool->AcquireShared();
							if (TextureSampleOdd->InitializeWithEvenOddLine(false
								, InFrameInfo.VideoBuffer
								, InFrameInfo.VideoPitch * InFrameInfo.VideoHeight
								, InFrameInfo.VideoPitch
								, InFrameInfo.VideoWidth
								, InFrameInfo.VideoHeight
								, SampleFormat
								, DecodedTimeF2
								, MediaPlayer->VideoFrameRate
								, DecodedTimecodeF2
								, bIsSRGBInput))
							{
								MediaPlayer->Samples->AddVideo(TextureSampleOdd);
							}
						}
					}
				}
			}
		}

		virtual void OnFrameFormatChanged(const BlackmagicDesign::FFormatInfo& NewFormat) override
		{
			UE_LOG(LogBlackmagicMedia, Error, TEXT("The video format changed for '%s'."), MediaPlayer ? *MediaPlayer->GetUrl() : TEXT("<Invalid>"));
			MediaState = EMediaState::Error;
		}

		virtual void OnInterlacedOddFieldEvent() override
		{
			
		}

	private:
		TAtomic<int32> RefCounter;

		BlackmagicDesign::FUniqueIdentifier BlackmagicIdendifier;
		BlackmagicDesign::FChannelInfo ChannelInfo;

		mutable FCriticalSection CallbackLock;
		FBlackmagicMediaPlayer* MediaPlayer;

		EMediaState MediaState;

		BlackmagicDesign::FTimecode PreviousTimecode;
		FTimespan PrevousTimespan;
		bool bEncodeTimecodeInTexel;

		/** Number of audio bits per sample, audio channels and sample rate. */
		uint32 LastBitsPerSample;
		uint32 LastNumChannels;
		uint32 LastSampleRate;

		int32 AudioFrameDropCount;
		int32 MetadataFrameDropCount;
		int32 VideoFrameDropCount;

		int32 MaxNumAudioFrameBuffer;
		int32 MaxNumVideoFrameBuffer;

		/** Has video frame detection */
		double LastHasFrameTime;
		bool bReceivedValidFrame;

		bool bIsTimecodeExpected;
		bool bHasWarnedMissingTimecode;

		/** Whether this input is in sRGB space and needs a to linear conversion */
		bool bIsSRGBInput;
	};
}

/* FBlackmagicVideoPlayer structors
*****************************************************************************/

FBlackmagicMediaPlayer::FBlackmagicMediaPlayer(IMediaEventSink& InEventSink)
	: Super(InEventSink)
	, EventCallback(nullptr)
	, AudioSamplePool(new FBlackmagicMediaAudioSamplePool)
	, TextureSamplePool(new FBlackmagicMediaTextureSamplePool)
	, bVerifyFrameDropCount(false)
{
}

FBlackmagicMediaPlayer::~FBlackmagicMediaPlayer()
{
	Close();
	delete TextureSamplePool;
	delete AudioSamplePool;
}

/* IMediaPlayer interface
*****************************************************************************/

void FBlackmagicMediaPlayer::Close()
{
	if (EventCallback)
	{
		EventCallback->Uninitialize();
		EventCallback = nullptr;
	}

	AudioSamplePool->Reset();
	TextureSamplePool->Reset();

	Super::Close();
}

FName FBlackmagicMediaPlayer::GetPlayerName() const
{
	static FName PlayerName(TEXT("BlackmagicMedia"));
	return PlayerName;
}

bool FBlackmagicMediaPlayer::Open(const FString& Url, const IMediaOptions* Options)
{
	if (!FBlackmagic::IsInitialized())
	{
		UE_LOG(LogBlackmagicMedia, Error, TEXT("The BlackmagicMediaPlayer can't open URL '%s'. Blackmagic is not initialized on your machine."), *Url);
		return false;
	}

	if (!FBlackmagic::CanUseBlackmagicCard())
	{
		UE_LOG(LogBlackmagicMedia, Warning, TEXT("The BlackmagicMediaPlayer can't open URL '%s' because Blackmagic card cannot be used. Are you in a Commandlet? You may override this behavior by launching with -ForceBlackmagicUsage"), *Url);
		return false;
	}

	if (!Super::Open(Url, Options))
	{
		return false;
	}

	BlackmagicDesign::FChannelInfo ChannelInfo;
	ChannelInfo.DeviceIndex = Options->GetMediaOption(BlackmagicMediaOption::DeviceIndex, (int64)0);

	check(EventCallback == nullptr);
	EventCallback = new BlackmagicMediaPlayerHelpers::FBlackmagicMediaPlayerEventCallback(this, ChannelInfo);

	BlackmagicDesign::FInputChannelOptions ChannelOptions;
	ChannelOptions.CallbackPriority = 10;
	ChannelOptions.bReadVideo = Options->GetMediaOption(BlackmagicMediaOption::CaptureVideo, true);
	ChannelOptions.FormatInfo.DisplayMode = Options->GetMediaOption(BlackmagicMediaOption::BlackmagicVideoFormat, (int64)BlackmagicMediaOption::DefaultVideoFormat);
	EBlackmagicMediaSourceColorFormat ColorFormat = (EBlackmagicMediaSourceColorFormat)(Options->GetMediaOption(BlackmagicMediaOption::ColorFormat, (int64)EBlackmagicMediaSourceColorFormat::YUV8));
	ChannelOptions.PixelFormat = ColorFormat == EBlackmagicMediaSourceColorFormat::YUV8 ? BlackmagicDesign::EPixelFormat::pf_8Bits : BlackmagicDesign::EPixelFormat::pf_10Bits;
	const bool bIsSRGBInput = Options->GetMediaOption(BlackmagicMediaOption::SRGBInput, true);

	const EMediaIOTimecodeFormat TimecodeFormat = (EMediaIOTimecodeFormat)(Options->GetMediaOption(BlackmagicMediaOption::TimecodeFormat, (int64)EMediaIOTimecodeFormat::None));
	switch (TimecodeFormat)
	{
	case EMediaIOTimecodeFormat::LTC:
		ChannelOptions.TimecodeFormat = BlackmagicDesign::ETimecodeFormat::TCF_LTC;
		break;
	case EMediaIOTimecodeFormat::VITC:
		ChannelOptions.TimecodeFormat = BlackmagicDesign::ETimecodeFormat::TCF_VITC1;
		break;
	case EMediaIOTimecodeFormat::None:
	default:
		ChannelOptions.TimecodeFormat = BlackmagicDesign::ETimecodeFormat::TCF_None;
		break;
	}

	//Audio options
	{
		ChannelOptions.bReadAudio = Options->GetMediaOption(BlackmagicMediaOption::CaptureAudio, false);
		const EBlackmagicMediaAudioChannel AudioChannelOption = (EBlackmagicMediaAudioChannel)(Options->GetMediaOption(BlackmagicMediaOption::AudioChannelOption, (int64)EBlackmagicMediaAudioChannel::Stereo2));
		ChannelOptions.NumberOfAudioChannel = (AudioChannelOption == EBlackmagicMediaAudioChannel::Surround8) ? 8 : 2;
	}

	bVerifyFrameDropCount = Options->GetMediaOption(BlackmagicMediaOption::LogDropFrame, false);
	const bool bEncodeTimecodeInTexel = TimecodeFormat != EMediaIOTimecodeFormat::None && Options->GetMediaOption(BlackmagicMediaOption::EncodeTimecodeInTexel, false);
	int32 MaxNumAudioFrameBuffer = Options->GetMediaOption(BlackmagicMediaOption::MaxAudioFrameBuffer, (int64)8);
	int32 MaxNumVideoFrameBuffer = Options->GetMediaOption(BlackmagicMediaOption::MaxVideoFrameBuffer, (int64)8);

	bool bSuccess = EventCallback->Initialize(ChannelOptions, bEncodeTimecodeInTexel, MaxNumAudioFrameBuffer, MaxNumVideoFrameBuffer, bIsSRGBInput);

	if (!bSuccess)
	{
		EventCallback->Uninitialize();
		EventCallback = nullptr;
	}
	return bSuccess;
}


void FBlackmagicMediaPlayer::TickInput(FTimespan DeltaTime, FTimespan Timecode)
{
	// update player state
	EMediaState NewState = EventCallback ? EventCallback->GetMediaState() : EMediaState::Closed;

	if (NewState != CurrentState)
	{
		CurrentState = NewState;
		if (CurrentState == EMediaState::Playing)
		{
			EventSink.ReceiveMediaEvent(EMediaEvent::TracksChanged);
			EventSink.ReceiveMediaEvent(EMediaEvent::MediaOpened);
			EventSink.ReceiveMediaEvent(EMediaEvent::PlaybackResumed);
		}
		else if (NewState == EMediaState::Error)
		{
			EventSink.ReceiveMediaEvent(EMediaEvent::MediaOpenFailed);
			Close();
		}
	}

	if (CurrentState != EMediaState::Playing)
	{
		return;
	}

	TickTimeManagement();
}

void FBlackmagicMediaPlayer::TickFetch(FTimespan DeltaTime, FTimespan /*Timecode*/)
{
	if (IsHardwareReady())
	{
		ProcessFrame();
		VerifyFrameDropCount();
	}
}

void FBlackmagicMediaPlayer::ProcessFrame()
{
	EventCallback->UpdateAudioTrackFormat(AudioTrackFormat);
}

void FBlackmagicMediaPlayer::VerifyFrameDropCount()
{
	EventCallback->VerifyFrameDropCount_GameThread(OpenUrl);
}

bool FBlackmagicMediaPlayer::IsHardwareReady() const
{
	return EventCallback && EventCallback->GetMediaState() == EMediaState::Playing;
}

#undef LOCTEXT_NAMESPACE

