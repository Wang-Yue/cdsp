#ifndef CDSP_PUBLIC_FADER_H
#define CDSP_PUBLIC_FADER_H

#include <stdbool.h>
#include <stdint.h>

#include "cdsp_export.h"
#include "cdsp_pub_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Get the volume of the main fader (index 0).
 * @param engine Pointer to the engine.
 * @return Volume in dB.
 */
CDSP_API float cdsp_get_volume(const dsp_engine_t *engine);

/**
 * @brief Set the volume of the main fader (index 0).
 * @param engine Pointer to the engine.
 * @param db Volume in dB (clamped internally, typically -150 to +50 dB).
 * @param instant If true, bypasses the volume ramp and applies immediately.
 */
CDSP_API void cdsp_set_volume(dsp_engine_t *engine, float db, bool instant);

/**
 * @brief Get the mute state of the main fader (index 0).
 * @param engine Pointer to the engine.
 * @return true if muted, false if unmuted.
 */
CDSP_API bool cdsp_get_mute(const dsp_engine_t *engine);

/**
 * @brief Set the mute state of the main fader (index 0).
 * @param engine Pointer to the engine.
 * @param mute true to mute, false to unmute.
 */
CDSP_API void cdsp_set_mute(dsp_engine_t *engine, bool mute);

/**
 * @brief Get the volume of a specific fader.
 * @param engine Pointer to the engine.
 * @param fader Fader identifier (CDSP_FADER_MAIN, CDSP_FADER_AUX1-4).
 * @return Volume in dB.
 */
CDSP_API float cdsp_get_fader_volume(const dsp_engine_t *engine,
                                     cdsp_fader_t fader);

/**
 * @brief Set the volume of a specific fader.
 * @param engine Pointer to the engine.
 * @param fader Fader identifier.
 * @param db Volume in dB.
 * @param instant If true, bypasses the volume ramp and applies immediately.
 */
CDSP_API void cdsp_set_fader_volume(dsp_engine_t *engine, cdsp_fader_t fader,
                                    float db, bool instant);

/**
 * @brief Adjust the volume of a specific fader by a delta in dB.
 * @param engine Pointer to the engine.
 * @param fader Fader identifier.
 * @param delta Volume change in dB.
 * @return New volume in dB.
 */
CDSP_API float cdsp_adjust_fader_volume(dsp_engine_t *engine, cdsp_fader_t fader,
                                        float delta);

/**
 * @brief Adjust the volume of a specific fader by a delta in dB, clamped to [min_db, max_db].
 * @param engine Pointer to the engine.
 * @param fader Fader identifier.
 * @param delta Volume change in dB.
 * @param min_db Minimum allowable volume in dB.
 * @param max_db Maximum allowable volume in dB.
 * @return New volume in dB.
 */
CDSP_API float cdsp_adjust_fader_volume_clamped(dsp_engine_t *engine,
                                                cdsp_fader_t fader, float delta,
                                                float min_db, float max_db);

/**
 * @brief Adjust the volume of the main fader by a delta in dB.
 * @param engine Pointer to the engine.
 * @param delta Volume change in dB.
 * @return New volume in dB.
 */
CDSP_API float cdsp_adjust_volume(dsp_engine_t *engine, float delta);

/**
 * @brief Get the mute state of a specific fader (WebSocket: GetFaderMute).
 * @param engine Pointer to the engine.
 * @param fader Fader identifier.
 * @return true if muted, false otherwise.
 */
CDSP_API bool cdsp_get_fader_mute(const dsp_engine_t *engine,
                                  cdsp_fader_t fader);

/**
 * @brief Set the mute state of a specific fader.
 * @param engine Pointer to the engine.
 * @param fader Fader identifier.
 * @param mute true to mute, false to unmute.
 */
CDSP_API void cdsp_set_fader_mute(dsp_engine_t *engine, cdsp_fader_t fader,
                                  bool mute);

#ifdef __cplusplus
}
#endif

#endif // CDSP_PUBLIC_FADER_H
