#pragma once

#ifdef __cplusplus

#include <cstddef>
#include <cstdint>
#include <string>
#include <functional>

// Physical capture/mixer clock shared by the board driver and native hosts.
// AudioContext and network PCM may use other rates through their resamplers.
#ifndef GEA_AUDIO_DEVICE_SAMPLE_RATE
#define GEA_AUDIO_DEVICE_SAMPLE_RATE 16000
#endif

namespace gea::platform::audio {

inline constexpr int deviceSampleRate = GEA_AUDIO_DEVICE_SAMPLE_RATE;
static_assert(deviceSampleRate >= 8000 && deviceSampleRate <= 48000,
              "Audio device sample rate must be between 8000 and 48000 Hz");

enum class OscillatorType {
	Sine = 0,
	Square = 1,
	Sawtooth = 2,
	Triangle = 3,
};

using NativeAudioHandle = std::uintptr_t;

class AudioParam {
public:
	explicit AudioParam(NativeAudioHandle oscillator = 0);
	double value() const;
	void setValue(double value);
	void setValueAtTime(double value, double start_time);

private:
	NativeAudioHandle oscillator_;
};

class AudioNode {
public:
	NativeAudioHandle nativeId() const;

protected:
	explicit AudioNode(NativeAudioHandle native);

private:
	NativeAudioHandle native_;
};

class AudioDestinationNode : public AudioNode {
public:
	explicit AudioDestinationNode(NativeAudioHandle native = 0);
};

class OscillatorNode : public AudioNode {
public:
	explicit OscillatorNode(NativeAudioHandle native = 0);

	OscillatorType type() const;
	void setType(OscillatorType type);
	void connect(const AudioDestinationNode &destination);
	void start(double when = 0.0);
	void stop(double when = 0.0);

	AudioParam frequency;
};

class AudioContext {
public:
	double currentTime() const;
	AudioDestinationNode destination() const;
	OscillatorNode createOscillator() const;
};

class AudioSystem {
public:
	static AudioContext sharedContext();
	static int volume();
	static void setVolume(int volume_percent);
	static bool playFile(const std::string &path);
	static bool playPcm(const std::int16_t *samples, std::size_t sample_count, int sample_rate, int channels);
	static void stopPlayback();
	static void flushPlayback();
	// Continuous mono source at deviceSampleRate; zero means the platform could not attach it.
	static std::uint64_t playPcmStream(std::function<std::size_t(std::int16_t*, std::size_t)> pull);
	static void stopPcmStream(std::uint64_t stream);
	static bool pcmStreamSettled(std::uint64_t stream);
	// Maximum mixer and device buffering delays, in seconds.
	static double processingLatency();
	static double outputLatency();
};

#ifdef GEA_AUDIO_DRIVER_INTERNAL
class OutputDriver {
public:
	static bool open(int sample_rate, int channels, int bits_per_sample);
	static bool write(const std::int16_t *pcm, std::size_t sample_count, int timeout_ms);
	static void close();
	static bool flush();
	static int volume();
	static void setVolume(int volume_percent);
	static double outputLatency(int sample_rate);
};
#endif

}  // namespace gea::platform::audio

#endif
