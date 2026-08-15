#ifndef CLIB_BACKEND_ALSA_DEVICE_H
#define CLIB_BACKEND_ALSA_DEVICE_H

#if defined(ENABLE_ALSA)

#include <alsa/asoundlib.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "Config/engine_config_types.h"

/**
 * @file alsa_device.h
 * @brief Provides shared ALSA device resources, mutexes, buffer/period sizing,
 * format conversion, and control element utilities.
 */

/**
 * @brief Global mutex for protecting ALSA API calls.
 *
 * Matches ALSA_MUTEX in upstream CamillaDSP (src/alsa_backend/device.rs:59).
 */
extern pthread_mutex_t g_alsa_mutex;

/**
 * @brief Checks whether the given ALSA PCM format is a native DSD format.
 *
 * @param format ALSA PCM format.
 * @return True if DSD format, false otherwise.
 */
bool alsa_is_dsd_format(snd_pcm_format_t format);

/**
 * @brief Returns the byte size of a single sample for a given ALSA PCM format.
 *
 * @param format ALSA PCM format.
 * @return Size in bytes per sample.
 */
size_t alsa_format_sample_size(snd_pcm_format_t format);

/**
 * @brief Converts an alsa_sample_format_t enum to the corresponding ALSA
 * snd_pcm_format_t.
 *
 * @param fmt The ALSA sample format enum.
 * @return The corresponding snd_pcm_format_t, or SND_PCM_FORMAT_UNKNOWN.
 */
snd_pcm_format_t alsa_sample_format_to_pcm_format(alsa_sample_format_t fmt);

/**
 * @brief Converts an ALSA snd_pcm_format_t to the corresponding
 * binary_sample_format_t.
 *
 * @param fmt The snd_pcm_format_t format.
 * @return The corresponding binary_sample_format_t.
 */
binary_sample_format_t alsa_pcm_format_to_binary_format(snd_pcm_format_t fmt);

/**
 * @brief Calculates and applies a sample format to an ALSA hardware parameters
 * container.
 *
 * If `has_format` is true, tries `requested_format`.
 * If `has_format` is false, tries preferred PCM formats in descending order:
 * S32_LE -> S24_3_LE -> S24_4_LE -> S16_LE -> F32_LE -> F64_LE
 * (src/alsa_backend/utils.rs:433-456).
 *
 * @param pcm Pointer to the ALSA PCM handle.
 * @param hwp Pointer to the ALSA hardware parameters container.
 * @param has_format True if a specific format was requested.
 * @param requested_format Requested format enum if has_format is true.
 * @param out_format Pointer to receive the applied snd_pcm_format_t.
 * @return 0 on success, or a negative ALSA error code on failure.
 */
int alsa_apply_format(snd_pcm_t* pcm, snd_pcm_hw_params_t* hwp, bool has_format,
                      alsa_sample_format_t requested_format,
                      snd_pcm_format_t* out_format);

/**
 * @brief Calculates and applies a power-of-two buffer size to an ALSA hardware
 * parameters container.
 *
 * Matches calculate_buffer_size and apply_buffer_size in upstream CamillaDSP
 * (src/alsa_backend/buffermanager.rs:34-83).
 *
 * @param pcm Pointer to the ALSA PCM handle.
 * @param hwp Pointer to the ALSA hardware parameters container.
 * @param chunksize The requested audio chunk size in frames.
 * @param resampling_ratio Ratio of samplerate / capture_samplerate (or 1.0 for
 * playback).
 * @param out_bufsize Pointer to receive the applied buffer size in frames.
 * @return 0 on success, or a negative ALSA error code on failure.
 */
int alsa_apply_buffer_size(snd_pcm_t* pcm, snd_pcm_hw_params_t* hwp,
                           size_t chunksize, double resampling_ratio,
                           snd_pcm_uframes_t* out_bufsize);

/**
 * @brief Calculates and applies a period size to an ALSA hardware parameters
 * container.
 *
 * Matches apply_period_size in upstream CamillaDSP
 * (src/alsa_backend/buffermanager.rs:85-106).
 *
 * @param pcm Pointer to the ALSA PCM handle.
 * @param hwp Pointer to the ALSA hardware parameters container.
 * @param bufsize The configured buffer size in frames.
 * @param out_period Pointer to receive the applied period size in frames.
 * @return 0 on success, or a negative ALSA error code on failure.
 */
int alsa_apply_period_size(snd_pcm_t* pcm, snd_pcm_hw_params_t* hwp,
                           snd_pcm_uframes_t bufsize,
                           snd_pcm_uframes_t* out_period);

