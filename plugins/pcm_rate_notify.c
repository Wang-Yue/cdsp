#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <alsa/asoundlib.h>
#include <alsa/pcm_external.h>
#include <alsa/pcm_ioplug.h>

#ifndef PIC
#define PIC
#endif

typedef struct {
    snd_pcm_ioplug_t io;
    snd_pcm_t *slave;
    char *slave_name;
    char *ctl_card;
    int ctl_device;
    int ctl_subdevice;
    int retry_count;
    int retry_delay_us;
    snd_pcm_uframes_t hw_ptr;
} rate_notify_ioplug_t;

static int notify_capture_rate(const char *card, int device, int subdevice, unsigned int rate) {
    char ctl_name[64];
    if (card && (strncmp(card, "hw:", 3) == 0 || strncmp(card, "sysdefault:", 11) == 0)) {
        snprintf(ctl_name, sizeof(ctl_name), "%s", card);
    } else {
        snprintf(ctl_name, sizeof(ctl_name), "hw:%s", card ? card : "Loopback");
    }

    snd_ctl_t *ctl = NULL;
    int err = snd_ctl_open(&ctl, ctl_name, 0);
    if (err < 0) return err;

    snd_ctl_elem_id_t *id;
    snd_ctl_elem_id_alloca(&id);
    snd_ctl_elem_id_set_interface(id, SND_CTL_ELEM_IFACE_PCM);
    snd_ctl_elem_id_set_device(id, device >= 0 ? (unsigned int)device : 1);
    snd_ctl_elem_id_set_subdevice(id, subdevice >= 0 ? (unsigned int)subdevice : 0);
    snd_ctl_elem_id_set_name(id, "Capture Rate");

    snd_ctl_elem_value_t *val;
    snd_ctl_elem_value_alloca(&val);
    snd_ctl_elem_value_set_id(val, id);

    snd_ctl_elem_info_t *info;
    snd_ctl_elem_info_alloca(&info);
    snd_ctl_elem_info_set_id(info, id);
    if (snd_ctl_elem_info(ctl, info) < 0) {
        err = snd_ctl_elem_add_integer(ctl, id, 1, 0, 1536000, 1);
        if (err < 0) {
            snd_ctl_close(ctl);
            return err;
        }
    } else if (snd_ctl_elem_info_get_max(info) < (long)rate || snd_ctl_elem_info_get_max(info) < 1536000) {
        snd_ctl_elem_remove(ctl, id);
        err = snd_ctl_elem_add_integer(ctl, id, 1, 0, 1536000, 1);
        if (err < 0) {
            snd_ctl_close(ctl);
            return err;
        }
    }

    snd_ctl_elem_value_set_integer(val, 0, (long)rate);
    err = snd_ctl_elem_write(ctl, val);
    snd_ctl_close(ctl);
    return err;
}

static int rn_start(snd_pcm_ioplug_t *io) {
    rate_notify_ioplug_t *rec = (rate_notify_ioplug_t *)io;
    if (rec->slave) {
        snd_pcm_state_t state = snd_pcm_state(rec->slave);
        if (state == SND_PCM_STATE_PREPARED) {
            return snd_pcm_start(rec->slave);
        }
    }
    return 0;
}

static int rn_stop(snd_pcm_ioplug_t *io) {
    rate_notify_ioplug_t *rec = (rate_notify_ioplug_t *)io;
    if (rec->slave) {
        snd_pcm_state_t state = snd_pcm_state(rec->slave);
        if (state == SND_PCM_STATE_RUNNING || state == SND_PCM_STATE_DRAINING || state == SND_PCM_STATE_PAUSED) {
            return snd_pcm_drop(rec->slave);
        }
    }
    return 0;
}

static int rn_prepare(snd_pcm_ioplug_t *io) {
    rate_notify_ioplug_t *rec = (rate_notify_ioplug_t *)io;
    rec->hw_ptr = 0;
    if (rec->slave) {
        snd_pcm_drop(rec->slave);
        return snd_pcm_prepare(rec->slave);
    }
    return 0;
}

