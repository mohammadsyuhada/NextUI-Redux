#include "player.h"
#include "radio.h"
#include "album_art.h"
#include "settings.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <math.h>
#include <fcntl.h>
#include <dirent.h>
#include <samplerate.h>
#include <SDL2/SDL_image.h>
#include "api.h"
#include "msettings.h"

// Include dr_libs for audio decoding (header-only libraries)
#define DR_MP3_IMPLEMENTATION
#include "audio/dr_mp3.h"

#define DR_FLAC_IMPLEMENTATION
#include "audio/dr_flac.h"

#define DR_WAV_IMPLEMENTATION
#include "audio/dr_wav.h"

// For OGG we use stb_vorbis (implementation is in the .c file renamed to .h)
#include "audio/stb_vorbis.h"

// For M4A/AAC we use minimp4 for demuxing and FDK-AAC for decoding
#define MINIMP4_IMPLEMENTATION
#include "audio/minimp4.h"
#include <src/aacdecoder_lib.h>
#include <opusfile.h>

// Linux input event definitions (avoid including linux/input.h due to conflicts)
#define EV_KEY 0x01
#define KEY_VOLUMEDOWN 114
#define KEY_VOLUMEUP 115
#define KEY_NEXTSONG 163
#define KEY_PLAYPAUSE 164
#define KEY_PREVIOUSSONG 165
#define KEY_PLAYCD 200
#define KEY_PAUSECD 201

// Input event struct for 64-bit systems (24 bytes)
struct input_event_raw {
	uint64_t tv_sec;
	uint64_t tv_usec;
	uint16_t type;
	uint16_t code;
	int32_t value;
};

// M4A decoder state (uses minimp4 + FDK-AAC)
typedef struct {
	MP4D_demux_t mp4;
	FILE* file;
	HANDLE_AACDECODER aac_decoder;
	int audio_track;		 // Index of audio track in MP4
	unsigned current_sample; // Current sample/frame index
	unsigned sample_count;	 // Total samples
	int sample_rate;
	int channels;
	// Buffer for reading AAC frames
	uint8_t* frame_buffer;
	size_t frame_buffer_size;
	// Leftover buffer for decoded samples that didn't fit in output
	int16_t* leftover_buffer;
	size_t leftover_count;	  // Number of stereo frames in leftover buffer
	size_t leftover_capacity; // Capacity in stereo frames
} M4ADecoder;

// Standalone AAC/ADTS file decoder state (uses FDK-AAC with TT_MP4_ADTS)
#define AAC_FILE_READ_BUF_SIZE 32768 // 32KB read buffer for efficient file I/O
typedef struct {
	FILE* file;
	HANDLE_AACDECODER aac_decoder;
	int sample_rate;
	int channels;
	int frame_size; // PCM frames per AAC frame (1024 or 2048 for HE-AAC)
	int64_t file_size;
	// File read buffer
	uint8_t* read_buf;
	int read_buf_size;
	// Leftover PCM buffer
	int16_t* leftover_buffer;
	size_t leftover_count;
	size_t leftover_capacity;
} AACFileDecoder;

// minimp4 read callback - returns 0 on success, non-zero on failure
static int m4a_read_callback(int64_t offset, void* buffer, size_t size, void* token) {
	FILE* f = (FILE*)token;
	if (fseek(f, (long)offset, SEEK_SET) != 0) {
		return -1; // Seek failed
	}
	size_t bytes_read = fread(buffer, 1, size, f);
	if (bytes_read != size) {
		return -1; // Read failed or incomplete
	}
	return 0; // Success
}

// Sample rates for different audio outputs
#define SAMPLE_RATE_BLUETOOTH 44100 // 44.1kHz for Bluetooth A2DP compatibility
#define SAMPLE_RATE_SPEAKER 48000	// 48kHz for speaker output
#define SAMPLE_RATE_USB_DAC 48000	// 48kHz for USB DAC output
#define SAMPLE_RATE_DEFAULT 48000	// Default fallback

#define AUDIO_CHANNELS 2
#define AUDIO_SAMPLES 2048 // Smaller buffer for lower latency

// Convert linear volume (0-1) to perceived volume using logarithmic curve
// This makes volume steps feel more natural to human hearing
static inline float apply_volume_curve(float linear_vol) {
	if (linear_vol <= 0.0f)
		return 0.0f;
	if (linear_vol >= 1.0f)
		return 1.0f;
	// Use power curve with exponent 0.4 for natural perceived loudness
	// At 50% input -> ~76% output, at 25% input -> ~57% output
	return powf(linear_vol, 0.4f);
}

// Soft limiter for built-in speaker to prevent amplifier clipping
// Linear below threshold, asymptotically compressed above
static inline int16_t speaker_soft_limit(int16_t sample, float threshold) {
	float headroom = 1.0f - threshold;

	float x = sample * (1.0f / 32768.0f);
	float abs_x = fabsf(x);
	if (abs_x <= threshold)
		return sample;

	float sign = (x >= 0.0f) ? 1.0f : -1.0f;
	float over = abs_x - threshold;
	// Asymptotic curve: smoothly approaches 1.0 but never reaches it
	float compressed = threshold + headroom * over / (over + headroom);

	return (int16_t)(sign * compressed * 32767.0f);
}

// High-pass biquad filter for built-in speaker to remove sub-bass
// that the tiny speaker can't reproduce (just wastes amp headroom)
typedef struct {
	float w1, w2; // Direct Form II Transposed state
} BiquadState;

static struct {
	float b0, b1, b2, a1, a2;
} speaker_hpf_coeffs;
static BiquadState speaker_hpf_state[AUDIO_CHANNELS];
static int speaker_hpf_last_hz = 0; // tracks setting changes for coefficient recalc

static void speaker_hpf_init(int sample_rate, float cutoff_hz) {
	// 2nd-order Butterworth high-pass
	const float fc = cutoff_hz;
	const float Q = 0.7071f; // Butterworth
	float omega = 2.0f * M_PI * fc / (float)sample_rate;
	float sin_w = sinf(omega);
	float cos_w = cosf(omega);
	float alpha = sin_w / (2.0f * Q);

	float a0 = 1.0f + alpha;
	speaker_hpf_coeffs.b0 = ((1.0f + cos_w) / 2.0f) / a0;
	speaker_hpf_coeffs.b1 = (-(1.0f + cos_w)) / a0;
	speaker_hpf_coeffs.b2 = ((1.0f + cos_w) / 2.0f) / a0;
	speaker_hpf_coeffs.a1 = (-2.0f * cos_w) / a0;
	speaker_hpf_coeffs.a2 = (1.0f - alpha) / a0;

	for (int i = 0; i < AUDIO_CHANNELS; i++) {
		speaker_hpf_state[i].w1 = 0.0f;
		speaker_hpf_state[i].w2 = 0.0f;
	}
}

static inline int16_t speaker_hpf_process(int16_t sample, int channel) {
	BiquadState* s = &speaker_hpf_state[channel];
	float x = (float)sample;

	// Direct Form II Transposed
	float y = speaker_hpf_coeffs.b0 * x + s->w1;
	s->w1 = speaker_hpf_coeffs.b1 * x - speaker_hpf_coeffs.a1 * y + s->w2;
	s->w2 = speaker_hpf_coeffs.b2 * x - speaker_hpf_coeffs.a2 * y;

	if (y > 32767.0f)
		y = 32767.0f;
	if (y < -32768.0f)
		y = -32768.0f;

	return (int16_t)y;
}

// Global player context
static PlayerContext player = {0};
static int64_t audio_position_samples = 0;			  // Track position in samples for precision
static WaveformData waveform = {0};					  // Waveform overview for progress display
static int current_sample_rate = SAMPLE_RATE_DEFAULT; // Track current SDL audio device rate
static bool bluetooth_audio_active = false;			  // Track if Bluetooth audio is active
static bool usbdac_audio_active = false;			  // Track if USB DAC is active

// Get target sample rate based on current audio sink
static int get_target_sample_rate(void) {
	if (bluetooth_audio_active) {
		return SAMPLE_RATE_BLUETOOTH; // 44100 Hz for Bluetooth
	}
	// Check audio sink from msettings
	int sink = GetAudioSink();
	switch (sink) {
	case AUDIO_SINK_BLUETOOTH:
		return SAMPLE_RATE_BLUETOOTH; // 44100 Hz
	case AUDIO_SINK_USBDAC:
		return SAMPLE_RATE_USB_DAC; // 48000 Hz
	default:
		return SAMPLE_RATE_SPEAKER; // 48000 Hz for speaker
	}
}

// Forward declaration for audio device change callback
static void audio_device_change_callback(int device_type, int event);

// Forward declaration for FLAC metadata callback
static void flac_metadata_callback(void* pUserData, drflac_metadata* pMetadata);

// ============ STREAMING PLAYBACK SYSTEM ============

// Decode chunk size (~0.5 seconds at 48kHz)
#define DECODE_CHUNK_FRAMES 24000

// Circular buffer functions
static int circular_buffer_init(CircularBuffer* cb, size_t capacity_frames) {
	cb->buffer = malloc(capacity_frames * sizeof(int16_t) * AUDIO_CHANNELS);
	if (!cb->buffer) {
		LOG_error("Failed to allocate circular buffer (%zu KB)\n",
				  capacity_frames * sizeof(int16_t) * AUDIO_CHANNELS / 1024);
		return -1;
	}
	cb->capacity = capacity_frames;
	cb->write_pos = 0;
	cb->read_pos = 0;
	cb->available = 0;
	pthread_mutex_init(&cb->mutex, NULL);
	return 0;
}

static void circular_buffer_free(CircularBuffer* cb) {
	if (cb->buffer) {
		free(cb->buffer);
		cb->buffer = NULL;
	}
	pthread_mutex_destroy(&cb->mutex);
	cb->capacity = 0;
	cb->write_pos = 0;
	cb->read_pos = 0;
	cb->available = 0;
}

static void circular_buffer_clear(CircularBuffer* cb) {
	pthread_mutex_lock(&cb->mutex);
	cb->write_pos = 0;
	cb->read_pos = 0;
	cb->available = 0;
	pthread_mutex_unlock(&cb->mutex);
}

static size_t circular_buffer_available(CircularBuffer* cb) {
	pthread_mutex_lock(&cb->mutex);
	size_t avail = cb->available;
	pthread_mutex_unlock(&cb->mutex);
	return avail;
}

// Write frames to circular buffer (called by decode thread)
static size_t circular_buffer_write(CircularBuffer* cb, int16_t* data, size_t frames) {
	pthread_mutex_lock(&cb->mutex);

	size_t space = cb->capacity - cb->available;
	size_t to_write = (frames < space) ? frames : space;

	if (to_write == 0) {
		pthread_mutex_unlock(&cb->mutex);
		return 0;
	}

	// Write in two parts if wrapping
	size_t first_part = cb->capacity - cb->write_pos;
	if (first_part > to_write)
		first_part = to_write;

	memcpy(&cb->buffer[cb->write_pos * AUDIO_CHANNELS], data,
		   first_part * sizeof(int16_t) * AUDIO_CHANNELS);

	size_t second_part = to_write - first_part;
	if (second_part > 0) {
		memcpy(cb->buffer, &data[first_part * AUDIO_CHANNELS],
			   second_part * sizeof(int16_t) * AUDIO_CHANNELS);
	}

	cb->write_pos = (cb->write_pos + to_write) % cb->capacity;
	cb->available += to_write;

	pthread_mutex_unlock(&cb->mutex);
	return to_write;
}