/**
 * @brief Recovers an ALSA PCM handle from a suspended state
 * (SND_PCM_STATE_SUSPENDED / ESTRPIPE).
 *
 * Matches recover_suspended_pcm in upstream CamillaDSP
 * (src/alsa_backend/utils.rs:279-310).
 *
 * @param pcm Pointer to the ALSA PCM handle.
 * @param direction String label ("PB" for playback, "Capture" for capture) used
 * for logging.
 * @return 0 on success, or a negative ALSA error code on failure.
 */
int alsa_recover_suspended_pcm(snd_pcm_t* pcm, const char* direction);

/**
 * @brief Returns the descriptive string representation of an ALSA PCM state.
 *
 * Matches state_desc in upstream CamillaDSP
 * (src/alsa_backend/utils.rs:255-277).
 *
 * @param state ALSA PCM state code.
 * @return Constant string description.
 */
const char* alsa_state_desc(snd_pcm_state_t state);

/**
 * @brief Helper to look up an ALSA control element.
 *
 * Matches find_elem in upstream CamillaDSP (src/alsa_backend/utils.rs:758-780).
 *
 * @param hctl ALSA High-level Control handle.
 * @param iface ALSA element interface type.
 * @param device Device index (< 0 to ignore).
 * @param subdevice Subdevice index (< 0 to ignore).
 * @param name Element name.
 * @param out_numid Optional pointer to receive the element numid.
 * @return Pointer to snd_hctl_elem_t if found, NULL otherwise.
 */
snd_hctl_elem_t* alsa_find_elem(snd_hctl_t* hctl, snd_ctl_elem_iface_t iface,
                                int device, int subdevice, const char* name,
                                unsigned int* out_numid);

/**
 * @brief Reads an ALSA control element value as an integer.
 *
 * Matches elem_read_as_int in upstream CamillaDSP
 * (src/alsa_backend/utils.rs:473-478).
 *
 * @param elem Pointer to the ALSA control element.
 * @param out_val Pointer to receive integer value.
 * @return True on success, false on failure.
 */
bool alsa_elem_read_as_int(snd_hctl_elem_t* elem, long* out_val);

/**
 * @brief Reads an ALSA control element value as a boolean.
 *
 * Matches elem_read_as_bool in upstream CamillaDSP
 * (src/alsa_backend/utils.rs:480-485).
 *
 * @param elem Pointer to the ALSA control element.
 * @param out_val Pointer to receive boolean value.
 * @return True on success, false on failure.
 */
bool alsa_elem_read_as_bool(snd_hctl_elem_t* elem, bool* out_val);

/**
 * @brief Reads an ALSA volume control element value converted to dB.
 *
 * Matches elem_read_volume_in_db in upstream CamillaDSP
 * (src/alsa_backend/utils.rs:487-493).
 *
 * @param ctl ALSA Control handle.
 * @param elem Pointer to the ALSA control element.
 * @param out_db Pointer to receive dB value.
 * @return True on success, false on failure.
 */
bool alsa_elem_read_volume_in_db(snd_ctl_t* ctl, snd_hctl_elem_t* elem,
                                 double* out_db);

/**
 * @brief Writes an integer value to an ALSA control element.
 *
 * Matches elem_write_as_int in upstream CamillaDSP
 * (src/alsa_backend/utils.rs:506-511).
 *
 * @param elem Pointer to the ALSA control element.
 * @param value Integer value to write.
 */
void alsa_elem_write_as_int(snd_hctl_elem_t* elem, long value);

/**
 * @brief Writes a boolean value to an ALSA control element.
 *
 * Matches elem_write_as_bool in upstream CamillaDSP
 * (src/alsa_backend/utils.rs:513-518).
 *
 * @param elem Pointer to the ALSA control element.
 * @param value Boolean value to write.
 */
void alsa_elem_write_as_bool(snd_hctl_elem_t* elem, bool value);

/**
 * @brief Converts a dB value and writes it to an ALSA volume control element.
 *
 * Matches elem_write_volume_in_db in upstream CamillaDSP
 * (src/alsa_backend/utils.rs:495-504).
 *
 * @param ctl ALSA Control handle.
 * @param elem Pointer to the ALSA control element.
 * @param db_val Volume value in dB.
 */
void alsa_elem_write_volume_in_db(snd_ctl_t* ctl, snd_hctl_elem_t* elem,
                                  double db_val);