static snd_pcm_sframes_t rn_pointer(snd_pcm_ioplug_t *io) {
    rate_notify_ioplug_t *rec = (rate_notify_ioplug_t *)io;
    if (!rec->slave) return -EBADFD;
    if (io->buffer_size == 0) return 0;

    snd_pcm_state_t state = snd_pcm_state(rec->slave);
    if (state == SND_PCM_STATE_XRUN) return -EPIPE;
    if (state != SND_PCM_STATE_RUNNING && state != SND_PCM_STATE_DRAINING) return 0;

    snd_pcm_sframes_t delay = 0;
    int err = snd_pcm_delay(rec->slave, &delay);
    if (err < 0) return err;

    snd_pcm_sframes_t hw = (snd_pcm_sframes_t)io->appl_ptr - delay;
    if (hw < 0) hw = 0;
    return hw % io->buffer_size;
}

static int rn_drain(snd_pcm_ioplug_t *io) {
    rate_notify_ioplug_t *rec = (rate_notify_ioplug_t *)io;
    if (rec->slave) return snd_pcm_drain(rec->slave);
    return 0;
}

static int rn_pause(snd_pcm_ioplug_t *io, int enable) {
    rate_notify_ioplug_t *rec = (rate_notify_ioplug_t *)io;
    if (rec->slave) return snd_pcm_pause(rec->slave, enable);
    return 0;
}

static snd_pcm_sframes_t rn_transfer(snd_pcm_ioplug_t *io,
                                     const snd_pcm_channel_area_t *areas,
                                     snd_pcm_uframes_t offset,
                                     snd_pcm_uframes_t size) {
    rate_notify_ioplug_t *rec = (rate_notify_ioplug_t *)io;
    if (!rec->slave) return -EBADFD;

    char *buf = (char *)areas[0].addr + (offset * areas[0].step / 8);
    snd_pcm_sframes_t written = snd_pcm_writei(rec->slave, buf, size);
    if (written > 0) {
        rec->hw_ptr += written;
    }
    return written;
}

static int rn_poll_descriptors_count(snd_pcm_ioplug_t *io) {
    rate_notify_ioplug_t *rec = (rate_notify_ioplug_t *)io;
    if (rec->slave) return snd_pcm_poll_descriptors_count(rec->slave);
    return 1;
}

static int rn_poll_descriptors(snd_pcm_ioplug_t *io, struct pollfd *pfd, unsigned int space) {
    rate_notify_ioplug_t *rec = (rate_notify_ioplug_t *)io;
    if (rec->slave) return snd_pcm_poll_descriptors(rec->slave, pfd, space);
    return 0;
}

static int rn_poll_revents(snd_pcm_ioplug_t *io, struct pollfd *pfd, unsigned int nfds, unsigned short *revents) {
    rate_notify_ioplug_t *rec = (rate_notify_ioplug_t *)io;
    if (rec->slave) return snd_pcm_poll_descriptors_revents(rec->slave, pfd, nfds, revents);
    return 0;
}

static int rn_close(snd_pcm_ioplug_t *io) {
    rate_notify_ioplug_t *rec = (rate_notify_ioplug_t *)io;
    if (rec->slave) {
        snd_pcm_close(rec->slave);
        rec->slave = NULL;
    }
    if (rec->slave_name) free(rec->slave_name);
    if (rec->ctl_card) free(rec->ctl_card);
    free(rec);
    return 0;
}