// Read frames from circular buffer (called by audio callback)
static size_t circular_buffer_read(CircularBuffer* cb, int16_t* data, size_t frames) {
	pthread_mutex_lock(&cb->mutex);

	size_t to_read = (frames < cb->available) ? frames : cb->available;

	if (to_read == 0) {
		pthread_mutex_unlock(&cb->mutex);
		return 0;
	}

	// Read in two parts if wrapping
	size_t first_part = cb->capacity - cb->read_pos;
	if (first_part > to_read)
		first_part = to_read;

	memcpy(data, &cb->buffer[cb->read_pos * AUDIO_CHANNELS],
		   first_part * sizeof(int16_t) * AUDIO_CHANNELS);

	size_t second_part = to_read - first_part;
	if (second_part > 0) {
		memcpy(&data[first_part * AUDIO_CHANNELS], cb->buffer,
			   second_part * sizeof(int16_t) * AUDIO_CHANNELS);
	}

	cb->read_pos = (cb->read_pos + to_read) % cb->capacity;
	cb->available -= to_read;

	pthread_mutex_unlock(&cb->mutex);
	return to_read;
}

// ============ STREAMING DECODER INTERFACE ============

// Open decoder and read metadata (doesn't decode audio yet)
static int stream_decoder_open(StreamDecoder* sd, const char* filepath) {
	memset(sd, 0, sizeof(StreamDecoder));

	sd->format = Player_detectFormat(filepath);
	if (sd->format == AUDIO_FORMAT_UNKNOWN) {
		LOG_error("Stream: Unknown audio format: %s\n", filepath);
		return -1;
	}

	switch (sd->format) {
	case AUDIO_FORMAT_MP3: {
		drmp3* mp3 = malloc(sizeof(drmp3));
		if (!mp3 || !drmp3_init_file(mp3, filepath, NULL)) {
			free(mp3);
			LOG_error("Stream: Failed to open MP3: %s\n", filepath);
			return -1;
		}
		sd->decoder = mp3;
		sd->source_sample_rate = mp3->sampleRate;
		sd->source_channels = mp3->channels;
		sd->total_frames = drmp3_get_pcm_frame_count(mp3);
		break;
	}
	case AUDIO_FORMAT_WAV: {
		drwav* wav = malloc(sizeof(drwav));
		if (!wav || !drwav_init_file(wav, filepath, NULL)) {
			free(wav);
			LOG_error("Stream: Failed to open WAV: %s\n", filepath);
			return -1;
		}
		sd->decoder = wav;
		sd->source_sample_rate = wav->sampleRate;
		sd->source_channels = wav->channels;
		sd->total_frames = wav->totalPCMFrameCount;
		break;
	}
	case AUDIO_FORMAT_FLAC: {
		drflac* flac = drflac_open_file_with_metadata(filepath, flac_metadata_callback, NULL, NULL);
		if (!flac) {
			LOG_error("Stream: Failed to open FLAC: %s\n", filepath);
			return -1;
		}
		sd->decoder = flac;
		sd->source_sample_rate = flac->sampleRate;
		sd->source_channels = flac->channels;
		sd->total_frames = flac->totalPCMFrameCount;
		break;
	}
	case AUDIO_FORMAT_OGG: {
		int error;
		stb_vorbis* vorbis = stb_vorbis_open_filename(filepath, &error, NULL);
		if (!vorbis) {
			LOG_error("Stream: Failed to open OGG: %s (error %d)\n", filepath, error);
			return -1;
		}
		sd->decoder = vorbis;
		stb_vorbis_info info = stb_vorbis_get_info(vorbis);
		sd->source_sample_rate = info.sample_rate;
		sd->source_channels = info.channels;
		sd->total_frames = stb_vorbis_stream_length_in_samples(vorbis);
		break;
	}
	case AUDIO_FORMAT_OPUS: {
		int error;
		OggOpusFile* of = op_open_file(filepath, &error);
		if (!of) {
			LOG_error("Stream: Failed to open Opus: %s (error %d)\n", filepath, error);
			return -1;
		}
		sd->decoder = of;
		sd->source_sample_rate = 48000; // Opus always decodes at 48kHz
		sd->source_channels = 2;		// op_read_stereo() always outputs stereo
		sd->total_frames = op_pcm_total(of, -1);
		break;
	}
	case AUDIO_FORMAT_M4A: {
		M4ADecoder* m4a = malloc(sizeof(M4ADecoder));
		if (!m4a) {
			LOG_error("Stream: Failed to allocate M4A decoder\n");
			return -1;
		}
		memset(m4a, 0, sizeof(M4ADecoder));

		// Open the file
		m4a->file = fopen(filepath, "rb");
		if (!m4a->file) {
			free(m4a);
			LOG_error("Stream: Failed to open M4A file: %s\n", filepath);
			return -1;
		}

		// Get file size
		fseek(m4a->file, 0, SEEK_END);
		int64_t file_size = ftell(m4a->file);
		fseek(m4a->file, 0, SEEK_SET);

		// Open MP4 demuxer
		int track_count = MP4D_open(&m4a->mp4, m4a_read_callback, m4a->file, file_size);
		if (track_count == 0) {
			fclose(m4a->file);
			free(m4a);
			LOG_error("Stream: Failed to parse M4A container: %s\n", filepath);
			return -1;
		}

		// Find audio track
		m4a->audio_track = -1;
		for (unsigned i = 0; i < m4a->mp4.track_count; i++) {
			if (m4a->mp4.track[i].handler_type == MP4D_HANDLER_TYPE_SOUN) {
				m4a->audio_track = i;
				break;
			}
		}

		if (m4a->audio_track < 0) {
			MP4D_close(&m4a->mp4);
			fclose(m4a->file);
			free(m4a);
			LOG_error("Stream: No audio track found in M4A: %s\n", filepath);
			return -1;
		}

		MP4D_track_t* track = &m4a->mp4.track[m4a->audio_track];
		m4a->sample_count = track->sample_count;
		m4a->sample_rate = track->SampleDescription.audio.samplerate_hz;
		m4a->channels = track->SampleDescription.audio.channelcount;
		m4a->current_sample = 0;

		// Initialize FDK-AAC decoder (TT_MP4_RAW for raw AAC frames from MP4 container)
		m4a->aac_decoder = aacDecoder_Open(TT_MP4_RAW, 1);
		if (!m4a->aac_decoder) {
			MP4D_close(&m4a->mp4);
			fclose(m4a->file);
			free(m4a);
			LOG_error("Stream: Failed to init AAC decoder for M4A: %s\n", filepath);
			return -1;
		}

		// Configure decoder with AudioSpecificConfig from MP4 container
		if (track->dsi && track->dsi_bytes > 0) {
			UCHAR* conf[] = {(UCHAR*)track->dsi};
			UINT conf_len[] = {(UINT)track->dsi_bytes};
			AAC_DECODER_ERROR conf_err = aacDecoder_ConfigRaw(m4a->aac_decoder, conf, conf_len);
			if (conf_err != AAC_DEC_OK) {
				LOG_error("Stream: Failed to configure AAC decoder (err=%d) for M4A: %s\n", conf_err, filepath);
				aacDecoder_Close(m4a->aac_decoder);
				MP4D_close(&m4a->mp4);
				fclose(m4a->file);
				free(m4a);
				return -1;
			}
		}

		// Allocate frame buffer (AAC frames are typically < 2KB)
		m4a->frame_buffer_size = 8192;
		m4a->frame_buffer = malloc(m4a->frame_buffer_size);
		if (!m4a->frame_buffer) {
			aacDecoder_Close(m4a->aac_decoder);
			MP4D_close(&m4a->mp4);
			fclose(m4a->file);
			free(m4a);
			LOG_error("Stream: Failed to allocate M4A frame buffer\n");
			return -1;
		}

		// Calculate total PCM frames from track duration
		// duration is in timescale units, need to convert to sample count
		uint64_t duration = ((uint64_t)track->duration_hi << 32) | track->duration_lo;
		if (track->timescale > 0 && m4a->sample_rate > 0) {
			sd->total_frames = (duration * m4a->sample_rate) / track->timescale;
		} else {
			// Fallback: estimate from sample count (1024 samples per AAC frame)
			sd->total_frames = (int64_t)m4a->sample_count * 1024;
		}

		sd->decoder = m4a;
		sd->source_sample_rate = m4a->sample_rate;
		sd->source_channels = m4a->channels;
		break;
	}
	case AUDIO_FORMAT_AAC: {
		AACFileDecoder* aac = malloc(sizeof(AACFileDecoder));
		if (!aac) {
			LOG_error("Stream: Failed to allocate AAC decoder\n");
			return -1;
		}
		memset(aac, 0, sizeof(AACFileDecoder));

		// Allocate read buffer
		aac->read_buf = malloc(AAC_FILE_READ_BUF_SIZE);
		if (!aac->read_buf) {
			free(aac);
			LOG_error("Stream: Failed to allocate AAC read buffer\n");
			return -1;
		}

		aac->file = fopen(filepath, "rb");
		if (!aac->file) {
			free(aac->read_buf);
			free(aac);
			LOG_error("Stream: Failed to open AAC file: %s\n", filepath);
			return -1;
		}

		// Get file size
		fseek(aac->file, 0, SEEK_END);
		aac->file_size = ftell(aac->file);
		fseek(aac->file, 0, SEEK_SET);

		// Open FDK-AAC decoder with ADTS transport (handles sync internally)
		aac->aac_decoder = aacDecoder_Open(TT_MP4_ADTS, 1);
		if (!aac->aac_decoder) {
			fclose(aac->file);
			free(aac->read_buf);
			free(aac);
			LOG_error("Stream: Failed to init AAC decoder for: %s\n", filepath);
			return -1;
		}

		// Read initial chunk and decode first frame to get stream info
		aac->read_buf_size = fread(aac->read_buf, 1, AAC_FILE_READ_BUF_SIZE, aac->file);
		if (aac->read_buf_size > 0) {
			UCHAR* inBuf[] = {aac->read_buf};
			UINT inLen[] = {(UINT)aac->read_buf_size};
			UINT bytesValid[] = {(UINT)aac->read_buf_size};
			aacDecoder_Fill(aac->aac_decoder, inBuf, inLen, bytesValid);

			INT_PCM tmp_buf[2048 * 2];
			AAC_DECODER_ERROR err = aacDecoder_DecodeFrame(aac->aac_decoder, tmp_buf, sizeof(tmp_buf) / sizeof(INT_PCM), 0);

			if (IS_OUTPUT_VALID(err)) {
				CStreamInfo* info = aacDecoder_GetStreamInfo(aac->aac_decoder);
				if (info) {
					aac->sample_rate = info->sampleRate;
					aac->channels = info->numChannels;
					aac->frame_size = info->frameSize;
				}
			}

			// Keep unconsumed data in buffer
			int consumed = aac->read_buf_size - bytesValid[0];
			if (bytesValid[0] > 0) {
				memmove(aac->read_buf, aac->read_buf + consumed, bytesValid[0]);
			}
			aac->read_buf_size = bytesValid[0];
		}

		if (aac->sample_rate == 0) {
			aacDecoder_Close(aac->aac_decoder);
			fclose(aac->file);
			free(aac->read_buf);
			free(aac);
			LOG_error("Stream: Failed to decode AAC header: %s\n", filepath);
			return -1;
		}

		// Estimate total PCM frames from bitrate
		CStreamInfo* aac_info = aacDecoder_GetStreamInfo(aac->aac_decoder);
		if (aac_info && aac_info->bitRate > 0) {
			double duration_sec = (double)aac->file_size * 8.0 / (double)aac_info->bitRate;
			sd->total_frames = (int64_t)(duration_sec * aac->sample_rate);
		} else {
			// Fallback: assume 128kbps
			sd->total_frames = (int64_t)((double)aac->file_size * 8.0 / 128000.0 * aac->sample_rate);
		}

		// Don't seek back — continue from where we are with remaining buffered data
		// This avoids re-reading and re-syncing from the start

		sd->decoder = aac;
		sd->source_sample_rate = aac->sample_rate;
		sd->source_channels = aac->channels;
		break;
	}
	default:
		LOG_error("Stream: Unsupported format for streaming: %d\n", sd->format);
		return -1;
	}

	sd->current_frame = 0;
	return 0;
}