/**
 * @brief Opens an ALSA PCM device and configures all hardware parameters.
 *
 * Matches open_pcm / apply_hw_params in upstream CamillaDSP
 * (src/alsa_backend/device.rs:416-480).
 *
 * @param pcm Pointer to receive the opened snd_pcm_t handle.
 * @param device_name ALSA device name (e.g., "hw:0,0", "default").
 * @param stream SND_PCM_STREAM_PLAYBACK or SND_PCM_STREAM_CAPTURE.
 * @param channels Number of channels.
 * @param sample_rate Target sample rate in Hz.
 * @param has_format True if a specific sample format was requested.
 * @param requested_format Requested format if has_format is true.
 * @param chunk_size Chunk size in frames.
 * @param resampling_ratio Resampling ratio (pipeline rate / device rate,
 * or 1.0).
 * @param out_format Pointer to receive applied ALSA format.
 * @param out_bufsize Pointer to receive applied buffer size in frames.
 * @param out_period Pointer to receive applied period size in frames.
 * @param out_can_pause Pointer to receive whether device supports pausing.
 * @param out_error_msg Buffer to receive error message on failure (optional).
 * @param error_msg_len Size of out_error_msg buffer.
 * @return 0 on success, or negative ALSA error code on failure.
 */
int alsa_device_open_and_configure_hw(
    snd_pcm_t** pcm, const char* device_name, snd_pcm_stream_t stream,
    int channels, unsigned int sample_rate, bool has_format,
    alsa_sample_format_t requested_format, size_t chunk_size,
    double resampling_ratio, snd_pcm_format_t* out_format,
    snd_pcm_uframes_t* out_bufsize, snd_pcm_uframes_t* out_period,
    bool* out_can_pause, char* out_error_msg, size_t error_msg_len);

/**
 * @brief Configures ALSA software parameters on an opened PCM handle.
 *
 * Matches sw_params setup in upstream CamillaDSP
 * (src/alsa_backend/device.rs:483-491 & buffermanager.rs).
 *
 * @param pcm Pointer to the ALSA PCM handle.
 * @param avail_min Minimum available frames before waking poll/read/write.
 * @param start_threshold Buffer frames threshold to start playback/capture.
 * @return 0 on success, or negative ALSA error code on failure.
 */
int alsa_device_configure_sw(snd_pcm_t* pcm, snd_pcm_uframes_t avail_min,
                             snd_pcm_uframes_t start_threshold);

/**
 * @brief Extracts the control card name (e.g. "hw:0") from an opened PCM
 * device.
 *
 * @param pcm Pointer to the ALSA PCM handle.
 * @param out_ctl_name Buffer to receive card name string.
 * @param max_len Size of out_ctl_name buffer.
 * @param out_dev_idx Pointer to receive PCM device index (optional).
 * @param out_subdev_idx Pointer to receive PCM subdevice index (optional).
 * @return True on success, false on failure.
 */
bool alsa_device_get_card_ctl_name(snd_pcm_t* pcm, char* out_ctl_name,
                                   size_t max_len, int* out_dev_idx,
                                   int* out_subdev_idx);

/**
 * @brief Primes ALSA device delay with silence frames to reach the target
 * level.
 *
 * Matches prime_playback_delay in upstream CamillaDSP
 * (src/alsa_backend/threaded_device.rs:122-207).
 *
 * @param pcm Pointer to the ALSA PCM handle.
 * @param target_level Target buffer level in frames.
 * @param bufsize Total device buffer size in frames.
 * @param sample_rate Sample rate in Hz.
 * @param blockalign Frame size in bytes (channels * sample_size).
 * @param queued_frames Frames already queued.
 * @param silence_buf Pre-allocated buffer filled with silence/zeros.
 * @param silence_buf_size Size of silence_buf in bytes.
 * @return True on success, false on failure.
 */
bool alsa_device_prime_delay(snd_pcm_t* pcm, size_t target_level,
                             snd_pcm_uframes_t bufsize, int sample_rate,
                             size_t blockalign, size_t queued_frames,
                             const void* silence_buf, size_t silence_buf_size);

/**
 * @brief Calculates SPSC ring buffer frame capacity for threaded ALSA playback.
 */
static inline size_t alsa_playback_ring_capacity_frames(size_t target_level,
                                                        size_t chunk_size) {
  size_t base = target_level > 3 * chunk_size ? target_level : 3 * chunk_size;
  return base + 4 * chunk_size;
}

/**
 * @brief Calculates SPSC ring buffer frame capacity for threaded ALSA capture.
 */
static inline size_t alsa_capture_ring_capacity_frames(
    size_t chunk_size, snd_pcm_uframes_t period) {
  size_t base =
      3 * chunk_size > (size_t)period ? 3 * chunk_size : (size_t)period;
  return base + 4 * chunk_size;
}

#endif  // ENABLE_ALSA

#endif  // CLIB_BACKEND_ALSA_DEVICE_H