static int set_slave_hw_params(snd_pcm_t *slave, snd_pcm_hw_params_t *params) {
    snd_pcm_hw_params_t *sp;
    snd_pcm_hw_params_alloca(&sp);
    int err = snd_pcm_hw_params_any(slave, sp);
    if (err < 0) return err;

    snd_pcm_format_t format = SND_PCM_FORMAT_UNKNOWN;
    unsigned int channels = 0, rate = 0;
    snd_pcm_uframes_t buffer_size = 0, period_size = 0;

    snd_pcm_hw_params_get_format(params, &format);
    snd_pcm_hw_params_get_channels(params, &channels);
    snd_pcm_hw_params_get_rate(params, &rate, 0);

    err = snd_pcm_hw_params_set_access(slave, sp, SND_PCM_ACCESS_RW_INTERLEAVED);
    if (err < 0) return err;
    if (format != SND_PCM_FORMAT_UNKNOWN) {
        err = snd_pcm_hw_params_set_format(slave, sp, format);
        if (err < 0) return err;
    }
    if (channels > 0) {
        err = snd_pcm_hw_params_set_channels(slave, sp, channels);
        if (err < 0) return err;
    }
    if (rate > 0) {
        err = snd_pcm_hw_params_set_rate(slave, sp, rate, 0);
        if (err < 0) return err;
    }
    if (snd_pcm_hw_params_get_buffer_size(params, &buffer_size) >= 0 && buffer_size > 0) {
        snd_pcm_hw_params_set_buffer_size_near(slave, sp, &buffer_size);
    }
    if (snd_pcm_hw_params_get_period_size(params, &period_size, 0) >= 0 && period_size > 0) {
        snd_pcm_hw_params_set_period_size_near(slave, sp, &period_size, 0);
    }

    return snd_pcm_hw_params(slave, sp);
}

static bool is_capture_active_at_rate(const char *card, long device, unsigned int target_rate) {
    if (!card) return true;
    char ctl_name[64];
    snprintf(ctl_name, sizeof(ctl_name), "hw:%s", card);
    snd_ctl_t *ctl = NULL;
    if (snd_ctl_open(&ctl, ctl_name, 0) < 0) return true;

    snd_ctl_elem_id_t *id_active, *id_rate;
    snd_ctl_elem_id_alloca(&id_active);
    snd_ctl_elem_id_alloca(&id_rate);

    snd_ctl_elem_id_set_interface(id_active, SND_CTL_ELEM_IFACE_PCM);
    snd_ctl_elem_id_set_device(id_active, (unsigned int)device);
    snd_ctl_elem_id_set_name(id_active, "PCM Slave Active");

    snd_ctl_elem_id_set_interface(id_rate, SND_CTL_ELEM_IFACE_PCM);
    snd_ctl_elem_id_set_device(id_rate, (unsigned int)device);
    snd_ctl_elem_id_set_name(id_rate, "PCM Slave Rate");

    snd_ctl_elem_value_t *val_active, *val_rate;
    snd_ctl_elem_value_alloca(&val_active);
    snd_ctl_elem_value_alloca(&val_rate);
    snd_ctl_elem_value_set_id(val_active, id_active);
    snd_ctl_elem_value_set_id(val_rate, id_rate);

    bool active = false;
    if (snd_ctl_elem_read(ctl, val_active) == 0) {
        int is_act = snd_ctl_elem_value_get_boolean(val_active, 0);
        if (is_act) {
            if (target_rate > 0 && snd_ctl_elem_read(ctl, val_rate) == 0) {
                long r = snd_ctl_elem_value_get_integer(val_rate, 0);
                if ((unsigned int)r == target_rate) {
                    active = true;
                }
            } else {
                active = true;
            }
        }
    }

    snd_ctl_close(ctl);
    return active;
}