// Read chunk of audio from decoder (returns frames read, outputs stereo)
static size_t stream_decoder_read(StreamDecoder* sd, int16_t* buffer, size_t frames) {
	if (!sd->decoder)
		return 0;

	size_t frames_read = 0;

	switch (sd->format) {
	case AUDIO_FORMAT_MP3: {
		drmp3* mp3 = (drmp3*)sd->decoder;
		if (sd->source_channels == 1) {
			// Read mono, convert to stereo
			int16_t* mono = malloc(frames * sizeof(int16_t));
			if (mono) {
				frames_read = drmp3_read_pcm_frames_s16(mp3, frames, mono);
				for (size_t i = 0; i < frames_read; i++) {
					buffer[i * 2] = mono[i];
					buffer[i * 2 + 1] = mono[i];
				}
				free(mono);
			}
		} else {
			frames_read = drmp3_read_pcm_frames_s16(mp3, frames, buffer);
		}
		break;
	}
	case AUDIO_FORMAT_WAV: {
		drwav* wav = (drwav*)sd->decoder;
		if (sd->source_channels == 1) {
			int16_t* mono = malloc(frames * sizeof(int16_t));
			if (mono) {
				frames_read = drwav_read_pcm_frames_s16(wav, frames, mono);
				for (size_t i = 0; i < frames_read; i++) {
					buffer[i * 2] = mono[i];
					buffer[i * 2 + 1] = mono[i];
				}
				free(mono);
			}
		} else {
			frames_read = drwav_read_pcm_frames_s16(wav, frames, buffer);
		}
		break;
	}
	case AUDIO_FORMAT_FLAC: {
		drflac* flac = (drflac*)sd->decoder;
		if (sd->source_channels == 1) {
			int16_t* mono = malloc(frames * sizeof(int16_t));
			if (mono) {
				frames_read = drflac_read_pcm_frames_s16(flac, frames, mono);
				for (size_t i = 0; i < frames_read; i++) {
					buffer[i * 2] = mono[i];
					buffer[i * 2 + 1] = mono[i];
				}
				free(mono);
			}
		} else {
			frames_read = drflac_read_pcm_frames_s16(flac, frames, buffer);
		}
		break;
	}
	case AUDIO_FORMAT_OGG: {
		stb_vorbis* vorbis = (stb_vorbis*)sd->decoder;
		// stb_vorbis always outputs interleaved, can handle stereo conversion
		frames_read = stb_vorbis_get_samples_short_interleaved(
			vorbis, AUDIO_CHANNELS, buffer, frames * AUDIO_CHANNELS);
		break;
	}
	case AUDIO_FORMAT_OPUS: {
		int ret = op_read_stereo((OggOpusFile*)sd->decoder, buffer, frames * 2);
		frames_read = (ret > 0) ? (size_t)ret : 0;
		break;
	}
	case AUDIO_FORMAT_M4A: {
		M4ADecoder* m4a = (M4ADecoder*)sd->decoder;

		// Decode AAC frames until we have enough PCM samples
		size_t buffer_pos = 0; // Current position in output buffer (in frames)

		// First, copy any leftover samples from previous decode
		if (m4a->leftover_count > 0 && m4a->leftover_buffer) {
			size_t to_copy = m4a->leftover_count;
			if (to_copy > frames) {
				to_copy = frames;
			}
			memcpy(buffer, m4a->leftover_buffer, to_copy * sizeof(int16_t) * 2);
			buffer_pos = to_copy;

			// Shift remaining leftovers to front of buffer
			size_t remaining = m4a->leftover_count - to_copy;
			if (remaining > 0) {
				memmove(m4a->leftover_buffer, &m4a->leftover_buffer[to_copy * 2],
						remaining * sizeof(int16_t) * 2);
			}
			m4a->leftover_count = remaining;
		}

		while (buffer_pos < frames && m4a->current_sample < m4a->sample_count) {
			// Get frame offset and size
			unsigned frame_bytes = 0;
			unsigned timestamp = 0;
			unsigned duration = 0;
			MP4D_file_offset_t offset = MP4D_frame_offset(
				&m4a->mp4, m4a->audio_track, m4a->current_sample,
				&frame_bytes, &timestamp, &duration);

			if (offset == 0 || frame_bytes == 0) {
				m4a->current_sample++;
				continue;
			}

			// Read AAC frame data
			if (frame_bytes > m4a->frame_buffer_size) {
				// Resize buffer if needed
				uint8_t* new_buf = realloc(m4a->frame_buffer, frame_bytes);
				if (!new_buf) {
					break;
				}
				m4a->frame_buffer = new_buf;
				m4a->frame_buffer_size = frame_bytes;
			}

			if (fseek(m4a->file, (long)offset, SEEK_SET) != 0) {
				break;
			}
			if (fread(m4a->frame_buffer, 1, frame_bytes, m4a->file) != frame_bytes) {
				break;
			}

			// Decode AAC frame using FDK-AAC
			// FDK-AAC decode buffer: 2048 frames * 2 channels (HE-AAC can output 2048 frames)
			INT_PCM decode_buf[2048 * 2];
			UCHAR* inBuffer[] = {m4a->frame_buffer};
			UINT inBufferLength[] = {frame_bytes};
			UINT bytesValid[] = {frame_bytes};

			aacDecoder_Fill(m4a->aac_decoder, inBuffer, inBufferLength, bytesValid);
			AAC_DECODER_ERROR err = aacDecoder_DecodeFrame(m4a->aac_decoder, decode_buf, sizeof(decode_buf) / sizeof(INT_PCM), 0);

			if (IS_OUTPUT_VALID(err)) {
				CStreamInfo* info = aacDecoder_GetStreamInfo(m4a->aac_decoder);

				if (info && info->frameSize > 0) {
					int decoded_channels = info->numChannels;
					int decoded_frames = info->frameSize;
					int frames_to_copy = decoded_frames;
					int leftover_frames = 0;

					// Check if we'll overflow output buffer
					if (buffer_pos + frames_to_copy > frames) {
						frames_to_copy = frames - buffer_pos;
						leftover_frames = decoded_frames - frames_to_copy;
					}

					// Copy to output buffer, handling mono to stereo conversion
					if (decoded_channels == 1) {
						for (int i = 0; i < frames_to_copy; i++) {
							buffer[(buffer_pos + i) * 2] = decode_buf[i];
							buffer[(buffer_pos + i) * 2 + 1] = decode_buf[i];
						}
					} else {
						memcpy(&buffer[buffer_pos * 2], decode_buf,
							   frames_to_copy * sizeof(int16_t) * 2);
					}

					buffer_pos += frames_to_copy;

					// Store leftover samples for next call
					if (leftover_frames > 0) {
						// Ensure leftover buffer has enough capacity
						if ((size_t)leftover_frames > m4a->leftover_capacity) {
							size_t new_cap = leftover_frames + 256; // Add some headroom
							int16_t* new_buf = realloc(m4a->leftover_buffer,
													   new_cap * sizeof(int16_t) * 2);
							if (new_buf) {
								m4a->leftover_buffer = new_buf;
								m4a->leftover_capacity = new_cap;
							} else {
								// Can't store leftovers, they'll be lost
								leftover_frames = 0;
							}
						}

						if (leftover_frames > 0) {
							// Copy leftover samples (already stereo or converted above)
							if (decoded_channels == 1) {
								for (int i = 0; i < leftover_frames; i++) {
									m4a->leftover_buffer[i * 2] = decode_buf[frames_to_copy + i];
									m4a->leftover_buffer[i * 2 + 1] = decode_buf[frames_to_copy + i];
								}
							} else {
								memcpy(m4a->leftover_buffer, &decode_buf[frames_to_copy * 2],
									   leftover_frames * sizeof(int16_t) * 2);
							}
							m4a->leftover_count = leftover_frames;
						}
					}
				}
			}

			m4a->current_sample++;
		}

		frames_read = buffer_pos;
		break;
	}
	case AUDIO_FORMAT_AAC: {
		AACFileDecoder* aac = (AACFileDecoder*)sd->decoder;
		size_t buffer_pos = 0;

		// First, copy any leftover samples from previous decode
		if (aac->leftover_count > 0 && aac->leftover_buffer) {
			size_t to_copy = aac->leftover_count;
			if (to_copy > frames)
				to_copy = frames;
			memcpy(buffer, aac->leftover_buffer, to_copy * sizeof(int16_t) * 2);
			buffer_pos = to_copy;
			size_t remaining = aac->leftover_count - to_copy;
			if (remaining > 0) {
				memmove(aac->leftover_buffer, &aac->leftover_buffer[to_copy * 2],
						remaining * sizeof(int16_t) * 2);
			}
			aac->leftover_count = remaining;
		}

		while (buffer_pos < frames) {
			// Bulk read from file when buffer is less than half full
			if (aac->read_buf_size < AAC_FILE_READ_BUF_SIZE / 2) {
				int space = AAC_FILE_READ_BUF_SIZE - aac->read_buf_size;
				int bytes_read = fread(aac->read_buf + aac->read_buf_size, 1, space, aac->file);
				if (bytes_read > 0) {
					aac->read_buf_size += bytes_read;
				} else if (aac->read_buf_size == 0) {
					break; // EOF and no buffered data
				}
			}

			if (aac->read_buf_size == 0)
				break;

			// Feed entire buffer to FDK-AAC at once (it takes what it can)
			UCHAR* inBuf[] = {aac->read_buf};
			UINT inLen[] = {(UINT)aac->read_buf_size};
			UINT bytesValid[] = {(UINT)aac->read_buf_size};
			aacDecoder_Fill(aac->aac_decoder, inBuf, inLen, bytesValid);

			// Shift unconsumed data to front
			int consumed = aac->read_buf_size - bytesValid[0];
			if (consumed > 0 && bytesValid[0] > 0) {
				memmove(aac->read_buf, aac->read_buf + consumed, bytesValid[0]);
			}
			aac->read_buf_size = bytesValid[0];

			// Decode as many frames as possible from FDK's internal buffer
			bool need_more_data = false;
			while (buffer_pos < frames && !need_more_data) {
				INT_PCM decode_buf[2048 * 2];
				AAC_DECODER_ERROR err = aacDecoder_DecodeFrame(aac->aac_decoder, decode_buf, sizeof(decode_buf) / sizeof(INT_PCM), 0);

				if (IS_OUTPUT_VALID(err)) {
					CStreamInfo* info = aacDecoder_GetStreamInfo(aac->aac_decoder);
					if (info && info->frameSize > 0) {
						int decoded_channels = info->numChannels;
						int decoded_frames = info->frameSize;
						int frames_to_copy = decoded_frames;
						int leftover_frames = 0;

						if (buffer_pos + frames_to_copy > frames) {
							frames_to_copy = frames - buffer_pos;
							leftover_frames = decoded_frames - frames_to_copy;
						}

						if (decoded_channels == 1) {
							for (int i = 0; i < frames_to_copy; i++) {
								buffer[(buffer_pos + i) * 2] = decode_buf[i];
								buffer[(buffer_pos + i) * 2 + 1] = decode_buf[i];
							}
						} else {
							memcpy(&buffer[buffer_pos * 2], decode_buf,
								   frames_to_copy * sizeof(int16_t) * 2);
						}
						buffer_pos += frames_to_copy;

						if (leftover_frames > 0) {
							if ((size_t)leftover_frames > aac->leftover_capacity) {
								size_t new_cap = leftover_frames + 256;
								int16_t* new_buf = realloc(aac->leftover_buffer,
														   new_cap * sizeof(int16_t) * 2);
								if (new_buf) {
									aac->leftover_buffer = new_buf;
									aac->leftover_capacity = new_cap;
								} else {
									leftover_frames = 0;
								}
							}
							if (leftover_frames > 0) {
								if (decoded_channels == 1) {
									for (int i = 0; i < leftover_frames; i++) {
										aac->leftover_buffer[i * 2] = decode_buf[frames_to_copy + i];
										aac->leftover_buffer[i * 2 + 1] = decode_buf[frames_to_copy + i];
									}
								} else {
									memcpy(aac->leftover_buffer, &decode_buf[frames_to_copy * 2],
										   leftover_frames * sizeof(int16_t) * 2);
								}
								aac->leftover_count = leftover_frames;
							}
						}
					}
				} else if (err == AAC_DEC_NOT_ENOUGH_BITS) {
					if (feof(aac->file) && aac->read_buf_size == 0) {
						need_more_data = true; // True EOF
						break;
					}
					need_more_data = true; // Break inner loop to read more file data
				} else {
					need_more_data = true; // Break to refill
				}
			}

			// If we hit true EOF with no data left, stop
			if (feof(aac->file) && aac->read_buf_size == 0)
				break;
		}

		frames_read = buffer_pos;
		break;
	}
	default:
		break;
	}

	sd->current_frame += frames_read;
	return frames_read;
}

// Seek to frame position
static int stream_decoder_seek(StreamDecoder* sd, int64_t frame) {
	if (!sd->decoder)
		return -1;

	if (frame < 0)
		frame = 0;
	if (frame > sd->total_frames)
		frame = sd->total_frames;

	bool success = false;
	switch (sd->format) {
	case AUDIO_FORMAT_MP3:
		success = drmp3_seek_to_pcm_frame((drmp3*)sd->decoder, frame);
		break;
	case AUDIO_FORMAT_WAV:
		success = drwav_seek_to_pcm_frame((drwav*)sd->decoder, frame);
		break;
	case AUDIO_FORMAT_FLAC:
		success = drflac_seek_to_pcm_frame((drflac*)sd->decoder, frame);
		break;
	case AUDIO_FORMAT_OGG:
		success = (stb_vorbis_seek((stb_vorbis*)sd->decoder, (unsigned int)frame) != 0);
		break;
	case AUDIO_FORMAT_OPUS:
		success = (op_pcm_seek((OggOpusFile*)sd->decoder, frame) == 0);
		break;
	case AUDIO_FORMAT_M4A: {
		M4ADecoder* m4a = (M4ADecoder*)sd->decoder;
		// Convert PCM frame to AAC sample index
		// Each AAC frame typically produces 1024 PCM samples
		unsigned target_sample = (unsigned)(frame / 1024);
		if (target_sample >= m4a->sample_count) {
			target_sample = m4a->sample_count > 0 ? m4a->sample_count - 1 : 0;
		}
		m4a->current_sample = target_sample;
		// Flush FDK-AAC decoder state for clean seek
		// Use AACDEC_INTR to signal discontinuity, then decode a dummy frame to flush
		aacDecoder_SetParam(m4a->aac_decoder, AAC_TPDEC_CLEAR_BUFFER, 1);
		// Clear leftover buffer to avoid playing stale samples after seek
		m4a->leftover_count = 0;
		success = true;
		break;
	}
	case AUDIO_FORMAT_AAC: {
		AACFileDecoder* aac = (AACFileDecoder*)sd->decoder;
		// Estimate byte position from frame position
		if (sd->total_frames > 0 && aac->file_size > 0) {
			double ratio = (double)frame / (double)sd->total_frames;
			int64_t byte_pos = (int64_t)(ratio * aac->file_size);
			if (byte_pos >= aac->file_size)
				byte_pos = aac->file_size - 1;
			if (byte_pos < 0)
				byte_pos = 0;
			fseek(aac->file, (long)byte_pos, SEEK_SET);
		} else {
			fseek(aac->file, 0, SEEK_SET);
		}
		// Clear decoder state and buffers
		aac->read_buf_size = 0;
		aac->leftover_count = 0;
		aacDecoder_SetParam(aac->aac_decoder, AAC_TPDEC_CLEAR_BUFFER, 1);
		success = true;
		break;
	}
	default:
		break;
	}

	if (success) {
		sd->current_frame = frame;
		return 0;
	}
	return -1;
}

// Close decoder
static void stream_decoder_close(StreamDecoder* sd) {
	if (!sd->decoder)
		return;

	switch (sd->format) {
	case AUDIO_FORMAT_MP3:
		drmp3_uninit((drmp3*)sd->decoder);
		free(sd->decoder);
		break;
	case AUDIO_FORMAT_WAV:
		drwav_uninit((drwav*)sd->decoder);
		free(sd->decoder);
		break;
	case AUDIO_FORMAT_FLAC:
		drflac_close((drflac*)sd->decoder);
		break;
	case AUDIO_FORMAT_OGG:
		stb_vorbis_close((stb_vorbis*)sd->decoder);
		break;
	case AUDIO_FORMAT_OPUS:
		op_free((OggOpusFile*)sd->decoder);
		break;
	case AUDIO_FORMAT_M4A: {
		M4ADecoder* m4a = (M4ADecoder*)sd->decoder;
		if (m4a->aac_decoder) {
			aacDecoder_Close(m4a->aac_decoder);
		}
		if (m4a->frame_buffer) {
			free(m4a->frame_buffer);
		}
		if (m4a->leftover_buffer) {
			free(m4a->leftover_buffer);
		}
		MP4D_close(&m4a->mp4);
		if (m4a->file) {
			fclose(m4a->file);
		}
		free(m4a);
		break;
	}
	case AUDIO_FORMAT_AAC: {
		AACFileDecoder* aac = (AACFileDecoder*)sd->decoder;
		if (aac->aac_decoder) {
			aacDecoder_Close(aac->aac_decoder);
		}
		if (aac->read_buf) {
			free(aac->read_buf);
		}
		if (aac->leftover_buffer) {
			free(aac->leftover_buffer);
		}
		if (aac->file) {
			fclose(aac->file);
		}
		free(aac);
		break;
	}
	default:
		break;
	}

	sd->decoder = NULL;
	sd->format = AUDIO_FORMAT_UNKNOWN;
}

// ============ STREAMING RESAMPLER ============

// Resample a chunk of audio (for streaming)
// Returns number of output frames
// Tracks unconsumed input frames in player.resample_leftover for next call
static size_t resample_chunk(int16_t* input, size_t input_frames,
							 int src_rate, int dst_rate,
							 int16_t* output, size_t max_output_frames,
							 SRC_STATE* src_state, bool is_last) {
	if (src_rate == dst_rate) {
		// No resampling needed, just copy
		size_t to_copy = (input_frames < max_output_frames) ? input_frames : max_output_frames;
		memcpy(output, input, to_copy * sizeof(int16_t) * AUDIO_CHANNELS);
		return to_copy;
	}

	double ratio = (double)dst_rate / (double)src_rate;

	// Calculate total input frames (leftover from previous call + new input)
	size_t leftover_count = player.resample_leftover_count;
	size_t total_input_frames = leftover_count + input_frames;

	// Allocate buffers for combined input
	float* float_in = malloc(total_input_frames * AUDIO_CHANNELS * sizeof(float));
	float* float_out = malloc(max_output_frames * AUDIO_CHANNELS * sizeof(float));
	if (!float_in || !float_out) {
		free(float_in);
		free(float_out);
		return 0;
	}

	// Convert leftover frames to float first
	size_t float_idx = 0;
	if (leftover_count > 0 && player.resample_leftover) {
		for (size_t i = 0; i < leftover_count * AUDIO_CHANNELS; i++) {
			float_in[float_idx++] = player.resample_leftover[i] / 32768.0f;
		}
	}

	// Convert new input frames to float
	for (size_t i = 0; i < input_frames * AUDIO_CHANNELS; i++) {
		float_in[float_idx++] = input[i] / 32768.0f;
	}

	// Setup conversion
	SRC_DATA src_data;
	src_data.data_in = float_in;
	src_data.data_out = float_out;
	src_data.input_frames = total_input_frames;
	src_data.output_frames = max_output_frames;
	src_data.src_ratio = ratio;
	src_data.end_of_input = is_last ? 1 : 0;

	int error = src_process(src_state, &src_data);
	if (error) {
		LOG_error("Resample chunk failed: %s\n", src_strerror(error));
		free(float_in);
		free(float_out);
		return 0;
	}

	// Convert output back to int16
	size_t output_frames = src_data.output_frames_gen;
	for (size_t i = 0; i < output_frames * AUDIO_CHANNELS; i++) {
		float sample = float_out[i] * 32767.0f;
		if (sample > 32767.0f)
			sample = 32767.0f;
		if (sample < -32768.0f)
			sample = -32768.0f;
		output[i] = (int16_t)sample;
	}

	// Store unconsumed input frames for next call
	size_t frames_used = src_data.input_frames_used;
	size_t unconsumed = total_input_frames - frames_used;

	if (unconsumed > 0 && !is_last) {
		// Ensure leftover buffer has enough capacity
		if (unconsumed > player.resample_leftover_capacity) {
			int16_t* new_buf = realloc(player.resample_leftover,
									   unconsumed * AUDIO_CHANNELS * sizeof(int16_t));
			if (new_buf) {
				player.resample_leftover = new_buf;
				player.resample_leftover_capacity = unconsumed;
			} else {
				// Can't store leftovers, they'll be lost (but at least don't crash)
				unconsumed = player.resample_leftover_capacity;
			}
		}

		// Convert unconsumed float samples back to int16 and store
		// The unconsumed frames are at the end of float_in
		size_t start_idx = frames_used * AUDIO_CHANNELS;
		for (size_t i = 0; i < unconsumed * AUDIO_CHANNELS; i++) {
			float sample = float_in[start_idx + i] * 32768.0f;
			if (sample > 32767.0f)
				sample = 32767.0f;
			if (sample < -32768.0f)
				sample = -32768.0f;
			player.resample_leftover[i] = (int16_t)sample;
		}
		player.resample_leftover_count = unconsumed;
	} else {
		player.resample_leftover_count = 0;
	}

	free(float_in);
	free(float_out);
	return output_frames;
}

// ============ STREAMING DECODE THREAD ============

static void* stream_thread_func(void* arg) {
	(void)arg;

	// Allocate decode buffer
	int16_t* decode_buffer = malloc(DECODE_CHUNK_FRAMES * sizeof(int16_t) * AUDIO_CHANNELS);
	// Resample output buffer (allow for 2x expansion)
	size_t resample_buffer_size = DECODE_CHUNK_FRAMES * 3;
	int16_t* resample_buffer = malloc(resample_buffer_size * sizeof(int16_t) * AUDIO_CHANNELS);

	if (!decode_buffer || !resample_buffer) {
		LOG_error("Stream thread: Failed to allocate buffers\n");
		free(decode_buffer);
		free(resample_buffer);
		return NULL;
	}

	while (player.stream_running) {
		// Check if seeking requested
		if (player.stream_seeking) {
			stream_decoder_seek(&player.stream_decoder, player.seek_target_frame);
			circular_buffer_clear(&player.stream_buffer);
			if (player.resampler) {
				src_reset((SRC_STATE*)player.resampler);
			}
			// Clear resampler leftover buffer to avoid playing stale samples
			player.resample_leftover_count = 0;
			player.stream_eof = false; // Reset EOF flag on seek
			player.stream_seeking = false;
		}

		// Check if buffer needs more data (< 50% full)
		size_t available = circular_buffer_available(&player.stream_buffer);
		if (available < STREAM_BUFFER_FRAMES / 2) {
			// Decode a chunk
			size_t decoded = stream_decoder_read(&player.stream_decoder,
												 decode_buffer, DECODE_CHUNK_FRAMES);
			if (decoded == 0) {
				// Decoder has reached end of file
				player.stream_eof = true;
			} else {
				// Resample chunk to target rate if needed
				int src_rate = player.stream_decoder.source_sample_rate;
				int dst_rate = get_target_sample_rate();
				bool is_last = (player.stream_decoder.current_frame >= player.stream_decoder.total_frames);

				size_t output_frames;
				if (src_rate == dst_rate) {
					// No resampling needed
					output_frames = decoded;
					circular_buffer_write(&player.stream_buffer, decode_buffer, output_frames);
				} else {
					// Resample
					output_frames = resample_chunk(decode_buffer, decoded,
												   src_rate, dst_rate,
												   resample_buffer, resample_buffer_size,
												   (SRC_STATE*)player.resampler, is_last);
					circular_buffer_write(&player.stream_buffer, resample_buffer, output_frames);
				}
			}
		} else {
			// Buffer full enough, sleep briefly
			usleep(5000); // 5ms
		}
	}

	free(decode_buffer);
	free(resample_buffer);
	return NULL;
}