static int rn_hw_params(snd_pcm_ioplug_t *io, snd_pcm_hw_params_t *params) {
    rate_notify_ioplug_t *rec = (rate_notify_ioplug_t *)io;
    unsigned int rate = 0;

    snd_pcm_hw_params_get_rate(params, &rate, 0);
    if (rate == 0) rate = io->rate;

    // 1. Notify CamillaDSP via "Capture Rate" control on Loopback
    if (rate > 0) {
        notify_capture_rate(rec->ctl_card, rec->ctl_device, rec->ctl_subdevice, rate);
    }

    // 2. If slave is already open, close it to let new params apply cleanly
    if (rec->slave) {
        snd_pcm_close(rec->slave);
        rec->slave = NULL;
    }

    // 3. Re-open slave PCM with retries to give cdsp/Monitor-Qt time to restart at new rate.
    // Wait until the capture endpoint (hw:Loopback,1,0) is actively capturing at the new rate.
    int err = -EBUSY;
    for (int i = 0; i < rec->retry_count; i++) {
        bool peer_ready = is_capture_active_at_rate(rec->ctl_card, rec->ctl_device, rate);
        if (peer_ready) {
            err = snd_pcm_open(&rec->slave, rec->slave_name, io->stream, io->nonblock);
            if (err == 0 && rec->slave) {
                err = set_slave_hw_params(rec->slave, params);
                if (err == 0) break;
                snd_pcm_close(rec->slave);
                rec->slave = NULL;
            }
        }
        usleep((useconds_t)rec->retry_delay_us);
    }

    // Fallback: If peer took longer than retry window, open slave anyway so playback doesn't hard-fail
    if (!rec->slave) {
        err = snd_pcm_open(&rec->slave, rec->slave_name, io->stream, io->nonblock);
        if (err == 0 && rec->slave) {
            err = set_slave_hw_params(rec->slave, params);
        }
    }

    return err;
}

static const snd_pcm_ioplug_callback_t rn_callback = {
    .start = rn_start,
    .stop = rn_stop,
    .pointer = rn_pointer,
    .transfer = rn_transfer,
    .close = rn_close,
    .hw_params = rn_hw_params,
    .prepare = rn_prepare,
    .drain = rn_drain,
    .pause = rn_pause,
    .poll_descriptors_count = rn_poll_descriptors_count,
    .poll_descriptors = rn_poll_descriptors,
    .poll_revents = rn_poll_revents,
};