// ============ END STREAMING PLAYBACK SYSTEM ============

// Audio callback - SDL pulls audio data from here
static void audio_callback(void* userdata, Uint8* stream, int len) {
	PlayerContext* ctx = (PlayerContext*)userdata;
	int samples_needed = len / (sizeof(int16_t) * AUDIO_CHANNELS);
	int16_t* out = (int16_t*)stream;

	// Check if radio is active - handle radio audio separately
	if (Radio_isActive()) {
		RadioState state = Radio_getState();
		// Get audio from radio module for both PLAYING and BUFFERING states
		// This ensures we drain remaining buffer during rebuffer instead of abrupt silence
		if (state == RADIO_STATE_PLAYING || state == RADIO_STATE_BUFFERING) {
			// Get audio from radio module
			int samples_got = Radio_getAudioSamples(out, samples_needed * AUDIO_CHANNELS);

			// If we got less than needed, fill rest with silence
			if (samples_got < samples_needed * AUDIO_CHANNELS) {
				memset(&out[samples_got], 0, (samples_needed * AUDIO_CHANNELS - samples_got) * sizeof(int16_t));
			}

			// Apply volume with logarithmic curve for natural perceived loudness
			if (ctx->volume < 0.99f || ctx->volume > 1.01f) {
				float curved_vol = apply_volume_curve(ctx->volume);
				for (int i = 0; i < samples_needed * AUDIO_CHANNELS; i++) {
					out[i] = (int16_t)(out[i] * curved_vol);
				}
			}

			// Speaker processing: high-pass filter + soft limiter
			if (!bluetooth_audio_active && !usbdac_audio_active) {
				int bass_hz = Settings_getBassFilterHz();
				float limiter_thresh = Settings_getSoftLimiterThreshold();
				if (bass_hz != speaker_hpf_last_hz) {
					if (bass_hz > 0)
						speaker_hpf_init(current_sample_rate, (float)bass_hz);
					speaker_hpf_last_hz = bass_hz;
				}
				for (int i = 0; i < samples_needed * AUDIO_CHANNELS; i++) {
					if (bass_hz > 0)
						out[i] = speaker_hpf_process(out[i], i % AUDIO_CHANNELS);
					if (limiter_thresh > 0.0f)
						out[i] = speaker_soft_limit(out[i], limiter_thresh);
				}
			}
		} else {
			// CONNECTING or other states - output silence
			memset(stream, 0, len);
		}

		return;
	}

	// Try to lock, if can't, output silence (non-blocking to prevent crackling)
	if (pthread_mutex_trylock(&ctx->mutex) != 0) {
		memset(stream, 0, len);
		return;
	}

	if (ctx->state != PLAYER_STATE_PLAYING) {
		memset(stream, 0, len);
		pthread_mutex_unlock(&ctx->mutex);
		return;
	}

	// ============ STREAMING MODE ============
	if (ctx->use_streaming) {
		// Read from circular buffer
		size_t samples_read = circular_buffer_read(&ctx->stream_buffer, out, samples_needed);

		// If not enough data, fill rest with silence
		if (samples_read < (size_t)samples_needed) {
			memset(&out[samples_read * AUDIO_CHANNELS], 0,
				   (samples_needed - samples_read) * sizeof(int16_t) * AUDIO_CHANNELS);
		}

		// Apply volume with logarithmic curve for natural perceived loudness
		if (ctx->volume < 0.99f || ctx->volume > 1.01f) {
			float curved_vol = apply_volume_curve(ctx->volume);
			for (size_t i = 0; i < samples_read * AUDIO_CHANNELS; i++) {
				out[i] = (int16_t)(out[i] * curved_vol);
			}
		}

		// Speaker processing: high-pass filter + soft limiter
		if (!bluetooth_audio_active && !usbdac_audio_active) {
			int bass_hz = Settings_getBassFilterHz();
			float limiter_thresh = Settings_getSoftLimiterThreshold();
			if (bass_hz != speaker_hpf_last_hz) {
				if (bass_hz > 0)
					speaker_hpf_init(current_sample_rate, (float)bass_hz);
				speaker_hpf_last_hz = bass_hz;
			}
			for (size_t i = 0; i < samples_read * AUDIO_CHANNELS; i++) {
				if (bass_hz > 0)
					out[i] = speaker_hpf_process(out[i], i % AUDIO_CHANNELS);
				if (limiter_thresh > 0.0f)
					out[i] = speaker_soft_limit(out[i], limiter_thresh);
			}
		}

		// Copy to visualization buffer (non-blocking)
		if (samples_read > 0 && pthread_mutex_trylock(&ctx->vis_mutex) == 0) {
			int vis_samples = samples_read * AUDIO_CHANNELS;
			if (vis_samples > 2048)
				vis_samples = 2048;
			memcpy(ctx->vis_buffer, out, vis_samples * sizeof(int16_t));
			ctx->vis_buffer_pos = vis_samples;
			pthread_mutex_unlock(&ctx->vis_mutex);
		}

		// Update position
		audio_position_samples += samples_read;
		ctx->position_ms = (audio_position_samples * 1000) / current_sample_rate;

		// Check if track ended (decoder reached EOF or frame count)
		if ((ctx->stream_decoder.current_frame >= ctx->stream_decoder.total_frames || ctx->stream_eof) &&
			circular_buffer_available(&ctx->stream_buffer) == 0) {
			if (ctx->repeat) {
				// Seek back to beginning
				ctx->seek_target_frame = 0;
				ctx->stream_seeking = true;
				audio_position_samples = 0;
				ctx->position_ms = 0;
			} else {
				ctx->state = PLAYER_STATE_STOPPED;
				audio_position_samples = 0;
				ctx->position_ms = 0;
			}
		}

		pthread_mutex_unlock(&ctx->mutex);
		return;
	}

	// No audio loaded - output silence
	memset(stream, 0, len);
	pthread_mutex_unlock(&ctx->mutex);
}

int Player_init(void) {
	memset(&player, 0, sizeof(PlayerContext));

	pthread_mutex_init(&player.mutex, NULL);
	pthread_mutex_init(&player.vis_mutex, NULL);

	player.volume = 1.0f;
	player.state = PLAYER_STATE_STOPPED;

	// Initialize SDL audio
	if (SDL_InitSubSystem(SDL_INIT_AUDIO) < 0) {
		LOG_error("Failed to init SDL audio: %s\n", SDL_GetError());
		return -1;
	}

	// Check current audio sink setting
	int audio_sink = GetAudioSink();

	// Set USB DAC flag if that's the current sink
	if (audio_sink == AUDIO_SINK_USBDAC) {
		usbdac_audio_active = true;
	}

	// Also check if .asoundrc exists with bluealsa config (more reliable than msettings)
	const char* home = getenv("HOME");

	if (home) {
		char asoundrc_path[512];
		snprintf(asoundrc_path, sizeof(asoundrc_path), "%s/.asoundrc", home);
		FILE* f = fopen(asoundrc_path, "r");
		if (f) {
			char buf[256];
			while (fgets(buf, sizeof(buf), f)) {
				if (strstr(buf, "bluealsa")) {
					audio_sink = AUDIO_SINK_BLUETOOTH;
					bluetooth_audio_active = true;
					break;
				}
			}
			fclose(f);
		}
	}

	// If Bluetooth audio is detected, set BlueALSA mixer to 100% for software volume control
	if (audio_sink == AUDIO_SINK_BLUETOOTH) {
		// Set all mixer controls that contain "A2DP" in their name to 100%
		// This handles devices like "Galaxy Buds Live (4B23 A2DP" etc.
		system("amixer scontrols 2>/dev/null | grep -i 'A2DP' | "
			   "sed \"s/.*'\\([^']*\\)'.*/\\1/\" | "
			   "while read ctrl; do amixer sset \"$ctrl\" 127 2>/dev/null; done");
		// Initialize HID input monitoring for Bluetooth AVRCP buttons
		Player_initUSBHID();
	}

	// If USB DAC is detected, set its mixer to 100% for software volume control
	if (audio_sink == AUDIO_SINK_USBDAC) {
		// USB DACs typically appear as card 1, set common mixer controls to 100%
		// Different USB DACs use different control names (PCM, Master, Headset, etc.)
		system("amixer -c 1 sset PCM 100% 2>/dev/null; "
			   "amixer -c 1 sset Master 100% 2>/dev/null; "
			   "amixer -c 1 sset Speaker 100% 2>/dev/null; "
			   "amixer -c 1 sset Headphone 100% 2>/dev/null; "
			   "amixer -c 1 sset Headset 100% 2>/dev/null");
		// Initialize USB HID input monitoring for earphone buttons
		Player_initUSBHID();
	}

	// Determine target sample rate based on audio output
	int target_rate = get_target_sample_rate();

	SDL_AudioSpec want, have;
	SDL_zero(want);
	want.freq = target_rate;
	want.format = AUDIO_S16SYS;
	want.channels = AUDIO_CHANNELS;
	want.samples = AUDIO_SAMPLES;
	want.callback = audio_callback;
	want.userdata = &player;

	// Open default audio device - respects .asoundrc for Bluetooth/USB DAC routing
	player.audio_device = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);

	if (player.audio_device == 0) {
		LOG_error("Failed to open audio device: %s\n", SDL_GetError());

		// If Bluetooth was detected but device isn't available, fall back to speaker
		if (bluetooth_audio_active) {
			bluetooth_audio_active = false;

			// Retry with speaker sample rate and explicit device name to bypass .asoundrc
			want.freq = SAMPLE_RATE_SPEAKER;

			// Try to open the first available audio device explicitly
			int num_devices = SDL_GetNumAudioDevices(0);
			for (int i = 0; i < num_devices; i++) {
				const char* device_name = SDL_GetAudioDeviceName(i, 0);
				player.audio_device = SDL_OpenAudioDevice(device_name, 0, &want, &have, 0);
				if (player.audio_device != 0) {
					break;
				}
			}

			if (player.audio_device == 0) {
				LOG_error("All fallback audio devices failed\n");
				return -1;
			}
		} else {
			return -1;
		}
	}

	player.audio_initialized = true;
	{
		int bass_hz = Settings_getBassFilterHz();
		if (bass_hz > 0)
			speaker_hpf_init(current_sample_rate, (float)bass_hz);
	}

	// Register for audio device changes (Bluetooth, USB DAC, etc.)
	PLAT_audioDeviceWatchRegister(audio_device_change_callback);

	return 0;
}

// Reconfigure audio device with a new sample rate
static int reconfigure_audio_device(int new_sample_rate) {
	if (new_sample_rate == current_sample_rate && player.audio_device > 0) {
		return 0; // No change needed
	}

	// Pause and close existing device
	if (player.audio_device > 0) {
		SDL_PauseAudioDevice(player.audio_device, 1);
		SDL_CloseAudioDevice(player.audio_device);
		player.audio_device = 0;
	}

	// Open with new sample rate
	SDL_AudioSpec want, have;
	SDL_zero(want);
	want.freq = new_sample_rate;
	want.format = AUDIO_S16SYS;
	want.channels = AUDIO_CHANNELS;
	want.samples = AUDIO_SAMPLES;
	want.callback = audio_callback;
	want.userdata = &player;

	player.audio_device = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
	if (player.audio_device == 0) {
		LOG_error("Failed to open audio device at %d Hz: %s\n", new_sample_rate, SDL_GetError());
		// Try to reopen at target rate for current audio sink
		int fallback_rate = get_target_sample_rate();
		want.freq = fallback_rate;
		player.audio_device = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
		if (player.audio_device == 0) {
			return -1;
		}
	}

	current_sample_rate = have.freq;
	{
		int bass_hz = Settings_getBassFilterHz();
		if (bass_hz > 0)
			speaker_hpf_init(current_sample_rate, (float)bass_hz);
	}
	return 0;
}

// Reopen audio device (called when audio sink changes, e.g., Bluetooth connect/disconnect)
static void reopen_audio_device(void) {
	// Remember current playback state
	PlayerState prev_state = player.state;

	// Pause and close existing device
	if (player.audio_device > 0) {
		SDL_PauseAudioDevice(player.audio_device, 1);
		SDL_CloseAudioDevice(player.audio_device);
		player.audio_device = 0;
	}

	// Get target sample rate for the new audio sink
	int target_rate = get_target_sample_rate();

	// Reopen with target sample rate
	SDL_AudioSpec want, have;
	SDL_zero(want);
	want.freq = target_rate;
	want.format = AUDIO_S16SYS;
	want.channels = AUDIO_CHANNELS;
	want.samples = AUDIO_SAMPLES;
	want.callback = audio_callback;
	want.userdata = &player;

	player.audio_device = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
	if (player.audio_device == 0) {
		LOG_error("Failed to reopen audio device: %s\n", SDL_GetError());
		return;
	}

	current_sample_rate = have.freq;
	{
		int bass_hz = Settings_getBassFilterHz();
		if (bass_hz > 0)
			speaker_hpf_init(current_sample_rate, (float)bass_hz);
	}

	// Resume playback if it was playing
	if (prev_state == PLAYER_STATE_PLAYING) {
		SDL_PauseAudioDevice(player.audio_device, 0);
	}
}

// Callback for audio device changes (Bluetooth connect/disconnect, USB DAC, etc.)
static void audio_device_change_callback(int device_type, int event) {
	(void)device_type;
	(void)event;

	// Re-check if Bluetooth is now active/inactive
	bool was_bluetooth = bluetooth_audio_active;
	bool was_usbdac = usbdac_audio_active;
	bluetooth_audio_active = false;

	// Update USB DAC status
	usbdac_audio_active = (GetAudioSink() == AUDIO_SINK_USBDAC);

	const char* home = getenv("HOME");
	if (home) {
		char asoundrc_path[512];
		snprintf(asoundrc_path, sizeof(asoundrc_path), "%s/.asoundrc", home);
		FILE* f = fopen(asoundrc_path, "r");
		if (f) {
			char buf[256];
			while (fgets(buf, sizeof(buf), f)) {
				if (strstr(buf, "bluealsa")) {
					bluetooth_audio_active = true;
					break;
				}
			}
			fclose(f);
		}
	}

	if (was_bluetooth != bluetooth_audio_active) {
		// If Bluetooth just activated, set mixer to 100% and init HID
		if (bluetooth_audio_active) {
			system("amixer scontrols 2>/dev/null | grep -i 'A2DP' | "
				   "sed \"s/.*'\\([^']*\\)'.*/\\1/\" | "
				   "while read ctrl; do amixer sset \"$ctrl\" 127 2>/dev/null; done");
			// Initialize HID input monitoring for Bluetooth AVRCP buttons
			Player_initUSBHID();
		} else if (!usbdac_audio_active) {
			// Bluetooth disconnected and no USB DAC, close HID
			Player_quitUSBHID();
		}
	}

	// If USB DAC just activated, set its mixer to 100%
	if (!was_usbdac && usbdac_audio_active) {
		system("amixer -c 1 sset PCM 100% 2>/dev/null; "
			   "amixer -c 1 sset Master 100% 2>/dev/null; "
			   "amixer -c 1 sset Speaker 100% 2>/dev/null; "
			   "amixer -c 1 sset Headphone 100% 2>/dev/null; "
			   "amixer -c 1 sset Headset 100% 2>/dev/null");
		// Initialize USB HID input monitoring for earphone buttons
		Player_initUSBHID();
	} else if (was_usbdac && !usbdac_audio_active && !bluetooth_audio_active) {
		// USB DAC disconnected and no Bluetooth, close HID
		Player_quitUSBHID();
	}

	reopen_audio_device();
}

void Player_quit(void) {
	// Unregister audio device watcher
	PLAT_audioDeviceWatchUnregister();

	// Close USB HID input
	Player_quitUSBHID();

	Player_stop();

	if (player.audio_device > 0) {
		SDL_CloseAudioDevice(player.audio_device);
		player.audio_device = 0;
	}

	SDL_QuitSubSystem(SDL_INIT_AUDIO);

	pthread_mutex_destroy(&player.mutex);
	pthread_mutex_destroy(&player.vis_mutex);

	player.audio_initialized = false;
}

// ============ METADATA PARSING ============

// Helper: read syncsafe integer (ID3v2)
static uint32_t read_syncsafe_int(const uint8_t* data) {
	return ((uint32_t)(data[0] & 0x7F) << 21) |
		   ((uint32_t)(data[1] & 0x7F) << 14) |
		   ((uint32_t)(data[2] & 0x7F) << 7) |
		   ((uint32_t)(data[3] & 0x7F));
}

// Helper: read big-endian 32-bit integer
static uint32_t read_be32(const uint8_t* data) {
	return ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16) |
		   ((uint32_t)data[2] << 8) | (uint32_t)data[3];
}

// Helper: copy string, trimming trailing spaces
static void copy_metadata_string(char* dest, const char* src, size_t max_len) {
	if (!src || !dest || max_len == 0)
		return;

	size_t len = strlen(src);
	if (len >= max_len)
		len = max_len - 1;

	memcpy(dest, src, len);
	dest[len] = '\0';

	// Trim trailing spaces and nulls
	while (len > 0 && (dest[len - 1] == ' ' || dest[len - 1] == '\0')) {
		dest[--len] = '\0';
	}
}

// Helper: convert UTF-16LE to ASCII (for ID3v2 text frames)
static void utf16le_to_ascii(char* dest, const uint8_t* src, size_t src_len, size_t max_len) {
	if (!dest || !src || max_len == 0)
		return;

	size_t j = 0;
	for (size_t i = 0; i + 1 < src_len && j < max_len - 1; i += 2) {
		// Get UTF-16LE character (low byte first)
		uint16_t ch = src[i] | (src[i + 1] << 8);

		// Only copy ASCII range characters
		if (ch > 0 && ch < 128) {
			dest[j++] = (char)ch;
		} else if (ch >= 128 && ch < 256) {
			// Latin-1 supplement - just use low byte
			dest[j++] = (char)ch;
		}
		// Skip non-ASCII/non-Latin1 characters
	}
	dest[j] = '\0';
}

// Helper: convert UTF-16BE to ASCII (for ID3v2 text frames)
static void utf16be_to_ascii(char* dest, const uint8_t* src, size_t src_len, size_t max_len) {
	if (!dest || !src || max_len == 0)
		return;

	size_t j = 0;
	for (size_t i = 0; i + 1 < src_len && j < max_len - 1; i += 2) {
		// Get UTF-16BE character (high byte first)
		uint16_t ch = (src[i] << 8) | src[i + 1];

		// Only copy ASCII range characters
		if (ch > 0 && ch < 128) {
			dest[j++] = (char)ch;
		} else if (ch >= 128 && ch < 256) {
			// Latin-1 supplement
			dest[j++] = (char)ch;
		}
	}
	dest[j] = '\0';
}

// Parse ID3v1 tag (at end of file, 128 bytes)
static void parse_id3v1(const char* filepath) {
	FILE* f = fopen(filepath, "rb");
	if (!f)
		return;

	// Seek to last 128 bytes
	if (fseek(f, -128, SEEK_END) != 0) {
		fclose(f);
		return;
	}

	uint8_t tag[128];
	if (fread(tag, 1, 128, f) != 128) {
		fclose(f);
		return;
	}
	fclose(f);

	// Check for "TAG" header
	if (tag[0] != 'T' || tag[1] != 'A' || tag[2] != 'G') {
		return;
	}

	// ID3v1 layout: TAG(3) + Title(30) + Artist(30) + Album(30) + Year(4) + Comment(30) + Genre(1)
	char buf[31];

	// Title (bytes 3-32)
	if (player.track_info.title[0] == '\0' || strstr(player.track_info.title, ".") != NULL) {
		memcpy(buf, &tag[3], 30);
		buf[30] = '\0';
		copy_metadata_string(player.track_info.title, buf, sizeof(player.track_info.title));
	}

	// Artist (bytes 33-62)
	if (player.track_info.artist[0] == '\0') {
		memcpy(buf, &tag[33], 30);
		buf[30] = '\0';
		copy_metadata_string(player.track_info.artist, buf, sizeof(player.track_info.artist));
	}

	// Album (bytes 63-92)
	if (player.track_info.album[0] == '\0') {
		memcpy(buf, &tag[63], 30);
		buf[30] = '\0';
		copy_metadata_string(player.track_info.album, buf, sizeof(player.track_info.album));
	}
}