SND_PCM_PLUGIN_DEFINE_FUNC(rate_notify) {
    snd_config_iterator_t i, next;
    const char *slave_name = "hw:Loopback,0,0";
    const char *ctl_card = "Loopback";
    long ctl_device = 1;
    long ctl_subdevice = 0;
    long retry_count = 50;
    long retry_delay_us = 15000; // 15ms * 50 = 750ms total wait window
    int err;

    snd_config_for_each(i, next, conf) {
        snd_config_t *n = snd_config_iterator_entry(i);
        const char *id;
        if (snd_config_get_id(n, &id) < 0) continue;
        if (strcmp(id, "comment") == 0 || strcmp(id, "type") == 0 || strcmp(id, "hint") == 0)
            continue;
        if (strcmp(id, "slave") == 0) {
            if (snd_config_get_type(n) == SND_CONFIG_TYPE_STRING) {
                snd_config_get_string(n, &slave_name);
            } else if (snd_config_get_type(n) == SND_CONFIG_TYPE_COMPOUND) {
                snd_config_t *pcm_node = NULL;
                if (snd_config_search(n, "pcm", &pcm_node) >= 0) {
                    snd_config_get_string(pcm_node, &slave_name);
                }
            }
            continue;
        }
        if (strcmp(id, "ctl_card") == 0) {
            snd_config_get_string(n, &ctl_card);
            continue;
        }
        if (strcmp(id, "ctl_device") == 0) {
            snd_config_get_integer(n, &ctl_device);
            continue;
        }
        if (strcmp(id, "ctl_subdevice") == 0) {
            snd_config_get_integer(n, &ctl_subdevice);
            continue;
        }
        if (strcmp(id, "retry_count") == 0) {
            snd_config_get_integer(n, &retry_count);
            continue;
        }
        if (strcmp(id, "retry_delay_us") == 0) {
            snd_config_get_integer(n, &retry_delay_us);
            continue;
        }
    }

    rate_notify_ioplug_t *rec = (rate_notify_ioplug_t *)calloc(1, sizeof(*rec));
    if (!rec) return -ENOMEM;

    rec->io.version = SND_PCM_IOPLUG_VERSION;
    rec->io.name = "Rate Notify I/O Plugin";
    rec->io.callback = &rn_callback;
    rec->io.private_data = rec;
    rec->slave_name = strdup(slave_name ? slave_name : "hw:Loopback,0,0");
    rec->ctl_card = strdup(ctl_card ? ctl_card : "Loopback");
    rec->ctl_device = (int)ctl_device;
    rec->ctl_subdevice = (int)ctl_subdevice;
    rec->retry_count = (int)retry_count;
    rec->retry_delay_us = (int)retry_delay_us;

    err = snd_pcm_ioplug_create(&rec->io, name, stream, mode);
    if (err < 0) {
        free(rec->slave_name);
        free(rec->ctl_card);
        free(rec);
        return err;
    }

    // 1. Access modes: interleaved and mmap
    static const unsigned int accesses[] = {
        SND_PCM_ACCESS_RW_INTERLEAVED,
        SND_PCM_ACCESS_MMAP_INTERLEAVED
    };
    snd_pcm_ioplug_set_param_list(&rec->io, SND_PCM_IOPLUG_HW_ACCESS,
                                  sizeof(accesses)/sizeof(accesses[0]), accesses);

    // 2. Sample Rates: continuous range 8 kHz to 768 kHz (matching snd-aloop & cdsp)
    snd_pcm_ioplug_set_param_minmax(&rec->io, SND_PCM_IOPLUG_HW_RATE, 8000, 768000);

    // 3. Formats: complete list of PCM, Float, and DSD formats supported by ALSA & cdsp
    static const unsigned int formats[] = {
        SND_PCM_FORMAT_S8,
        SND_PCM_FORMAT_U8,
        SND_PCM_FORMAT_S16_LE,
        SND_PCM_FORMAT_S16_BE,
        SND_PCM_FORMAT_S24_LE,
        SND_PCM_FORMAT_S24_BE,
        SND_PCM_FORMAT_S24_3LE,
        SND_PCM_FORMAT_S24_3BE,
        SND_PCM_FORMAT_S32_LE,
        SND_PCM_FORMAT_S32_BE,
        SND_PCM_FORMAT_FLOAT_LE,
        SND_PCM_FORMAT_FLOAT_BE,
        SND_PCM_FORMAT_FLOAT64_LE,
        SND_PCM_FORMAT_FLOAT64_BE,
        SND_PCM_FORMAT_DSD_U8,
        SND_PCM_FORMAT_DSD_U16_LE,
        SND_PCM_FORMAT_DSD_U16_BE,
        SND_PCM_FORMAT_DSD_U32_LE,
        SND_PCM_FORMAT_DSD_U32_BE
    };
    snd_pcm_ioplug_set_param_list(&rec->io, SND_PCM_IOPLUG_HW_FORMAT,
                                  sizeof(formats)/sizeof(formats[0]), formats);

    // 4. Channels: 1 to 32 channels (matching snd-aloop & cdsp)
    snd_pcm_ioplug_set_param_minmax(&rec->io, SND_PCM_IOPLUG_HW_CHANNELS, 1, 32);
    snd_pcm_ioplug_set_param_minmax(&rec->io, SND_PCM_IOPLUG_HW_BUFFER_BYTES, 1024, 2 * 1024 * 1024);
    snd_pcm_ioplug_set_param_minmax(&rec->io, SND_PCM_IOPLUG_HW_PERIOD_BYTES, 256, 512 * 1024);
    snd_pcm_ioplug_set_param_minmax(&rec->io, SND_PCM_IOPLUG_HW_PERIODS, 2, 1024);

    *pcmp = rec->io.pcm;
    return 0;
}

SND_PCM_PLUGIN_SYMBOL(rate_notify);