// Parse ID3v2 tag (at beginning of file)
static void parse_id3v2(const char* filepath) {
	FILE* f = fopen(filepath, "rb");
	if (!f)
		return;

	uint8_t header[10];
	if (fread(header, 1, 10, f) != 10) {
		fclose(f);
		return;
	}

	// Check for "ID3" header
	if (header[0] != 'I' || header[1] != 'D' || header[2] != '3') {
		fclose(f);
		return;
	}

	uint8_t version_major = header[3]; // 3 = ID3v2.3, 4 = ID3v2.4
	// uint8_t version_minor = header[4];
	// uint8_t flags = header[5];
	uint32_t tag_size = read_syncsafe_int(&header[6]);


	// Read entire tag
	uint8_t* tag_data = malloc(tag_size);
	if (!tag_data) {
		fclose(f);
		return;
	}

	if (fread(tag_data, 1, tag_size, f) != tag_size) {
		free(tag_data);
		fclose(f);
		return;
	}
	fclose(f);

	// Parse frames
	uint32_t pos = 0;
	while (pos + 10 < tag_size) {
		// Frame header: ID(4) + Size(4) + Flags(2)
		char frame_id[5];
		memcpy(frame_id, &tag_data[pos], 4);
		frame_id[4] = '\0';

		// Check for padding (all zeros)
		if (frame_id[0] == '\0')
			break;

		uint32_t frame_size;
		if (version_major == 4) {
			frame_size = read_syncsafe_int(&tag_data[pos + 4]);
		} else {
			frame_size = read_be32(&tag_data[pos + 4]);
		}

		// Skip flags
		pos += 10;

		if (frame_size == 0 || pos + frame_size > tag_size)
			break;

		// Process text frames (TIT2, TPE1, TALB, etc.)
		if (frame_id[0] == 'T' && frame_size > 1) {
			const uint8_t* frame_data = &tag_data[pos];
			uint8_t encoding = frame_data[0];
			const uint8_t* text_data = &frame_data[1];
			size_t text_len = frame_size - 1;

			char temp[256];
			temp[0] = '\0';

			// Handle different encodings
			// 0 = ISO-8859-1, 1 = UTF-16 with BOM, 2 = UTF-16BE, 3 = UTF-8
			if (encoding == 0 || encoding == 3) {
				// ISO-8859-1 or UTF-8: copy directly
				size_t copy_len = text_len < 255 ? text_len : 255;
				memcpy(temp, text_data, copy_len);
				temp[copy_len] = '\0';
			} else if (encoding == 1) {
				// UTF-16 with BOM
				if (text_len >= 2) {
					bool is_le = (text_data[0] == 0xFF && text_data[1] == 0xFE);
					bool is_be = (text_data[0] == 0xFE && text_data[1] == 0xFF);
					if (is_le || is_be) {
						text_data += 2;
						text_len -= 2;
					}
					if (is_be) {
						utf16be_to_ascii(temp, text_data, text_len, sizeof(temp));
					} else {
						// Default to LE
						utf16le_to_ascii(temp, text_data, text_len, sizeof(temp));
					}
				}
			} else if (encoding == 2) {
				// UTF-16BE without BOM
				utf16be_to_ascii(temp, text_data, text_len, sizeof(temp));
			}

			// Assign to appropriate field
			if (strcmp(frame_id, "TIT2") == 0 && temp[0]) { // Title
				copy_metadata_string(player.track_info.title, temp, sizeof(player.track_info.title));
			} else if (strcmp(frame_id, "TPE1") == 0 && temp[0]) { // Artist
				copy_metadata_string(player.track_info.artist, temp, sizeof(player.track_info.artist));
			} else if (strcmp(frame_id, "TALB") == 0 && temp[0]) { // Album
				copy_metadata_string(player.track_info.album, temp, sizeof(player.track_info.album));
			}
		}
		// Process APIC frame (album art) - only if we don't already have art
		else if (strcmp(frame_id, "APIC") == 0 && frame_size > 10 && player.album_art == NULL) {
			const uint8_t* frame_data = &tag_data[pos];
			uint8_t encoding = frame_data[0];
			size_t offset = 1;

			// Skip MIME type (null-terminated string)
			while (offset < frame_size && frame_data[offset] != '\0')
				offset++;
			offset++; // Skip null terminator

			if (offset < frame_size) {
				uint8_t pic_type = frame_data[offset];
				offset++;

				// Skip description (null-terminated, encoding-dependent)
				if (encoding == 1 || encoding == 2) {
					// UTF-16: look for double null
					while (offset + 1 < frame_size) {
						if (frame_data[offset] == 0 && frame_data[offset + 1] == 0) {
							offset += 2;
							break;
						}
						offset++;
					}
				} else {
					// ISO-8859-1 or UTF-8: single null
					while (offset < frame_size && frame_data[offset] != '\0')
						offset++;
					offset++;
				}

				// Now frame_data + offset points to the image data
				if (offset < frame_size) {
					size_t image_size = frame_size - offset;
					const uint8_t* image_data = &frame_data[offset];

					// Prefer front cover (type 3), but accept any if we have none
					if (pic_type == 3 || player.album_art == NULL) {
						SDL_RWops* rw = SDL_RWFromConstMem(image_data, image_size);
						if (rw) {
							SDL_Surface* art = IMG_Load_RW(rw, 1); // 1 = auto-close RWops
							if (art) {
								// Free previous art if we're replacing with front cover
								if (player.album_art) {
									SDL_FreeSurface(player.album_art);
								}
								player.album_art = art;
							}
						}
					}
				}
			}
		}

		pos += frame_size;
	}

	free(tag_data);
}

// Parse MP3 metadata (ID3v2 first, then ID3v1 as fallback)
static void parse_mp3_metadata(const char* filepath) {
	// Try ID3v2 first (more modern, more info)
	parse_id3v2(filepath);

	// Fall back to ID3v1 for any missing fields
	if (player.track_info.artist[0] == '\0' || player.track_info.album[0] == '\0') {
		parse_id3v1(filepath);
	}
}

// Parse M4A metadata from the already-opened decoder
static void parse_m4a_metadata(void) {
	if (player.stream_decoder.format != AUDIO_FORMAT_M4A || !player.stream_decoder.decoder) {
		return;
	}

	M4ADecoder* m4a = (M4ADecoder*)player.stream_decoder.decoder;

	// Copy metadata from minimp4's parsed tags
	if (m4a->mp4.tag.title && m4a->mp4.tag.title[0]) {
		copy_metadata_string(player.track_info.title, (const char*)m4a->mp4.tag.title,
							 sizeof(player.track_info.title));
	}

	if (m4a->mp4.tag.artist && m4a->mp4.tag.artist[0]) {
		copy_metadata_string(player.track_info.artist, (const char*)m4a->mp4.tag.artist,
							 sizeof(player.track_info.artist));
	}

	if (m4a->mp4.tag.album && m4a->mp4.tag.album[0]) {
		copy_metadata_string(player.track_info.album, (const char*)m4a->mp4.tag.album,
							 sizeof(player.track_info.album));
	}

	// Load cover art if present
	if (m4a->mp4.tag.cover && m4a->mp4.tag.cover_size > 0 && player.album_art == NULL) {
		SDL_RWops* rw = SDL_RWFromConstMem(m4a->mp4.tag.cover, m4a->mp4.tag.cover_size);
		if (rw) {
			SDL_Surface* art = IMG_Load_RW(rw, 1); // 1 = auto-close RWops
			if (art) {
				player.album_art = art;
			}
		}
	}
}

// Parse Vorbis comments (for OGG and FLAC)
static void parse_vorbis_comment(const char* comment) {
	if (!comment)
		return;

	// Vorbis comments are in format "KEY=VALUE"
	const char* eq = strchr(comment, '=');
	if (!eq)
		return;

	size_t key_len = eq - comment;
	const char* value = eq + 1;

	if (strncasecmp(comment, "TITLE", key_len) == 0 && key_len == 5) {
		copy_metadata_string(player.track_info.title, value, sizeof(player.track_info.title));
	} else if (strncasecmp(comment, "ARTIST", key_len) == 0 && key_len == 6) {
		copy_metadata_string(player.track_info.artist, value, sizeof(player.track_info.artist));
	} else if (strncasecmp(comment, "ALBUM", key_len) == 0 && key_len == 5) {
		copy_metadata_string(player.track_info.album, value, sizeof(player.track_info.album));
	}
}

// FLAC metadata callback
static void flac_metadata_callback(void* pUserData, drflac_metadata* pMetadata) {
	(void)pUserData;

	if (pMetadata->type == DRFLAC_METADATA_BLOCK_TYPE_VORBIS_COMMENT) {
		// Parse Vorbis comments
		const drflac_vorbis_comment_iterator* comments = NULL;
		uint32_t commentCount = pMetadata->data.vorbis_comment.commentCount;
		const char* pComments = pMetadata->data.vorbis_comment.pComments;

		// Iterate through comments
		for (uint32_t i = 0; i < commentCount; i++) {
			uint32_t commentLength;
			if (pComments) {
				// Read comment length (little-endian 32-bit)
				commentLength = *(const uint32_t*)pComments;
				pComments += 4;

				// Create null-terminated copy
				char* comment = malloc(commentLength + 1);
				if (comment) {
					memcpy(comment, pComments, commentLength);
					comment[commentLength] = '\0';
					parse_vorbis_comment(comment);
					free(comment);
				}

				pComments += commentLength;
			}
		}
	}
}

AudioFormat Player_detectFormat(const char* filepath) {
	if (!filepath)
		return AUDIO_FORMAT_UNKNOWN;

	const char* ext = strrchr(filepath, '.');
	if (!ext)
		return AUDIO_FORMAT_UNKNOWN;
	ext++; // Skip the dot

	if (strcasecmp(ext, "mp3") == 0)
		return AUDIO_FORMAT_MP3;
	if (strcasecmp(ext, "wav") == 0)
		return AUDIO_FORMAT_WAV;
	if (strcasecmp(ext, "ogg") == 0)
		return AUDIO_FORMAT_OGG;
	if (strcasecmp(ext, "opus") == 0)
		return AUDIO_FORMAT_OPUS;
	if (strcasecmp(ext, "flac") == 0)
		return AUDIO_FORMAT_FLAC;
	if (strcasecmp(ext, "m4a") == 0)
		return AUDIO_FORMAT_M4A;
	if (strcasecmp(ext, "aac") == 0)
		return AUDIO_FORMAT_AAC;
	if (strcasecmp(ext, "mod") == 0 || strcasecmp(ext, "xm") == 0 ||
		strcasecmp(ext, "s3m") == 0 || strcasecmp(ext, "it") == 0) {
		return AUDIO_FORMAT_MOD;
	}

	return AUDIO_FORMAT_UNKNOWN;
}

// Reset audio device to default sample rate (for radio use)
void Player_resetSampleRate(void) {
	reconfigure_audio_device(get_target_sample_rate());
}

// Set audio device to specific sample rate
void Player_setSampleRate(int sample_rate) {
	if (sample_rate > 0) {
		reconfigure_audio_device(sample_rate);
	}
}

// Load file using streaming playback (decode on-the-fly)
static int load_streaming(const char* filepath) {
	// Open decoder
	if (stream_decoder_open(&player.stream_decoder, filepath) != 0) {
		return -1;
	}

	// Initialize circular buffer
	if (circular_buffer_init(&player.stream_buffer, STREAM_BUFFER_FRAMES) != 0) {
		stream_decoder_close(&player.stream_decoder);
		return -1;
	}

	// Initialize resampler for streaming
	int src_rate = player.stream_decoder.source_sample_rate;
	int dst_rate = get_target_sample_rate();

	if (src_rate != dst_rate) {
		int error;
		player.resampler = src_new(SRC_SINC_FASTEST, AUDIO_CHANNELS, &error);
		if (!player.resampler) {
			LOG_error("Stream: Failed to create resampler: %s\n", src_strerror(error));
			circular_buffer_free(&player.stream_buffer);
			stream_decoder_close(&player.stream_decoder);
			return -1;
		}
	}

	// Set track info
	player.track_info.sample_rate = dst_rate; // Output rate
	player.track_info.channels = AUDIO_CHANNELS;
	player.track_info.duration_ms = (int)((player.stream_decoder.total_frames * 1000) /
										  player.stream_decoder.source_sample_rate);

	// Configure audio device at target rate (no reconfiguration needed later!)
	reconfigure_audio_device(dst_rate);

	// Start decode thread
	player.stream_running = true;
	player.stream_seeking = false;
	player.stream_eof = false;
	pthread_create(&player.stream_thread, NULL, stream_thread_func, NULL);

	// Pre-buffer some audio before returning (~0.5 seconds)
	int prebuffer_timeout = 100; // 100 * 10ms = 1 second max
	while (circular_buffer_available(&player.stream_buffer) < STREAM_BUFFER_FRAMES / 6 &&
		   prebuffer_timeout > 0) {
		usleep(10000); // 10ms
		prebuffer_timeout--;
	}

	player.use_streaming = true;
	player.format = player.stream_decoder.format;

	return 0;
}

int Player_load(const char* filepath) {
	if (!filepath || !player.audio_initialized)
		return -1;

	// Stop any current playback
	Player_stop();

	int result = -1;

	pthread_mutex_lock(&player.mutex);

	// Store filename
	strncpy(player.current_file, filepath, sizeof(player.current_file) - 1);

	// Extract title from filename
	const char* filename = strrchr(filepath, '/');
	if (filename)
		filename++;
	else
		filename = filepath;
	strncpy(player.track_info.title, filename, sizeof(player.track_info.title) - 1);

	// Remove extension from title
	char* ext = strrchr(player.track_info.title, '.');
	if (ext)
		*ext = '\0';

	// Clear artist/album
	player.track_info.artist[0] = '\0';
	player.track_info.album[0] = '\0';

	pthread_mutex_unlock(&player.mutex);

	// Use streaming playback for supported formats
	AudioFormat format = Player_detectFormat(filepath);
	if (format == AUDIO_FORMAT_MP3 || format == AUDIO_FORMAT_WAV ||
		format == AUDIO_FORMAT_FLAC || format == AUDIO_FORMAT_OGG ||
		format == AUDIO_FORMAT_M4A || format == AUDIO_FORMAT_AAC ||
		format == AUDIO_FORMAT_OPUS) {
		result = load_streaming(filepath);

		// Parse metadata for MP3
		if (result == 0 && format == AUDIO_FORMAT_MP3) {
			parse_mp3_metadata(filepath);
		}
		// Parse metadata for M4A
		if (result == 0 && format == AUDIO_FORMAT_M4A) {
			parse_m4a_metadata();
		}
		// Parse metadata for Opus (uses Vorbis comment tags)
		if (result == 0 && format == AUDIO_FORMAT_OPUS) {
			OggOpusFile* of = (OggOpusFile*)player.stream_decoder.decoder;
			const OpusTags* tags = op_tags(of, -1);
			if (tags) {
				for (int i = 0; i < tags->comments; i++)
					parse_vorbis_comment(tags->user_comments[i]);
			}
		}

		// Album art fetch moved to module_player.c (after Player_play)
		// to avoid blocking playback start
	} else {
		LOG_error("Unsupported format for streaming: %s\n", filepath);
		return -1;
	}

	if (result == 0) {
		pthread_mutex_lock(&player.mutex);
		player.position_ms = 0;
		audio_position_samples = 0;
		player.state = PLAYER_STATE_STOPPED;
		pthread_mutex_unlock(&player.mutex);

		// Note: Waveform generation disabled for streaming mode
		// (would require reading entire file which defeats the purpose)
	}

	return result;
}

int Player_play(void) {
	// Check if we have audio loaded
	if (!player.use_streaming || !player.stream_decoder.decoder)
		return -1;

	pthread_mutex_lock(&player.mutex);
	player.state = PLAYER_STATE_PLAYING;
	pthread_mutex_unlock(&player.mutex);

	SDL_PauseAudioDevice(player.audio_device, 0);
	return 0;
}

void Player_pause(void) {
	pthread_mutex_lock(&player.mutex);
	if (player.state == PLAYER_STATE_PLAYING) {
		player.state = PLAYER_STATE_PAUSED;
		SDL_PauseAudioDevice(player.audio_device, 1);
	}
	pthread_mutex_unlock(&player.mutex);
}

void Player_stop(void) {
	// Stop streaming thread first (before locking mutex to avoid deadlock)
	if (player.use_streaming && player.stream_running) {
		player.stream_running = false;
		pthread_join(player.stream_thread, NULL);
	}

	pthread_mutex_lock(&player.mutex);

	SDL_PauseAudioDevice(player.audio_device, 1);

	player.state = PLAYER_STATE_STOPPED;
	player.position_ms = 0;
	audio_position_samples = 0;

	// Clean up streaming resources
	if (player.use_streaming) {
		stream_decoder_close(&player.stream_decoder);
		circular_buffer_free(&player.stream_buffer);
		if (player.resampler) {
			src_delete((SRC_STATE*)player.resampler);
			player.resampler = NULL;
		}
		// Free resampler leftover buffer
		if (player.resample_leftover) {
			free(player.resample_leftover);
			player.resample_leftover = NULL;
			player.resample_leftover_count = 0;
			player.resample_leftover_capacity = 0;
		}
		player.use_streaming = false;
	}

	memset(&player.track_info, 0, sizeof(TrackInfo));
	player.current_file[0] = '\0';

	// Clear waveform
	memset(&waveform, 0, sizeof(waveform));

	// Free album art
	if (player.album_art) {
		SDL_FreeSurface(player.album_art);
		player.album_art = NULL;
	}

	// Clear any internet-fetched album art
	album_art_clear();

	pthread_mutex_unlock(&player.mutex);
}

void Player_togglePause(void) {
	pthread_mutex_lock(&player.mutex);
	if (player.state == PLAYER_STATE_PLAYING) {
		player.state = PLAYER_STATE_PAUSED;
		SDL_PauseAudioDevice(player.audio_device, 1);
	} else if (player.state == PLAYER_STATE_PAUSED) {
		player.state = PLAYER_STATE_PLAYING;
		SDL_PauseAudioDevice(player.audio_device, 0);
	}
	pthread_mutex_unlock(&player.mutex);
}

void Player_seek(int position_ms) {
	pthread_mutex_lock(&player.mutex);
	if (position_ms < 0)
		position_ms = 0;
	if (position_ms > player.track_info.duration_ms) {
		position_ms = player.track_info.duration_ms;
	}

	if (player.use_streaming) {
		// Streaming mode: signal decode thread to seek
		// Calculate target frame in source sample rate
		int64_t target_frame = (int64_t)position_ms * player.stream_decoder.source_sample_rate / 1000;
		player.seek_target_frame = target_frame;
		player.stream_seeking = true;
	}

	player.position_ms = position_ms;
	audio_position_samples = (int64_t)position_ms * current_sample_rate / 1000;
	pthread_mutex_unlock(&player.mutex);
}

bool Player_resume(void) {
	return player.stream_seeking;
}

void Player_setVolume(float volume) {
	if (volume < 0.0f)
		volume = 0.0f;
	if (volume > 1.0f)
		volume = 1.0f;
	pthread_mutex_lock(&player.mutex);
	player.volume = volume;
	pthread_mutex_unlock(&player.mutex);
}

float Player_getVolume(void) {
	return player.volume;
}

PlayerState Player_getState(void) {
	return player.state;
}

int Player_getPosition(void) {
	return player.position_ms;
}

int Player_getDuration(void) {
	return player.track_info.duration_ms;
}

const TrackInfo* Player_getTrackInfo(void) {
	return &player.track_info;
}

const char* Player_getCurrentFile(void) {
	return player.current_file;
}

int Player_getVisBuffer(int16_t* buffer, int max_samples) {
	if (!buffer || max_samples <= 0)
		return 0;

	pthread_mutex_lock(&player.vis_mutex);
	int samples_to_copy = player.vis_buffer_pos;
	if (samples_to_copy > max_samples)
		samples_to_copy = max_samples;
	if (samples_to_copy > 0) {
		memcpy(buffer, player.vis_buffer, samples_to_copy * sizeof(int16_t));
	}
	pthread_mutex_unlock(&player.vis_mutex);

	return samples_to_copy;
}

const WaveformData* Player_getWaveform(void) {
	return &waveform;
}

SDL_Surface* Player_getAlbumArt(void) {
	// Return embedded album art if available
	if (player.album_art) {
		return player.album_art;
	}
	// Fallback to internet-fetched album art (from radio module)
	return album_art_get();
}

void Player_update(void) {
	// End-of-track detection is handled in the audio callback for streaming mode
	// This function is kept for any future polling needs
}

void Player_resumeAudio(void) {
	if (player.audio_device > 0) {
		SDL_PauseAudioDevice(player.audio_device, 0);
	}
}

void Player_pauseAudio(void) {
	if (player.audio_device > 0) {
		SDL_PauseAudioDevice(player.audio_device, 1);
	}
}

bool Player_isBluetoothActive(void) {
	return bluetooth_audio_active;
}

bool Player_isUSBDACActive(void) {
	return usbdac_audio_active;
}

// USB HID input monitoring
static int usb_hid_fd = -1;

// Find USB audio HID device by scanning /proc/bus/input/devices
static int find_audio_hid_device(char* event_path, size_t path_size, bool find_bluetooth) {
	FILE* f = fopen("/proc/bus/input/devices", "r");
	if (!f)
		return -1;

	char line[512];
	char name[256] = {0};
	char handlers[256] = {0};
	bool is_usb = false;
	bool is_bluetooth_avrcp = false;
	bool has_kbd = false;

	while (fgets(line, sizeof(line), f)) {
		if (strncmp(line, "N: Name=", 8) == 0) {
			// New device, reset state
			strncpy(name, line + 8, sizeof(name) - 1);
			handlers[0] = 0;
			is_usb = false;
			is_bluetooth_avrcp = false;
			has_kbd = false;
			// Check if Bluetooth AVRCP device
			if (strstr(name, "AVRCP")) {
				is_bluetooth_avrcp = true;
			}
		} else if (strncmp(line, "P: Phys=", 8) == 0) {
			// Check if USB device
			if (strstr(line, "usb-")) {
				is_usb = true;
			}
		} else if (strncmp(line, "H: Handlers=", 12) == 0) {
			strncpy(handlers, line + 12, sizeof(handlers) - 1);
			// Check if it has kbd handler (keyboard/keypad events)
			if (strstr(handlers, "kbd")) {
				has_kbd = true;
			}
		} else if (line[0] == '\n') {
			// End of device entry
			bool match = false;
			if (find_bluetooth && is_bluetooth_avrcp && has_kbd) {
				match = true;
			} else if (!find_bluetooth && is_usb && has_kbd) {
				match = true;
			}

			if (match && handlers[0]) {
				// Find event device number
				char* event_ptr = strstr(handlers, "event");
				if (event_ptr) {
					int event_num = -1;
					sscanf(event_ptr, "event%d", &event_num);
					if (event_num >= 0) {
						snprintf(event_path, path_size, "/dev/input/event%d", event_num);
						fclose(f);
						return 0;
					}
				}
			}
		}
	}

	fclose(f);
	return -1;
}

void Player_initUSBHID(void) {
	if (usb_hid_fd >= 0) {
		close(usb_hid_fd);
		usb_hid_fd = -1;
	}

	char event_path[64];

	// Try USB DAC HID first
	if (usbdac_audio_active) {
		if (find_audio_hid_device(event_path, sizeof(event_path), false) == 0) {
			usb_hid_fd = open(event_path, O_RDONLY | O_NONBLOCK);
			if (usb_hid_fd >= 0) {
				return;
			}
		}
	}

	// Try Bluetooth AVRCP
	if (bluetooth_audio_active) {
		if (find_audio_hid_device(event_path, sizeof(event_path), true) == 0) {
			usb_hid_fd = open(event_path, O_RDONLY | O_NONBLOCK);
			if (usb_hid_fd >= 0) {
				return;
			}
		}
	}
}

USBHIDEvent Player_pollUSBHID(void) {
	if (usb_hid_fd < 0) {
		return USB_HID_EVENT_NONE;
	}

	struct input_event_raw ev;
	while (read(usb_hid_fd, &ev, sizeof(ev)) == sizeof(ev)) {
		// Only handle key press events (value=1), ignore release (0) and repeat (2)
		if (ev.type == EV_KEY && ev.value == 1) {
			switch (ev.code) {
			case KEY_VOLUMEUP:
				return USB_HID_EVENT_VOLUME_UP;
			case KEY_VOLUMEDOWN:
				return USB_HID_EVENT_VOLUME_DOWN;
			case KEY_NEXTSONG:
				return USB_HID_EVENT_NEXT_TRACK;
			case KEY_PLAYPAUSE:
			case KEY_PLAYCD:
			case KEY_PAUSECD:
				return USB_HID_EVENT_PLAY_PAUSE;
			case KEY_PREVIOUSSONG:
				return USB_HID_EVENT_PREV_TRACK;
			}
		}
	}

	return USB_HID_EVENT_NONE;
}

void Player_quitUSBHID(void) {
	if (usb_hid_fd >= 0) {
		close(usb_hid_fd);
		usb_hid_fd = -1;
	}
}
