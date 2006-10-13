/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 * $Id$
 *
 * Copyright (C) 2005 Miika Pekkarinen
 *
 * All files in this archive are subject to the GNU General Public License.
 * See the file COPYING in the source tree root for full license agreement.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 ****************************************************************************/

/* TODO: Check for a possibly broken codepath on a rapid skip, stop event */
/* TODO: same in reverse ^^ */
/* TODO: Also play, stop ^^ */
/* TODO: Can use the track changed callback to detect end of track and seek
 * in the previous track until this happens */
/* TODO: Pause should be handled in here, rather than PCMBUF so that voice can
 * play whilst audio is paused */
/* Design: we have prev_ti already, have a conditional for what type of seek
 * to do on a seek request, if it is a previous track seek, skip previous,
 * and in the request_next_track callback set the offset up the same way that
 * starting from an offset works. */
/* This is also necesary to prevent the problem with buffer overwriting on
 * automatic track changes */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

#include "system.h"
#include "thread.h"
#include "file.h"
#include "lcd.h"
#include "font.h"
#include "button.h"
#include "kernel.h"
#include "tree.h"
#include "debug.h"
#include "sprintf.h"
#include "settings.h"
#include "codecs.h"
#include "audio.h"
#include "logf.h"
#include "mp3_playback.h"
#include "usb.h"
#include "status.h"
#include "main_menu.h"
#include "ata.h"
#include "screens.h"
#include "playlist.h"
#include "playback.h"
#include "pcmbuf.h"
#include "pcm_playback.h"
#include "pcm_record.h"
#include "buffer.h"
#include "dsp.h"
#include "abrepeat.h"
#include "tagcache.h"
#ifdef HAVE_LCD_BITMAP
#include "icons.h"
#include "peakmeter.h"
#include "action.h"
#endif
#include "lang.h"
#include "bookmark.h"
#include "misc.h"
#include "sound.h"
#include "metadata.h"
#include "splash.h"
#include "talk.h"

#ifdef HAVE_RECORDING
#include "recording.h"
#endif

#define PLAYBACK_VOICE


/* default point to start buffer refill */
#define AUDIO_DEFAULT_WATERMARK      (1024*512)
/* amount of data to read in one read() call */
#define AUDIO_DEFAULT_FILECHUNK      (1024*32)
/* point at which the file buffer will fight for CPU time */
#define AUDIO_FILEBUF_CRITICAL       (1024*128)
/* amount of guess-space to allow for codecs that must hunt and peck
 * for their correct seeek target, 32k seems a good size */
#define AUDIO_REBUFFER_GUESS_SIZE    (1024*32)

/* macros to enable logf for queues */
#ifdef SIMULATOR
#define PLAYBACK_LOGQUEUES /* Define this for logf output of all queuing */
#endif

#ifdef PLAYBACK_LOGQUEUES
#define LOGFQUEUE(s) logf("%s", s)
#else
#define LOGFQUEUE(s)
#endif

enum {
    Q_AUDIO_PLAY = 1,
    Q_AUDIO_STOP,
    Q_AUDIO_PAUSE,
    Q_AUDIO_SKIP,
    Q_AUDIO_PRE_FF_REWIND,
    Q_AUDIO_FF_REWIND,
    Q_AUDIO_REBUFFER_SEEK,
    Q_AUDIO_CHECK_NEW_TRACK,
    Q_AUDIO_FLUSH,
    Q_AUDIO_TRACK_CHANGED,
    Q_AUDIO_DIR_SKIP,
    Q_AUDIO_NEW_PLAYLIST,
    Q_AUDIO_POSTINIT,
    Q_AUDIO_FILL_BUFFER,

    Q_CODEC_REQUEST_PENDING,
    Q_CODEC_REQUEST_COMPLETE,
    Q_CODEC_REQUEST_FAILED,

    Q_VOICE_PLAY,
    Q_VOICE_STOP,

    Q_CODEC_LOAD,
    Q_CODEC_LOAD_DISK,

#if defined(HAVE_RECORDING) && !defined(SIMULATOR)
    Q_ENCODER_LOAD_DISK,
    Q_ENCODER_RECORD,
#endif
};

/* As defined in plugins/lib/xxx2wav.h */
#if MEM > 1
#define MALLOC_BUFSIZE (512*1024)
#define GUARD_BUFSIZE  (32*1024)
#else
#define MALLOC_BUFSIZE (100*1024)
#define GUARD_BUFSIZE  (8*1024)
#endif

/* As defined in plugin.lds */
#if CONFIG_CPU == PP5020 || CONFIG_CPU == PP5002
#define CODEC_IRAM_ORIGIN   0x4000c000
#elif defined(IAUDIO_X5)
#define CODEC_IRAM_ORIGIN   0x10014000
#else
#define CODEC_IRAM_ORIGIN   0x1000c000
#endif
#define CODEC_IRAM_SIZE     0xc000

#ifdef PLAYBACK_VOICE
#ifdef SIMULATOR
static unsigned char sim_iram[CODEC_IRAM_SIZE];
#undef CODEC_IRAM_ORIGIN
#define CODEC_IRAM_ORIGIN sim_iram
#endif
#endif

#ifndef SIMULATOR
extern bool audio_is_initialized;
#else
static bool audio_is_initialized = false;
#endif



static struct mutex mutex_codecthread;
static struct event_queue codec_callback_queue;

static volatile bool audio_codec_loaded;
static volatile bool playing;
static volatile bool paused;

/* Is file buffer currently being refilled? */
static volatile bool filling IDATA_ATTR;

volatile int current_codec IDATA_ATTR;
extern unsigned char codecbuf[];

/* Ring buffer where tracks and codecs are loaded. */
static char *filebuf;

/* Total size of the ring buffer. */
size_t filebuflen;

/* Ring buffer read and write indexes. */
static volatile size_t buf_ridx IDATA_ATTR;
static volatile size_t buf_widx IDATA_ATTR;

/* Ring buffer arithmetic */
#define RINGBUF_ADD(p,v) ((p+v)<filebuflen ? p+v : p+v-filebuflen)
#define RINGBUF_SUB(p,v) ((p>=v) ? p-v : p+filebuflen-v)

/* Bytes available in the buffer. */
#define FILEBUFUSED RINGBUF_SUB(buf_widx, buf_ridx)

/* Codec swapping pointers */
static unsigned char *iram_buf[2];
static unsigned char *dram_buf[2];

/* Step count to the next unbuffered track. */
static int last_peek_offset;

/* Track information (count in file buffer, read/write indexes for
   track ring structure. */
static int track_ridx;
static int track_widx;
static bool track_changed;                      /* Audio and codec threads */

/* Partially loaded song's file handle to continue buffering later. */
static int current_fd;

/* Information about how many bytes left on the buffer re-fill run. */
static size_t fill_bytesleft;

/* Track info structure about songs in the file buffer. */
static struct track_info tracks[MAX_TRACK];     /* Audio thread */

/* Pointer to track info structure about current song playing. */
#define CUR_TI (&tracks[track_ridx])

/* Pointer to track info structure about previous played song. */
static struct track_info *prev_ti;              /* Audio and codec threads */

/* Have we reached end of the current playlist. */
static bool playlist_end = false;               /* Audio thread */

/* Was the skip being executed manual or automatic? */
static bool automatic_skip = false;             /* Audio and codec threads */
static bool dir_skip = false;                   /* Audio thread */
static bool new_playlist = false;               /* Audio thread */
static int wps_offset = 0;

/* Callback function to call when current track has really changed. */
void (*track_changed_callback)(struct mp3entry *id3);
void (*track_buffer_callback)(struct mp3entry *id3, bool last_track);
void (*track_unbuffer_callback)(struct mp3entry *id3, bool last_track);

/* Configuration */
static size_t conf_watermark;
static size_t conf_filechunk;
static size_t conf_preseek;
static size_t buffer_margin;
static bool v1first = false;

/* Multiple threads */
static const char * get_codec_filename(int enc_spec);
static void set_filebuf_watermark(int seconds);

/* Audio thread */
static struct event_queue audio_queue;
static long audio_stack[(DEFAULT_STACK_SIZE + 0x1000)/sizeof(long)];
static const char audio_thread_name[] = "audio";

static void audio_thread(void);
static void audio_initiate_track_change(long direction);
static bool audio_have_tracks(void);
static void audio_reset_buffer(void);

/* Codec thread */
extern struct codec_api ci;
static struct event_queue codec_queue;
static long codec_stack[(DEFAULT_STACK_SIZE + 0x2000)/sizeof(long)]
IBSS_ATTR;
static const char codec_thread_name[] = "codec";
/* For modifying thread priority later. */
struct thread_entry *codec_thread_p;

/* Voice thread */
#ifdef PLAYBACK_VOICE
extern struct codec_api ci_voice;

static volatile bool voice_thread_start;
static volatile bool voice_is_playing;
static volatile bool voice_codec_loaded;
static void (*voice_getmore)(unsigned char** start, int* size);
static char *voicebuf;
static size_t voice_remaining;
static struct thread_entry *voice_thread_p = NULL;

static struct event_queue voice_queue;
static long voice_stack[(DEFAULT_STACK_SIZE + 0x2000)/sizeof(long)]
IBSS_ATTR;
static const char voice_thread_name[] = "voice codec";
struct voice_info {
    void (*callback)(unsigned char **start, int *size);
    int size;
    char *buf;
};

#ifdef HAVE_ADJUSTABLE_CPU_FREQ
static void voice_boost_cpu(bool state);
#else
#define voice_boost_cpu(state)   do { } while(0)
#endif
static void voice_thread(void);

#endif /* PLAYBACK_VOICE */

/* --- External interfaces --- */

void mp3_play_data(const unsigned char* start, int size,
                   void (*get_more)(unsigned char** start, int* size))
{
#ifdef PLAYBACK_VOICE
    static struct voice_info voice_clip;
    voice_clip.callback = get_more;
    voice_clip.buf = (char *)start;
    voice_clip.size = size;
    LOGFQUEUE("mp3 > voice Q_VOICE_STOP");
    queue_post(&voice_queue, Q_VOICE_STOP, 0);
    LOGFQUEUE("mp3 > voice Q_VOICE_PLAY");
    queue_post(&voice_queue, Q_VOICE_PLAY, &voice_clip);
    voice_thread_start = true;
    voice_boost_cpu(true);
#else
    (void) start;
    (void) size;
    (void) get_more;
#endif
}

void mp3_play_stop(void)
{
#ifdef PLAYBACK_VOICE
    LOGFQUEUE("mp3 > voice Q_VOICE_STOP");
    queue_post(&voice_queue, Q_VOICE_STOP, 0);
#endif
}

bool mp3_pause_done(void)
{
    return pcm_is_paused();
}

void mpeg_id3_options(bool _v1first)
{
    v1first = _v1first;
}

void audio_load_encoder(int enc_id)
{
#if defined(HAVE_RECORDING) && !defined(SIMULATOR)
    const char *enc_fn = get_codec_filename(enc_id | CODEC_TYPE_ENCODER);
    if (!enc_fn)
        return;

    audio_remove_encoder();

    LOGFQUEUE("audio > codec Q_ENCODER_LOAD_DISK");
    queue_post(&codec_queue, Q_ENCODER_LOAD_DISK, (void *)enc_fn);

    while (!ci.enc_codec_loaded)
        yield();
#endif
    return;
    (void)enc_id;
} /* audio_load_encoder */

void audio_remove_encoder(void)
{
#if defined(HAVE_RECORDING) && !defined(SIMULATOR)
    /* force encoder codec unload (if previously loaded) */
    if (!ci.enc_codec_loaded)
        return;

    ci.stop_codec = true;
    while (ci.enc_codec_loaded)
        yield();
#endif
} /* audio_remove_encoder */

struct mp3entry* audio_current_track(void)
{
    const char *filename;
    const char *p;
    static struct mp3entry temp_id3;
    int cur_idx;
    int offset = ci.new_track + wps_offset;
    
    cur_idx = track_ridx + offset;
    cur_idx &= MAX_TRACK_MASK;

    if (tracks[cur_idx].taginfo_ready)
        return &tracks[cur_idx].id3;

    memset(&temp_id3, 0, sizeof(struct mp3entry));
    
    filename = playlist_peek(offset);
    if (!filename)
        filename = "No file!";

#ifdef HAVE_TC_RAMCACHE
    if (tagcache_fill_tags(&temp_id3, filename))
        return &temp_id3;
#endif

    p = strrchr(filename, '/');
    if (!p)
        p = filename;
    else
        p++;

    strncpy(temp_id3.path, p, sizeof(temp_id3.path)-1);
    temp_id3.title = &temp_id3.path[0];

    return &temp_id3;
}

struct mp3entry* audio_next_track(void)
{
    int next_idx = track_ridx;

    if (!audio_have_tracks())
        return NULL;

    next_idx++;
    next_idx &= MAX_TRACK_MASK;

    if (!tracks[next_idx].taginfo_ready)
        return NULL;

    return &tracks[next_idx].id3;
}

bool audio_has_changed_track(void)
{
    if (track_changed) 
    {
        track_changed = false;
        return true;
    }

    return false;
}

void audio_play(long offset)
{
    logf("audio_play");
    if (playing && offset <= 0)
    {
        LOGFQUEUE("audio > audio Q_AUDIO_NEW_PLAYLIST");
        queue_post(&audio_queue, Q_AUDIO_NEW_PLAYLIST, 0);
    }
    else
    {
        LOGFQUEUE("audio > audio Q_AUDIO_STOP");
        queue_post(&audio_queue, Q_AUDIO_STOP, 0);
        LOGFQUEUE("audio > audio Q_AUDIO_PLAY");
        queue_post(&audio_queue, Q_AUDIO_PLAY, (void *)offset);
    }
    while (!playing)
        yield();
}

void audio_stop(void)
{
    LOGFQUEUE("audio > audio Q_AUDIO_STOP");
    queue_post(&audio_queue, Q_AUDIO_STOP, 0);
    while(playing)
        yield();
}

void audio_pause(void)
{
    LOGFQUEUE("audio > audio Q_AUDIO_PAUSE");
    queue_post(&audio_queue, Q_AUDIO_PAUSE, (void *)true);
}

void audio_resume(void)
{
    LOGFQUEUE("audio > audio Q_AUDIO_PAUSE resume");
    queue_post(&audio_queue, Q_AUDIO_PAUSE, (void *)false);
}

void audio_next(void)
{
    if (playlist_check(ci.new_track + wps_offset + 1))
    {
        if (global_settings.beep)
            pcmbuf_beep(5000, 100, 2500*global_settings.beep);

        LOGFQUEUE("audio > audio Q_AUDIO_SKIP 1");
        queue_post(&audio_queue, Q_AUDIO_SKIP, (void *)1);
        /* Keep wps fast while our message travels inside deep playback queues. */
        wps_offset++;
        track_changed = true;
    }
    else
    {
        /* No more tracks. */
        if (global_settings.beep)
            pcmbuf_beep(1000, 100, 1000*global_settings.beep);
    }
}

void audio_prev(void)
{
    if (playlist_check(ci.new_track + wps_offset - 1))
    {
        if (global_settings.beep)
            pcmbuf_beep(5000, 100, 2500*global_settings.beep);

        LOGFQUEUE("audio > audio Q_AUDIO_SKIP -1");
        queue_post(&audio_queue, Q_AUDIO_SKIP, (void *)-1);
        /* Keep wps fast while our message travels inside deep playback queues. */
        wps_offset--;
        track_changed = true;
    }
    else
    {
        /* No more tracks. */
        if (global_settings.beep)
            pcmbuf_beep(1000, 100, 1000*global_settings.beep);
    }
}

void audio_next_dir(void)
{
    LOGFQUEUE("audio > audio Q_AUDIO_DIR_SKIP 1");
    queue_post(&audio_queue, Q_AUDIO_DIR_SKIP, (void *)1);
}

void audio_prev_dir(void)
{
    LOGFQUEUE("audio > audio Q_AUDIO_DIR_SKIP -1");
    queue_post(&audio_queue, Q_AUDIO_DIR_SKIP, (void *)-1);
}

void audio_pre_ff_rewind(void)
{
    LOGFQUEUE("audio > audio Q_AUDIO_PRE_FF_REWIND");
    queue_post(&audio_queue, Q_AUDIO_PRE_FF_REWIND, 0);
}

void audio_ff_rewind(long newpos)
{
    LOGFQUEUE("audio > audio Q_AUDIO_FF_REWIND");
    queue_post(&audio_queue, Q_AUDIO_FF_REWIND, (int *)newpos);
}

void audio_flush_and_reload_tracks(void)
{
    LOGFQUEUE("audio > audio Q_AUDIO_FLUSH");
    queue_post(&audio_queue, Q_AUDIO_FLUSH, 0);
}

void audio_error_clear(void)
{
}

int audio_status(void)
{
    int ret = 0;

    if (playing)
        ret |= AUDIO_STATUS_PLAY;

    if (paused)
        ret |= AUDIO_STATUS_PAUSE;

#ifdef HAVE_RECORDING
    /* Do this here for constitency with mpeg.c version */
    ret |= pcm_rec_status();
#endif

    return ret;
}

bool audio_query_poweroff(void)
{
    return !(playing && paused);
}

int audio_get_file_pos(void)
{
    return 0;
}

void audio_set_buffer_margin(int setting)
{
    static const int lookup[] = {5, 15, 30, 60, 120, 180, 300, 600};
    buffer_margin = lookup[setting];
    logf("buffer margin: %ds", buffer_margin);
    set_filebuf_watermark(buffer_margin);
}

/* Set crossfade & PCM buffer length. */
void audio_set_crossfade(int enable)
{
    size_t size;
    bool was_playing = (playing && audio_is_initialized);
    size_t offset = 0;
#if MEM > 1
    int seconds = 1;
#endif

    if (!filebuf)
        return;     /* Audio buffers not yet set up */

#if MEM > 1
    if (enable)
        seconds = global_settings.crossfade_fade_out_delay
                + global_settings.crossfade_fade_out_duration;

    /* Buffer has to be at least 2s long. */
    seconds += 2;
    logf("buf len: %d", seconds);
    size = seconds * (NATIVE_FREQUENCY*4);
#else
    enable = 0;
    size = NATIVE_FREQUENCY*2;
#endif
    if (pcmbuf_get_bufsize() == size)
        return ;

    if (was_playing)
    {
        /* Store the track resume position */
        offset = CUR_TI->id3.offset;

        /* Playback has to be stopped before changing the buffer size. */
        gui_syncsplash(0, true, (char *)str(LANG_RESTARTING_PLAYBACK));
        LOGFQUEUE("audio > audio Q_AUDIO_STOP");
        queue_post(&audio_queue, Q_AUDIO_STOP, 0);
        while (audio_codec_loaded)
            yield();
    }

    voice_stop();

    /* Re-initialize audio system. */
    pcmbuf_init(size);
    pcmbuf_crossfade_enable(enable);
    audio_reset_buffer();
    logf("abuf:%dB", pcmbuf_get_bufsize());
    logf("fbuf:%dB", filebuflen);

    voice_init();

    /* Restart playback. */
    if (was_playing) 
    {
        LOGFQUEUE("audio > audio Q_AUDIO_PLAY");
        queue_post(&audio_queue, Q_AUDIO_PLAY, (void *)offset);

        /* Wait for the playback to start again (and display the splash
           screen during that period. */
        while (!playing)
            yield();
    }
}

void audio_preinit(void)
{
    logf("playback system pre-init");

    filling = false;
    current_codec = CODEC_IDX_AUDIO;
    playing = false;
    paused = false;
    audio_codec_loaded = false;
#ifdef PLAYBACK_VOICE
    voice_is_playing = false;
    voice_thread_start = false;
    voice_codec_loaded = false;
#endif
    track_changed = false;
    current_fd = -1;
    track_buffer_callback = NULL;
    track_unbuffer_callback = NULL;
    track_changed_callback = NULL;
    /* Just to prevent CUR_TI never be anything random. */
    track_ridx = 0;

    mutex_init(&mutex_codecthread);

    queue_init(&audio_queue, true);
    queue_init(&codec_queue, true);
    /* create a private queue */
    queue_init(&codec_callback_queue, false);

    create_thread(audio_thread, audio_stack, sizeof(audio_stack),
                  audio_thread_name IF_PRIO(, PRIORITY_BUFFERING));
}

void audio_init(void)
{
    LOGFQUEUE("audio > audio Q_AUDIO_POSTINIT");
    queue_post(&audio_queue, Q_AUDIO_POSTINIT, 0);
}

void voice_init(void)
{
#ifdef PLAYBACK_VOICE
    if (!filebuf)
        return;     /* Audio buffers not yet set up */

    if (voice_thread_p)
        return;

    if (!talk_voice_required())
        return;

    logf("Starting voice codec");
    queue_init(&voice_queue, true);
    voice_thread_p = create_thread(voice_thread, voice_stack,
            sizeof(voice_stack), voice_thread_name 
            IF_PRIO(, PRIORITY_PLAYBACK));
    
    while (!voice_codec_loaded)
        yield();
#endif
} /* voice_init */

void voice_stop(void)
{
#ifdef PLAYBACK_VOICE
    /* Messages should not be posted to voice codec queue unless it is the
       current codec or deadlocks happen.
       -- jhMikeS */
    if (current_codec != CODEC_IDX_VOICE)
        return;

    LOGFQUEUE("mp3 > voice Q_VOICE_STOP");
    queue_post(&voice_queue, Q_VOICE_STOP, 0);
    while (voice_is_playing)
        yield();
    if (!playing)   
        pcmbuf_play_stop();
#endif
} /* voice_stop */



/* --- Routines called from multiple threads --- */

#ifdef PLAYBACK_VOICE
static void swap_codec(void)
{
    int my_codec = current_codec;

    logf("swapping out codec:%d", my_codec);

    /* Save our current IRAM and DRAM */
    memcpy(iram_buf[my_codec], (unsigned char *)CODEC_IRAM_ORIGIN,
            CODEC_IRAM_SIZE);
    memcpy(dram_buf[my_codec], codecbuf, CODEC_SIZE);

    /* Release my semaphore */
    mutex_unlock(&mutex_codecthread);

    /* Loop until the other codec has locked and run */
    do {
        /* Release my semaphore and force a task switch. */
        yield();
    } while (my_codec == current_codec);

    /* Wait for other codec to unlock */
    mutex_lock(&mutex_codecthread);

    /* Take control */
    current_codec = my_codec;

    /* Reload our IRAM and DRAM */
    memcpy((unsigned char *)CODEC_IRAM_ORIGIN, iram_buf[my_codec],
            CODEC_IRAM_SIZE);
    invalidate_icache();
    memcpy(codecbuf, dram_buf[my_codec], CODEC_SIZE);

    logf("resuming codec:%d", my_codec);
}
#endif

static void set_filebuf_watermark(int seconds)
{
    size_t bytes;

    if (current_codec == CODEC_IDX_VOICE)
        return;

    if (!filebuf)
        return;     /* Audio buffers not yet set up */

    bytes = MAX(CUR_TI->id3.bitrate * seconds * (1000/8), conf_watermark);
    bytes = MIN(bytes, filebuflen / 2);
    conf_watermark = bytes;
}

static const char * get_codec_filename(int enc_spec)
{
    const char *fname;
    int type = enc_spec & CODEC_TYPE_MASK;
    int afmt = enc_spec & CODEC_AFMT_MASK;

    if ((unsigned)afmt >= AFMT_NUM_CODECS)
        type = AFMT_UNKNOWN | (type & CODEC_TYPE_MASK);

    fname = (type == CODEC_TYPE_DECODER) ?
        audio_formats[afmt].codec_fn : audio_formats[afmt].codec_enc_fn;

    logf("%s: %d - %s",
        (type == CODEC_TYPE_ENCODER) ? "Encoder" : "Decoder",
        afmt, fname ? fname : "<unknown>");

    return fname;
} /* get_codec_filename */


/* --- Voice thread --- */

#ifdef PLAYBACK_VOICE

#ifdef HAVE_ADJUSTABLE_CPU_FREQ
static void voice_boost_cpu(bool state)
{
    static bool voice_cpu_boosted = false;

    if (state != voice_cpu_boosted)
    {
        voice_cpu_boosted = state;
        cpu_boost_id(state, CPUBOOSTID_PLAYBACK_VOICE);
    }
}
#endif

static bool voice_pcmbuf_insert_split_callback(
        const void *ch1, const void *ch2, size_t length)
{
    const char* src[2];
    char *dest;
    long input_size;
    size_t output_size;

    src[0] = ch1;
    src[1] = ch2;

    if (dsp_stereo_mode() == STEREO_NONINTERLEAVED)
        length *= 2;    /* Length is per channel */

    while (length)
    {
        long est_output_size = dsp_output_size(length);
        
        while ((dest = pcmbuf_request_voice_buffer(est_output_size,
                        &output_size, playing)) == NULL)
        {
            if (playing && audio_codec_loaded)
                swap_codec();
            else
                yield();
        }
        
        /* Get the real input_size for output_size bytes, guarding
         * against resampling buffer overflows. */
        input_size = dsp_input_size(output_size);

        if (input_size <= 0) 
        {
            DEBUGF("Error: dsp_input_size(%ld=dsp_output_size(%ld))=%ld<=0\n",
                    output_size, length, input_size);
            /* If this happens, there are samples of codec data that don't
             * become a number of pcm samples, and something is broken */
            return false;
        }

        /* Input size has grown, no error, just don't write more than length */
        if ((size_t)input_size > length)
            input_size = length;

        output_size = dsp_process(dest, src, input_size);

        if (playing)
        {
            pcmbuf_mix_voice(output_size);
            if ((pcmbuf_usage() < 10 || pcmbuf_mix_free() < 30) && audio_codec_loaded)
                swap_codec();
        }
        else
            pcmbuf_write_complete(output_size);

        length -= input_size;
    }

    return true;
} /* voice_pcmbuf_insert_split_callback */

static bool voice_pcmbuf_insert_callback(const char *buf, size_t length)
{
    /* TODO: The audiobuffer API should probably be updated, and be based on
     *       pcmbuf_insert_split().  */
    long real_length = length;

    if (dsp_stereo_mode() == STEREO_NONINTERLEAVED)
        length /= 2;    /* Length is per channel */

    /* Second channel is only used for non-interleaved stereo. */
    return voice_pcmbuf_insert_split_callback(buf, buf + (real_length / 2),
        length);
}

static void* voice_get_memory_callback(size_t *size)
{
    *size = 0;
    return NULL;
}

static void voice_set_elapsed_callback(unsigned int value)
{
    (void)value;
}

static void voice_set_offset_callback(size_t value)
{
    (void)value;
}

static size_t voice_filebuf_callback(void *ptr, size_t size)
{
    (void)ptr;
    (void)size;

    return 0;
}

static void* voice_request_buffer_callback(size_t *realsize, size_t reqsize)
{
    struct event ev;

    if (ci_voice.new_track)
    {
        *realsize = 0;
        return NULL;
    }

    while (1)
    {
        if (voice_is_playing || playing)
            queue_wait_w_tmo(&voice_queue, &ev, 0);
        else
            queue_wait(&voice_queue, &ev);
        if (!voice_is_playing)
        {
            if (ev.id == SYS_TIMEOUT)
                ev.id = Q_AUDIO_PLAY;
        }

        switch (ev.id) {
            case Q_AUDIO_PLAY:
                LOGFQUEUE("voice < Q_AUDIO_PLAY");
                if (playing)
                {
                    if (audio_codec_loaded)
                        swap_codec();
                    yield();
                }
                break;

#if defined(HAVE_RECORDING) && !defined(SIMULATOR)
            case Q_ENCODER_RECORD:
                LOGFQUEUE("voice < Q_ENCODER_RECORD");
                swap_codec();
                break;
#endif

            case Q_VOICE_STOP:
                LOGFQUEUE("voice < Q_VOICE_STOP");
                if (voice_is_playing)
                {
                    /* Clear the current buffer */
                    voice_is_playing = false;
                    voice_getmore = NULL;
                    voice_remaining = 0;
                    voicebuf = NULL;
                    voice_boost_cpu(false);

                    /* Force the codec to think it's changing tracks */
                    ci_voice.new_track = 1; 
                    *realsize = 0;
                    return NULL;
                }
                else
                    break;

            case SYS_USB_CONNECTED:
                LOGFQUEUE("voice < SYS_USB_CONNECTED");
                usb_acknowledge(SYS_USB_CONNECTED_ACK);
                if (audio_codec_loaded)
                    swap_codec();
                usb_wait_for_disconnect(&voice_queue);
                break;

            case Q_VOICE_PLAY:
                LOGFQUEUE("voice < Q_VOICE_PLAY");
                if (!voice_is_playing)
                {
                    /* Slight hack - flush PCM buffer if only being used for voice */
                    if (!playing && pcm_is_playing())
                        pcmbuf_play_stop();

                    /* Set up new voice data */
                    struct voice_info *voice_data;
                    voice_is_playing = true;
                    voice_boost_cpu(true);
                    voice_data = ev.data;
                    voice_remaining = voice_data->size;
                    voicebuf = voice_data->buf;
                    voice_getmore = voice_data->callback;
                }
                goto voice_play_clip;

            case SYS_TIMEOUT:
                LOGFQUEUE("voice < SYS_TIMEOUT");
                goto voice_play_clip;

            default:
                LOGFQUEUE("voice < default");
        }
    }

voice_play_clip:

    if (voice_remaining == 0 || voicebuf == NULL)
    {
        if (voice_getmore)
            voice_getmore((unsigned char **)&voicebuf, (int *)&voice_remaining);

        /* If this clip is done */
        if (voice_remaining == 0)
        {
            LOGFQUEUE("voice > voice Q_VOICE_STOP");
            queue_post(&voice_queue, Q_VOICE_STOP, 0);
            /* Force pcm playback. */
            if (!pcm_is_playing())
                pcmbuf_play_start();
        }
    }
    
    *realsize = MIN(voice_remaining, reqsize);

    if (*realsize == 0)
        return NULL;

    return voicebuf;
} /* voice_request_buffer_callback */

static void voice_advance_buffer_callback(size_t amount)
{
    amount = MIN(amount, voice_remaining);
    voicebuf += amount;
    voice_remaining -= amount;
}

static void voice_advance_buffer_loc_callback(void *ptr)
{
    size_t amount = (size_t)ptr - (size_t)voicebuf;
    
    voice_advance_buffer_callback(amount);
}

static off_t voice_mp3_get_filepos_callback(int newtime)
{
    (void)newtime;
    
    return 0;
}

static void voice_do_nothing(void)
{
    return;
}

static bool voice_seek_buffer_callback(size_t newpos)
{
    (void)newpos;
    
    return false;
}

static bool voice_request_next_track_callback(void)
{
    ci_voice.new_track = 0;
    return true;
}

static void voice_thread(void)
{
    while (1)
    {
        logf("Loading voice codec");
        voice_codec_loaded = true;
        mutex_lock(&mutex_codecthread);
        current_codec = CODEC_IDX_VOICE;
        dsp_configure(DSP_RESET, 0);
        voice_remaining = 0;
        voice_getmore = NULL;

        codec_load_file(get_codec_filename(AFMT_MPA_L3), &ci_voice);

        logf("Voice codec finished");
        voice_codec_loaded = false;
        mutex_unlock(&mutex_codecthread);
    }
} /* voice_thread */

#endif /* PLAYBACK_VOICE */

/* --- Codec thread --- */

static bool codec_pcmbuf_insert_split_callback(
        const void *ch1, const void *ch2, size_t length)
{
    const char* src[2];
    char *dest;
    long input_size;
    size_t output_size;

    src[0] = ch1;
    src[1] = ch2;

    if (dsp_stereo_mode() == STEREO_NONINTERLEAVED)
        length *= 2;    /* Length is per channel */

    while (length)
    {
        long est_output_size = dsp_output_size(length);
        /* Prevent audio from a previous track from playing */
        if (ci.new_track || ci.stop_codec)
            return true;

        while ((dest = pcmbuf_request_buffer(est_output_size,
                        &output_size)) == NULL) 
        {
            sleep(1);
            if (ci.seek_time || ci.new_track || ci.stop_codec)
                return true;
        }

        /* Get the real input_size for output_size bytes, guarding
         * against resampling buffer overflows. */
        input_size = dsp_input_size(output_size);

        if (input_size <= 0) 
        {
            DEBUGF("Error: dsp_input_size(%ld=dsp_output_size(%ld))=%ld<=0\n",
                    output_size, length, input_size);
            /* If this happens, there are samples of codec data that don't
             * become a number of pcm samples, and something is broken */
            return false;
        }

        /* Input size has grown, no error, just don't write more than length */
        if ((size_t)input_size > length)
            input_size = length;

        output_size = dsp_process(dest, src, input_size);

        pcmbuf_write_complete(output_size);

#ifdef PLAYBACK_VOICE
        if ((voice_is_playing || voice_thread_start) 
            && pcm_is_playing() && voice_codec_loaded &&
            pcmbuf_usage() > 30 && pcmbuf_mix_free() > 80)
        {
            voice_thread_start = false;
            swap_codec();
        }
#endif
        
        length -= input_size;
    }

    return true;
} /* codec_pcmbuf_insert_split_callback */

static bool codec_pcmbuf_insert_callback(const char *buf, size_t length)
{
    /* TODO: The audiobuffer API should probably be updated, and be based on
     *       pcmbuf_insert_split().  */
    long real_length = length;

    if (dsp_stereo_mode() == STEREO_NONINTERLEAVED)
        length /= 2;    /* Length is per channel */

    /* Second channel is only used for non-interleaved stereo. */
    return codec_pcmbuf_insert_split_callback(buf, buf + (real_length / 2),
        length);
}

static void* codec_get_memory_callback(size_t *size)
{
    *size = MALLOC_BUFSIZE;
    return &audiobuf[talk_get_bufsize()];
}

static void codec_pcmbuf_position_callback(size_t size) ICODE_ATTR;
static void codec_pcmbuf_position_callback(size_t size) 
{
    unsigned int time = size * 1000 / 4 / NATIVE_FREQUENCY +
        prev_ti->id3.elapsed;
    
    if (time >= prev_ti->id3.length) 
    {
        pcmbuf_set_position_callback(NULL);
        prev_ti->id3.elapsed = prev_ti->id3.length;
    } 
    else 
        prev_ti->id3.elapsed = time;
}

static void codec_set_elapsed_callback(unsigned int value)
{
    unsigned int latency;
    if (ci.seek_time)
        return;

#ifdef AB_REPEAT_ENABLE
    ab_position_report(value);
#endif

    latency = pcmbuf_get_latency();
    if (value < latency)
        CUR_TI->id3.elapsed = 0;
    else if (value - latency > CUR_TI->id3.elapsed ||
            value - latency < CUR_TI->id3.elapsed - 2)
    {
        CUR_TI->id3.elapsed = value - latency;
    }
}

static void codec_set_offset_callback(size_t value)
{
    unsigned int latency;
    
    if (ci.seek_time)
        return;

    latency = pcmbuf_get_latency() * CUR_TI->id3.bitrate / 8;
    if (value < latency)
        CUR_TI->id3.offset = 0;
    else
        CUR_TI->id3.offset = value - latency;
}

static void codec_advance_buffer_counters(size_t amount) 
{
    buf_ridx = RINGBUF_ADD(buf_ridx, amount);
    
    ci.curpos += amount;
    CUR_TI->available -= amount;

    /* Start buffer filling as necessary. */
    if (!pcmbuf_is_lowdata() && !filling)
    {
        if (conf_watermark && FILEBUFUSED <= conf_watermark && playing)
        {
            LOGFQUEUE("codec > audio Q_AUDIO_FILL_BUFFER");
            queue_post(&audio_queue, Q_AUDIO_FILL_BUFFER, 0);
        }
    }
}

/* copy up-to size bytes into ptr and return the actual size copied */
static size_t codec_filebuf_callback(void *ptr, size_t size)
{
    char *buf = (char *)ptr;
    size_t copy_n;
    size_t part_n;

    if (ci.stop_codec || !playing)
        return 0;

    /* The ammount to copy is the lesser of the requested amount and the
     * amount left of the current track (both on disk and already loaded) */
    copy_n = MIN(size, CUR_TI->available + CUR_TI->filerem);

    /* Nothing requested OR nothing left */
    if (copy_n == 0)
        return 0;

    /* Let the disk buffer catch fill until enough data is available */
    while (copy_n > CUR_TI->available) 
    {
        if (!filling)
        {
            LOGFQUEUE("codec > audio Q_AUDIO_FILL_BUFFER");
            queue_post(&audio_queue, Q_AUDIO_FILL_BUFFER, 0);
        }
        
        sleep(1);
        if (ci.stop_codec || ci.new_track)
            return 0;
    }

    /* Copy as much as possible without wrapping */
    part_n = MIN(copy_n, filebuflen - buf_ridx);
    memcpy(buf, &filebuf[buf_ridx], part_n);
    /* Copy the rest in the case of a wrap */
    if (part_n < copy_n) {
        memcpy(&buf[part_n], &filebuf[0], copy_n - part_n);
    }

    /* Update read and other position pointers */
    codec_advance_buffer_counters(copy_n);

    /* Return the actual amount of data copied to the buffer */
    return copy_n;
} /* codec_filebuf_callback */

static void* codec_request_buffer_callback(size_t *realsize, size_t reqsize)
{
    size_t short_n, copy_n, buf_rem;

    if (!playing) 
    {
        *realsize = 0;
        return NULL;
    }

    copy_n = MIN(reqsize, CUR_TI->available + CUR_TI->filerem);
    if (copy_n == 0) 
    {
        *realsize = 0;
        return NULL;
    }

    while (copy_n > CUR_TI->available) 
    {
        if (!filling)
        {
            LOGFQUEUE("codec > audio Q_AUDIO_FILL_BUFFER");
            queue_post(&audio_queue, Q_AUDIO_FILL_BUFFER, 0);
        }
        
        sleep(1);
        if (ci.stop_codec || ci.new_track) 
        {
            *realsize = 0;
            return NULL;
        }
    }

    /* How much is left at the end of the file buffer before wrap? */
    buf_rem = filebuflen - buf_ridx;
    
    /* If we can't satisfy the request without wrapping */
    if (buf_rem < copy_n) 
    {
        /* How short are we? */
        short_n = copy_n - buf_rem;
        
        /* If we can fudge it with the guardbuf */
        if (short_n < GUARD_BUFSIZE)
            memcpy(&filebuf[filebuflen], &filebuf[0], short_n);
        else
            copy_n = buf_rem;
    }

    *realsize = copy_n;
    
    return (char *)&filebuf[buf_ridx];
} /* codec_request_buffer_callback */

static int get_codec_base_type(int type)
{
    switch (type) {
        case AFMT_MPA_L1:
        case AFMT_MPA_L2:
        case AFMT_MPA_L3:
            return AFMT_MPA_L3;
    }

    return type;
}

static void codec_advance_buffer_callback(size_t amount)
{
    if (amount > CUR_TI->available + CUR_TI->filerem)
        amount = CUR_TI->available + CUR_TI->filerem;

    while (amount > CUR_TI->available && filling)
        sleep(1);

    if (amount > CUR_TI->available) 
    {
        struct event ev;
        
        LOGFQUEUE("codec > audio Q_AUDIO_REBUFFER_SEEK");
        queue_post(&audio_queue,
                Q_AUDIO_REBUFFER_SEEK, (void *)(ci.curpos + amount));
        
        queue_wait(&codec_callback_queue, &ev);
        switch (ev.id)
        {
            case Q_CODEC_REQUEST_FAILED:
                LOGFQUEUE("codec < Q_CODEC_REQUEST_FAILED");
                ci.stop_codec = true;
                return;

            case Q_CODEC_REQUEST_COMPLETE:
                LOGFQUEUE("codec < Q_CODEC_REQUEST_COMPLETE");
                return;           

            default:
                LOGFQUEUE("codec < default");
                ci.stop_codec = true;
                return;
        }
    }

    codec_advance_buffer_counters(amount);

    codec_set_offset_callback(ci.curpos);
}

static void codec_advance_buffer_loc_callback(void *ptr)
{
    size_t amount = (size_t)ptr - (size_t)&filebuf[buf_ridx];
    
    codec_advance_buffer_callback(amount);
}

/* Copied from mpeg.c. Should be moved somewhere else. */
static int codec_get_file_pos(void)
{
    int pos = -1;
    struct mp3entry *id3 = audio_current_track();

    if (id3->vbr)
    {
        if (id3->has_toc)
        {
            /* Use the TOC to find the new position */
            unsigned int percent, remainder;
            int curtoc, nexttoc, plen;

            percent = (id3->elapsed*100)/id3->length;
            if (percent > 99)
                percent = 99;

            curtoc = id3->toc[percent];

            if (percent < 99)
                nexttoc = id3->toc[percent+1];
            else
                nexttoc = 256;

            pos = (id3->filesize/256)*curtoc;

            /* Use the remainder to get a more accurate position */
            remainder   = (id3->elapsed*100)%id3->length;
            remainder   = (remainder*100)/id3->length;
            plen        = (nexttoc - curtoc)*(id3->filesize/256);
            pos        += (plen/100)*remainder;
        }
        else
        {
            /* No TOC exists, estimate the new position */
            pos = (id3->filesize / (id3->length / 1000)) *
                (id3->elapsed / 1000);
        }
    }
    else if (id3->bitrate)
        pos = id3->elapsed * (id3->bitrate / 8);
    else
        return -1;

    /* Don't seek right to the end of the file so that we can
       transition properly to the next song */
    if (pos >= (int)(id3->filesize - id3->id3v1len))
        pos = id3->filesize - id3->id3v1len - 1;
    /* skip past id3v2 tag and other leading garbage */
    else if (pos < (int)id3->first_frame_offset)
        pos = id3->first_frame_offset;

    return pos;
}

static off_t codec_mp3_get_filepos_callback(int newtime)
{
    off_t newpos;

    CUR_TI->id3.elapsed = newtime;
    newpos = codec_get_file_pos();

    return newpos;
}

static void codec_seek_complete_callback(void)
{
    logf("seek_complete");
    if (pcm_is_paused()) 
    {
        /* If this is not a seamless seek, clear the buffer */
        pcmbuf_play_stop();
        
        /* If playback was not 'deliberately' paused, unpause now */
        if (!paused)
            pcmbuf_pause(false);
    }
    ci.seek_time = 0;
}

static bool codec_seek_buffer_callback(size_t newpos)
{
    int difference;

    logf("codec_seek_buffer_callback");

    if (newpos >= CUR_TI->filesize)
        newpos = CUR_TI->filesize - 1;

    difference = newpos - ci.curpos;
    if (difference >= 0) 
    {
        /* Seeking forward */
        logf("seek: +%d", difference);
        codec_advance_buffer_callback(difference);
        return true;
    }

    /* Seeking backward */
    difference = -difference;
    if (ci.curpos - difference < 0)
        difference = ci.curpos;

    /* We need to reload the song. */
    if (newpos < CUR_TI->start_pos)
    {
        struct event ev;
        
        LOGFQUEUE("codec > audio Q_AUDIO_REBUFFER_SEEK");
        queue_post(&audio_queue, Q_AUDIO_REBUFFER_SEEK, (void *)newpos);
        
        queue_wait(&codec_callback_queue, &ev);
        switch (ev.id)
        {
            case Q_CODEC_REQUEST_COMPLETE:
                LOGFQUEUE("codec < Q_CODEC_REQUEST_COMPLETE");
                return true;
            
            case Q_CODEC_REQUEST_FAILED:
                LOGFQUEUE("codec < Q_CODEC_REQUEST_FAILED");
                ci.stop_codec = true;
                return false;
            
            default:
                LOGFQUEUE("codec < default");
                return false;
        }
    }

    /* Seeking inside buffer space. */
    logf("seek: -%d", difference);
    CUR_TI->available += difference;
    buf_ridx = RINGBUF_SUB(buf_ridx, (unsigned)difference);
    ci.curpos -= difference;

    return true;
}

static void codec_configure_callback(int setting, void *value)
{
    switch (setting) {
    case CODEC_SET_FILEBUF_WATERMARK:
        conf_watermark = (unsigned long)value;
        set_filebuf_watermark(buffer_margin);
        break;

    case CODEC_SET_FILEBUF_CHUNKSIZE:
        conf_filechunk = (unsigned long)value;
        break;

    case CODEC_SET_FILEBUF_PRESEEK:
        conf_preseek = (unsigned long)value;
        break;

    default:
        if (!dsp_configure(setting, value)) { logf("Illegal key:%d", setting); }
    }
}

static void codec_track_changed(void)
{
    automatic_skip = false;
    track_changed = true;
    LOGFQUEUE("codec > audio Q_AUDIO_TRACK_CHANGED");
    queue_post(&audio_queue, Q_AUDIO_TRACK_CHANGED, 0);
}

static void codec_pcmbuf_track_changed_callback(void)
{
    pcmbuf_set_position_callback(NULL);
    codec_track_changed();
}

static void codec_discard_codec_callback(void)
{
    if (CUR_TI->has_codec) 
    {
        CUR_TI->has_codec = false;
        buf_ridx = RINGBUF_ADD(buf_ridx, CUR_TI->codecsize);
    }

#if 0
    /* Check if a buffer desync has happened, log it and stop playback. */
    if (buf_ridx != CUR_TI->buf_idx)
    {
        int offset = CUR_TI->buf_idx - buf_ridx;
        size_t new_used = FILEBUFUSED - offset;
        
        logf("Buf off :%d=%d-%d", offset, CUR_TI->buf_idx, buf_ridx);
        logf("Used off:%d",FILEBUFUSED - new_used);
        
        /* This is a fatal internal error and it's not safe to 
         * continue playback. */
        ci.stop_codec = true;
        queue_post(&audio_queue, Q_AUDIO_STOP, 0);
    }
#endif
}

static void codec_track_skip_done(bool was_manual)
{
    /* Manual track change (always crossfade or flush audio). */
    if (was_manual)
    {
        pcmbuf_crossfade_init(true);
        LOGFQUEUE("codec > audio Q_AUDIO_TRACK_CHANGED");
        queue_post(&audio_queue, Q_AUDIO_TRACK_CHANGED, 0);
    }
    /* Automatic track change w/crossfade, if not in "Track Skip Only" mode. */
    else if (pcmbuf_is_crossfade_enabled() && !pcmbuf_is_crossfade_active()
             && global_settings.crossfade != 2)
    {
        pcmbuf_crossfade_init(false);
        codec_track_changed();
    }
    /* Gapless playback. */
    else
    {
        pcmbuf_set_position_callback(codec_pcmbuf_position_callback);
        pcmbuf_set_event_handler(codec_pcmbuf_track_changed_callback);
    }
}

static bool codec_load_next_track(void) 
{
    struct event ev;

    if (ci.seek_time)
        codec_seek_complete_callback();

#ifdef AB_REPEAT_ENABLE
    ab_end_of_track_report();
#endif

    logf("Request new track");

    if (ci.new_track == 0)
    {
        ci.new_track++;
        automatic_skip = true;
    }
    
    cpu_boost_id(true, CPUBOOSTID_PLAYBACK_CODEC);
    LOGFQUEUE("codec > audio Q_AUDIO_CHECK_NEW_TRACK");
    queue_post(&audio_queue, Q_AUDIO_CHECK_NEW_TRACK, 0);
    while (1) 
    {
        queue_wait(&codec_callback_queue, &ev);
        if (ev.id == Q_CODEC_REQUEST_PENDING)
        {
            if (!automatic_skip)
                pcmbuf_play_stop();
        }
        else
            break;
    }
    cpu_boost_id(false, CPUBOOSTID_PLAYBACK_CODEC);
    switch (ev.id)
    {
        case Q_CODEC_REQUEST_COMPLETE:
            LOGFQUEUE("codec < Q_CODEC_REQUEST_COMPLETE");
            codec_track_skip_done(!automatic_skip);
            return true;

        case Q_CODEC_REQUEST_FAILED:
            LOGFQUEUE("codec < Q_CODEC_REQUEST_FAILED");
            ci.new_track = 0;
            ci.stop_codec = true;
            return false;

        default:
            LOGFQUEUE("codec < default");
            ci.stop_codec = true;
            return false;
    }
}

static bool codec_request_next_track_callback(void)
{
    int prev_codectype;

    if (ci.stop_codec || !playing)
        return false;

    prev_codectype = get_codec_base_type(CUR_TI->id3.codectype);

    if (!codec_load_next_track())
        return false;

    /* Check if the next codec is the same file. */
    if (prev_codectype == get_codec_base_type(CUR_TI->id3.codectype))
    {
        logf("New track loaded");
        codec_discard_codec_callback();
        return true;
    }
    else
    {
        logf("New codec:%d/%d", CUR_TI->id3.codectype, prev_codectype);
        return false;
    }
}

static void codec_thread(void)
{
    struct event ev;
    int status;
    size_t wrap;

    while (1) {
        status = 0;
        queue_wait(&codec_queue, &ev);

        switch (ev.id) {
            case Q_CODEC_LOAD_DISK:
                LOGFQUEUE("codec < Q_CODEC_LOAD_DISK");
                audio_codec_loaded = true;
#ifdef PLAYBACK_VOICE
                /* Don't sent messages to voice codec if it's not current */
                if (voice_codec_loaded && current_codec == CODEC_IDX_VOICE)
                {
                    LOGFQUEUE("codec > voice Q_AUDIO_PLAY");
                    queue_post(&voice_queue, Q_AUDIO_PLAY, 0);
                }
#endif
                mutex_lock(&mutex_codecthread);
                current_codec = CODEC_IDX_AUDIO;
                ci.stop_codec = false;
                status = codec_load_file((const char *)ev.data, &ci);
                mutex_unlock(&mutex_codecthread);
                break ;

            case Q_CODEC_LOAD:
                LOGFQUEUE("codec < Q_CODEC_LOAD");
                if (!CUR_TI->has_codec) {
                    logf("Codec slot is empty!");
                    /* Wait for the pcm buffer to go empty */
                    while (pcm_is_playing())
                        yield();
                    /* This must be set to prevent an infinite loop */
                    ci.stop_codec = true;
                    LOGFQUEUE("codec > codec Q_AUDIO_PLAY");
                    queue_post(&codec_queue, Q_AUDIO_PLAY, 0);
                    break ;
                }

                audio_codec_loaded = true;
#ifdef PLAYBACK_VOICE
                if (voice_codec_loaded && current_codec == CODEC_IDX_VOICE)
                {
                    LOGFQUEUE("codec > voice Q_AUDIO_PLAY");
                    queue_post(&voice_queue, Q_AUDIO_PLAY, 0);
                }
#endif
                mutex_lock(&mutex_codecthread);
                current_codec = CODEC_IDX_AUDIO;
                ci.stop_codec = false;
                wrap = (size_t)&filebuf[filebuflen] - (size_t)CUR_TI->codecbuf;
                status = codec_load_ram(CUR_TI->codecbuf, CUR_TI->codecsize,
                        &filebuf[0], wrap, &ci);
                mutex_unlock(&mutex_codecthread);
                break ;

#if defined(HAVE_RECORDING) && !defined(SIMULATOR)
            case Q_ENCODER_LOAD_DISK:
                LOGFQUEUE("codec < Q_ENCODER_LOAD_DISK");
                audio_codec_loaded = false; /* Not audio codec! */
#ifdef PLAYBACK_VOICE
                if (voice_codec_loaded && current_codec == CODEC_IDX_VOICE)
                {
                    LOGFQUEUE("codec > voice Q_ENCODER_RECORD");
                    queue_post(&voice_queue, Q_ENCODER_RECORD, NULL);
                }
#endif
                mutex_lock(&mutex_codecthread);
                current_codec = CODEC_IDX_AUDIO;
                ci.stop_codec = false;
                status = codec_load_file((const char *)ev.data, &ci);
                mutex_unlock(&mutex_codecthread);
                break;
#endif

#ifndef SIMULATOR
            case SYS_USB_CONNECTED:  
                LOGFQUEUE("codec < SYS_USB_CONNECTED");
                queue_clear(&codec_queue);
                usb_acknowledge(SYS_USB_CONNECTED_ACK);
                usb_wait_for_disconnect(&codec_queue);
                break;
#endif
                
            default:
                LOGFQUEUE("codec < default");
        }

        if (audio_codec_loaded)
        {
            if (ci.stop_codec)
            {
                status = CODEC_OK;
                if (!playing)
                    pcmbuf_play_stop();
            }
            audio_codec_loaded = false;
        }

        switch (ev.id) {
            case Q_CODEC_LOAD_DISK:
            case Q_CODEC_LOAD:
                LOGFQUEUE("codec < Q_CODEC_LOAD");
                if (playing) 
                {
                    if (ci.new_track || status != CODEC_OK) 
                    {
                        if (!ci.new_track) 
                        {
                            logf("Codec failure");
                            gui_syncsplash(HZ*2, true, "Codec failure");
                        }
                        
                        if (!codec_load_next_track())
                        {
                            // queue_post(&codec_queue, Q_AUDIO_STOP, 0);
                            LOGFQUEUE("codec > audio Q_AUDIO_STOP");
                            queue_post(&audio_queue, Q_AUDIO_STOP, 0);
                            break;
                        }
                    } 
                    else 
                    {
                        logf("Codec finished");
                        if (ci.stop_codec)
                        {
                            /* Wait for the audio to stop playing before
                             * triggering the WPS exit */
                            while(pcm_is_playing())
                                sleep(1);
                            LOGFQUEUE("codec > audio Q_AUDIO_STOP");
                            queue_post(&audio_queue, Q_AUDIO_STOP, 0);
                            break;
                        }
                    }
                    
                    if (CUR_TI->has_codec)
                    {
                        LOGFQUEUE("codec > codec Q_CODEC_LOAD");
                        queue_post(&codec_queue, Q_CODEC_LOAD, 0);
                    }
                    else
                    {
                        const char *codec_fn = get_codec_filename(CUR_TI->id3.codectype);
                        LOGFQUEUE("codec > codec Q_CODEC_LOAD_DISK");
                        queue_post(&codec_queue, Q_CODEC_LOAD_DISK,
                            (void *)codec_fn);
                    }
                }
                break;

            default:
                LOGFQUEUE("codec < default");

        } /* end switch */
    }
}


/* --- Audio thread --- */

static bool audio_filebuf_is_lowdata(void)
{
    return FILEBUFUSED < AUDIO_FILEBUF_CRITICAL;
}

static bool audio_have_tracks(void)
{
    return track_ridx != track_widx || CUR_TI->filesize;
}

static bool audio_have_free_tracks(void)
{
    if (track_widx < track_ridx)
        return track_widx + 1 < track_ridx;
    else if (track_ridx == 0)
        return track_widx < MAX_TRACK - 1;
    
    return true;
}
        
int audio_track_count(void)
{
    if (audio_have_tracks())
    {
        int relative_track_widx = track_widx;
        
        if (track_ridx > track_widx)
            relative_track_widx += MAX_TRACK;
        
        return relative_track_widx - track_ridx + 1;
    }
    
    return 0;
}

long audio_filebufused(void)
{
    return (long) FILEBUFUSED;
}

/* Count the data BETWEEN the selected tracks */
static size_t audio_buffer_count_tracks(int from_track, int to_track) 
{
    size_t amount = 0;
    bool need_wrap = to_track < from_track;

    while (1)
    {
        if (++from_track >= MAX_TRACK)
        {
            from_track -= MAX_TRACK;
            need_wrap = false;
        }
        
        if (from_track >= to_track && !need_wrap)
            break;
        
        amount += tracks[from_track].codecsize + tracks[from_track].filesize;
    }
    return amount;
}

static bool audio_buffer_wind_forward(int new_track_ridx, int old_track_ridx)
{
    size_t amount;

    /* Start with the remainder of the previously playing track */
    amount = tracks[old_track_ridx].filesize - ci.curpos;
    /* Then collect all data from tracks in between them */
    amount += audio_buffer_count_tracks(old_track_ridx, new_track_ridx);
    
    if (amount > FILEBUFUSED)
        return false;

    logf("bwf:%ldB",amount);

    /* Wind the buffer to the beginning of the target track or its codec */
    buf_ridx = RINGBUF_ADD(buf_ridx, amount);
    
    return true;
}

static bool audio_buffer_wind_backward(int new_track_ridx, int old_track_ridx) 
{
    /* Available buffer data */
    size_t buf_back;
    /* Start with the previously playing track's data and our data */
    size_t amount;
    
    amount = ci.curpos;
    buf_back = RINGBUF_SUB(buf_ridx, buf_widx);
    
    /* If we're not just resetting the current track */
    if (new_track_ridx != old_track_ridx)
    {
        /* Need to wind to before the old track's codec and our filesize */
        amount += tracks[old_track_ridx].codecsize;
        amount += tracks[new_track_ridx].filesize;

        /* Rewind the old track to its beginning */
        tracks[old_track_ridx].available =
            tracks[old_track_ridx].filesize - tracks[old_track_ridx].filerem;
    }

    /* If the codec was ever buffered */
    if (tracks[new_track_ridx].codecsize)
    {
        /* Add the codec to the needed size */
        amount += tracks[new_track_ridx].codecsize;
        tracks[new_track_ridx].has_codec = true;
    }

    /* Then collect all data from tracks between new and old */
    amount += audio_buffer_count_tracks(new_track_ridx, old_track_ridx);

    /* Do we have space to make this skip? */
    if (amount > buf_back)
        return false;

    logf("bwb:%ldB",amount);

    /* Rewind the buffer to the beginning of the target track or its codec */
    buf_ridx = RINGBUF_SUB(buf_ridx, amount);

    /* Reset to the beginning of the new track */
    tracks[new_track_ridx].available = tracks[new_track_ridx].filesize;

    return true;
}

static void audio_update_trackinfo(void)
{
    ci.filesize = CUR_TI->filesize;
    CUR_TI->id3.elapsed = 0;
    CUR_TI->id3.offset = 0;
    ci.id3 = &CUR_TI->id3;
    ci.curpos = 0;
    ci.taginfo_ready = &CUR_TI->taginfo_ready;
}

/* Yield to codecs for as long as possible if they are in need of data
 * return true if the caller should break to let the audio thread process
 * new events */
static bool audio_yield_codecs(void)
{
    yield();
    
    if (!queue_empty(&audio_queue)) 
        return true;

    while ((pcmbuf_is_crossfade_active() || pcmbuf_is_lowdata())
            && !ci.stop_codec && playing && !audio_filebuf_is_lowdata())
    {
        sleep(1);
        if (!queue_empty(&audio_queue)) 
            return true;
    }
    
    return false;
}

/* Note that this function might yield(). */
static void audio_clear_track_entries(
    bool clear_buffered, bool clear_unbuffered,
    bool may_yield)
{
    int cur_idx = track_widx;
    int last_idx = -1;
    
    logf("Clearing tracks:%d/%d, %d", track_ridx, track_widx, clear_unbuffered);
    
    /* Loop over all tracks from write-to-read */
    while (1) 
    {
        cur_idx++;
        cur_idx &= MAX_TRACK_MASK;

        if (cur_idx == track_ridx)
            break;

        /* If the track is buffered, conditionally clear/notify,
         * otherwise clear the track if that option is selected */
        if (tracks[cur_idx].event_sent) 
        {
            if (clear_buffered) 
            {
                if (last_idx >= 0)
                {
                    /* If there is an unbuffer callback, call it, otherwise,
                     * just clear the track */
                    if (track_unbuffer_callback)
                    {
                        if (may_yield)
                            audio_yield_codecs();
                        track_unbuffer_callback(&tracks[last_idx].id3, false);
                    }
                    
                    memset(&tracks[last_idx], 0, sizeof(struct track_info));
                }
                last_idx = cur_idx;
            }
        } 
        else if (clear_unbuffered)
            memset(&tracks[cur_idx], 0, sizeof(struct track_info));
    }

    /* We clear the previous instance of a buffered track throughout
     * the above loop to facilitate 'last' detection.  Clear/notify
     * the last track here */
    if (last_idx >= 0)
    {
        if (track_unbuffer_callback)
            track_unbuffer_callback(&tracks[last_idx].id3, true);
        memset(&tracks[last_idx], 0, sizeof(struct track_info));
    }
}

/* FIXME: This code should be made more generic and move to metadata.c */
static void audio_strip_id3v1_tag(void)
{
    int i;
    static const unsigned char tag[] = "TAG";
    size_t tag_idx;
    size_t cur_idx;

    tag_idx = RINGBUF_SUB(buf_widx, 128);

    if (FILEBUFUSED > 128 && tag_idx > buf_ridx)
    {
        cur_idx = tag_idx;
        for(i = 0;i < 3;i++)
        {
            if(filebuf[cur_idx] != tag[i])
                return;

            cur_idx = RINGBUF_ADD(cur_idx, 1);
        }

        /* Skip id3v1 tag */
        logf("Skipping ID3v1 tag");
        buf_widx = tag_idx;
        tracks[track_widx].available -= 128;
        tracks[track_widx].filesize -= 128;
    }
}

static void audio_read_file(bool quick)
{
    size_t copy_n;
    int rc;

    /* If we're called and no file is open, this is an error */
    if (current_fd < 0) 
    {
        logf("Bad fd in arf");
        /* Stop this buffer cycle immediately */
        fill_bytesleft = 0;
        /* Give some hope of miraculous recovery by forcing a track reload */
        tracks[track_widx].filesize = 0;
        return ;
    }

    cpu_boost_id(true, CPUBOOSTID_PLAYBACK_AUDIO);
    while (tracks[track_widx].filerem > 0) 
    {
        int overlap;

        if (fill_bytesleft == 0)
            break ;

        /* copy_n is the largest chunk that is safe to read */
        copy_n = MIN(conf_filechunk, filebuflen - buf_widx);
        copy_n = MIN(copy_n, fill_bytesleft);

        /* rc is the actual amount read */
        rc = read(current_fd, &filebuf[buf_widx], copy_n);

        if (rc <= 0) 
        {
            /* Reached the end of the file */
            tracks[track_widx].filerem = 0;
            break ;
        }

        overlap = buf_widx + rc - CUR_TI->buf_idx;
        buf_widx = RINGBUF_ADD(buf_widx, rc);

        if (overlap > 0 && (unsigned) overlap >= filebuflen)
            overlap -= filebuflen;

        if (overlap > 0 && overlap <= rc && CUR_TI->available != 0) {
            CUR_TI->buf_idx = buf_widx;
            CUR_TI->start_pos += overlap;
        }
        
        tracks[track_widx].available += rc;
        tracks[track_widx].filerem -= rc;

        if (fill_bytesleft > (unsigned)rc)
            fill_bytesleft -= rc;
        else
            fill_bytesleft = 0;

        /* Let the codec process until it is out of the danger zone, or there
         * is an event to handle.  In the latter case, break this fill cycle
         * immediately */
        if (quick || audio_yield_codecs())
            break;
    }

    if (tracks[track_widx].filerem == 0) 
    {
        logf("Finished buf:%dB", tracks[track_widx].filesize);
        close(current_fd);
        current_fd = -1;
        audio_strip_id3v1_tag();

        track_widx++;
        track_widx &= MAX_TRACK_MASK;

        tracks[track_widx].filesize = 0;
    } 
    else 
    {
        logf("Partially buf:%dB",
                tracks[track_widx].filesize - tracks[track_widx].filerem);
    }
    cpu_boost_id(false, CPUBOOSTID_PLAYBACK_AUDIO);
}

static bool audio_loadcodec(bool start_play)
{
    size_t size;
    int fd;
    int rc;
    size_t copy_n;
    int prev_track;
    char codec_path[MAX_PATH]; /* Full path to codec */

    const char * codec_fn = get_codec_filename(tracks[track_widx].id3.codectype);
    if (codec_fn == NULL)
        return false;

    tracks[track_widx].has_codec = false;
    tracks[track_widx].codecsize = 0;

    if (start_play)
    {
        /* Load the codec directly from disk and save some memory. */
        track_ridx = track_widx;
        ci.filesize = CUR_TI->filesize;
        ci.id3 = &CUR_TI->id3;
        ci.taginfo_ready = &CUR_TI->taginfo_ready;
        ci.curpos = 0;
        LOGFQUEUE("codec > codec Q_CODEC_LOAD_DISK");
        queue_post(&codec_queue, Q_CODEC_LOAD_DISK, (void *)codec_fn);
        return true;
    }
    else
    {
        /* If we already have another track than this one buffered */
        if (track_widx != track_ridx) 
        {
            prev_track = (track_widx - 1) & MAX_TRACK_MASK;
            
            /* If the previous codec is the same as this one, there is no need
             * to put another copy of it on the file buffer */
            if (get_codec_base_type(tracks[track_widx].id3.codectype) ==
                    get_codec_base_type(tracks[prev_track].id3.codectype)
                && audio_codec_loaded)
            {
                logf("Reusing prev. codec");
                return true;
            }
        }
    }

    codec_get_full_path(codec_path, codec_fn);

    fd = open(codec_path, O_RDONLY);
    if (fd < 0)
    {
        logf("Codec doesn't exist!");
        return false;
    }

    size = filesize(fd);
    
    /* Never load a partial codec */
    if (fill_bytesleft < size) 
    {
        logf("Not enough space");
        fill_bytesleft = 0;
        close(fd);
        return false;
    }

    while (tracks[track_widx].codecsize < size) 
    {
        copy_n = MIN(conf_filechunk, filebuflen - buf_widx);
        rc = read(fd, &filebuf[buf_widx], copy_n);
        if (rc < 0)
        {
            close(fd);
            return false;
        }
        
        if (fill_bytesleft > (unsigned)rc)
            fill_bytesleft -= rc;
        else
            fill_bytesleft = 0;

        buf_widx = RINGBUF_ADD(buf_widx, rc);

        tracks[track_widx].codecsize += rc;       
    }

    tracks[track_widx].has_codec = true;

    close(fd);
    logf("Done: %dB", size);

    return true;
}

/* TODO: Copied from mpeg.c. Should be moved somewhere else. */
static void audio_set_elapsed(struct mp3entry* id3)
{
    if ( id3->vbr ) {
        if ( id3->has_toc ) {
            /* calculate elapsed time using TOC */
            int i;
            unsigned int remainder, plen, relpos, nextpos;

            /* find wich percent we're at */
            for (i=0; i<100; i++ )
                if ( id3->offset < id3->toc[i] * (id3->filesize / 256) )
                    break;

            i--;
            if (i < 0)
                i = 0;

            relpos = id3->toc[i];

            if (i < 99)
                nextpos = id3->toc[i+1];
            else
                nextpos = 256;

            remainder = id3->offset - (relpos * (id3->filesize / 256));

            /* set time for this percent (divide before multiply to prevent
               overflow on long files. loss of precision is negligible on
               short files) */
            id3->elapsed = i * (id3->length / 100);

            /* calculate remainder time */
            plen = (nextpos - relpos) * (id3->filesize / 256);
            id3->elapsed += (((remainder * 100) / plen) *
                             (id3->length / 10000));
        }
        else {
            /* no TOC exists. set a rough estimate using average bitrate */
            int tpk = id3->length / (id3->filesize / 1024);
            id3->elapsed = id3->offset / 1024 * tpk;
        }
    }
    else
    {
        /* constant bitrate, use exact calculation */
        if (id3->bitrate != 0)
            id3->elapsed = id3->offset / (id3->bitrate / 8);
    }
}

static bool audio_load_track(int offset, bool start_play, bool rebuffer)
{
    char *trackname;
    off_t size;
    char msgbuf[80];

    /* Stop buffer filling if there is no free track entries.
       Don't fill up the last track entry (we wan't to store next track
       metadata there). */
    if (!audio_have_free_tracks()) 
    {
        logf("No free tracks");
        return false;
    }

    if (current_fd >= 0)
    {
        logf("Nonzero fd in alt");
        close(current_fd);
        current_fd = -1;
    }

    last_peek_offset++;
    peek_again:
    logf("Buffering track:%d/%d", track_widx, track_ridx);
    /* Get track name from current playlist read position. */
    while ((trackname = playlist_peek(last_peek_offset)) != NULL) 
    {
        /* Handle broken playlists. */
        current_fd = open(trackname, O_RDONLY);
        if (current_fd < 0) 
        {
            logf("Open failed");
            /* Skip invalid entry from playlist. */
            playlist_skip_entry(NULL, last_peek_offset);
        } 
        else
            break;
    }

    if (!trackname) 
    {
        logf("End-of-playlist");
        playlist_end = true;
        return false;
    }

    /* Initialize track entry. */
    size = filesize(current_fd);
    tracks[track_widx].filerem = size;
    tracks[track_widx].filesize = size;
    tracks[track_widx].available = 0;

    /* Set default values */
    if (start_play) 
    {
        int last_codec = current_codec;
        
        current_codec = CODEC_IDX_AUDIO;
        conf_watermark = AUDIO_DEFAULT_WATERMARK;
        conf_filechunk = AUDIO_DEFAULT_FILECHUNK;
        conf_preseek = AUDIO_REBUFFER_GUESS_SIZE;
        dsp_configure(DSP_RESET, 0);
        current_codec = last_codec;
    }

    /* Get track metadata if we don't already have it. */
    if (!tracks[track_widx].taginfo_ready) 
    {
        if (get_metadata(&tracks[track_widx],current_fd,trackname,v1first)) 
        {
            if (start_play) 
            {
                track_changed = true;
                playlist_update_resume_info(audio_current_track());
            }
        } 
        else 
        {
            logf("mde:%s!",trackname);
            
            /* Set filesize to zero to indicate no file was loaded. */
            tracks[track_widx].filesize = 0;
            tracks[track_widx].filerem = 0;
            close(current_fd);
            current_fd = -1;
            
            /* Skip invalid entry from playlist. */
            playlist_skip_entry(NULL, last_peek_offset);
            tracks[track_widx].taginfo_ready = false;
            goto peek_again;
        }

    }

    /* Load the codec. */
    tracks[track_widx].codecbuf = &filebuf[buf_widx];
    if (!audio_loadcodec(start_play)) 
    {
        if (tracks[track_widx].codecsize)
        {
            /* Must undo the buffer write of the partial codec */
            logf("Partial codec loaded");
            fill_bytesleft += tracks[track_widx].codecsize;
            buf_widx = RINGBUF_SUB(buf_widx, tracks[track_widx].codecsize);
            tracks[track_widx].codecsize = 0;
        }

        /* Set filesize to zero to indicate no file was loaded. */
        tracks[track_widx].filesize = 0;
        tracks[track_widx].filerem = 0;
        close(current_fd);
        current_fd = -1;

        /* Try skipping to next track if there is space. */
        if (fill_bytesleft > 0) 
        {
            /* This is an error condition unless the fill_bytesleft is 0 */
            snprintf(msgbuf, sizeof(msgbuf)-1, "No codec for: %s", trackname);
            /* We should not use gui_syncplash from audio thread! */
            gui_syncsplash(HZ*2, true, msgbuf);
            /* Skip invalid entry from playlist. */
            playlist_skip_entry(NULL, last_peek_offset);
            tracks[track_widx].taginfo_ready = false;
            goto peek_again;
        }
        
        return false;
    }

    tracks[track_widx].start_pos = 0;
    set_filebuf_watermark(buffer_margin);
    tracks[track_widx].id3.elapsed = 0;

    if (offset > 0) 
    {
        switch (tracks[track_widx].id3.codectype) {
        case AFMT_MPA_L1:
        case AFMT_MPA_L2:
        case AFMT_MPA_L3:
            lseek(current_fd, offset, SEEK_SET);
            tracks[track_widx].id3.offset = offset;
            audio_set_elapsed(&tracks[track_widx].id3);
            tracks[track_widx].filerem = size - offset;
            ci.curpos = offset;
            tracks[track_widx].start_pos = offset;
            break;

        case AFMT_WAVPACK:
            lseek(current_fd, offset, SEEK_SET);
            tracks[track_widx].id3.offset = offset;
            tracks[track_widx].id3.elapsed =
                tracks[track_widx].id3.length / 2;
            tracks[track_widx].filerem = size - offset;
            ci.curpos = offset;
            tracks[track_widx].start_pos = offset;
            break;

        case AFMT_OGG_VORBIS:
        case AFMT_FLAC:
        case AFMT_PCM_WAV:
        case AFMT_A52:
        case AFMT_AAC:
            tracks[track_widx].id3.offset = offset;
            break;
        }

    }
    
    logf("alt:%s", trackname);
    tracks[track_widx].buf_idx = buf_widx;

    audio_read_file(rebuffer);

    return true;
}

static bool audio_read_next_metadata(void)
{
    int fd;
    char *trackname;
    int next_idx;
    int status;

    next_idx = track_widx;
    if (tracks[next_idx].taginfo_ready)
    {
        next_idx++;
        next_idx &= MAX_TRACK_MASK;

        if (tracks[next_idx].taginfo_ready)
            return true;
    }

    trackname = playlist_peek(last_peek_offset + 1);
    if (!trackname)
        return false;

    fd = open(trackname, O_RDONLY);
    if (fd < 0)
        return false;

    status = get_metadata(&tracks[next_idx],fd,trackname,v1first);
    /* Preload the glyphs in the tags */
    if (status) 
    {
        if (tracks[next_idx].id3.title)
            lcd_getstringsize(tracks[next_idx].id3.title, NULL, NULL);
        if (tracks[next_idx].id3.artist)
            lcd_getstringsize(tracks[next_idx].id3.artist, NULL, NULL);
        if (tracks[next_idx].id3.album)
            lcd_getstringsize(tracks[next_idx].id3.album, NULL, NULL);
    }
    close(fd);

    return status;
}

/* Send callback events to notify about new tracks. */
static void audio_generate_postbuffer_events(void)
{
    int cur_idx;
    int last_idx = -1;

    logf("Postbuffer:%d/%d",track_ridx,track_widx);

    if (audio_have_tracks()) 
    {
        cur_idx = track_ridx;
        
        while (1) {
            if (!tracks[cur_idx].event_sent) 
            {
                if (last_idx >= 0 && !tracks[last_idx].event_sent)
                {
                    /* Mark the event 'sent' even if we don't really send one */
                    tracks[last_idx].event_sent = true;
                    if (track_buffer_callback)
                        track_buffer_callback(&tracks[last_idx].id3, false);
                }
                last_idx = cur_idx;
            }
            if (cur_idx == track_widx)
                break;
            cur_idx++;
            cur_idx &= MAX_TRACK_MASK;
        }

        if (last_idx >= 0 && !tracks[last_idx].event_sent)
        {
            tracks[last_idx].event_sent = true;
            if (track_buffer_callback)
                track_buffer_callback(&tracks[last_idx].id3, true);
        }
        
        /* Force WPS reload. */
        track_changed = true;
    }
}

static bool audio_initialize_buffer_fill(bool clear_tracks)
{
    /* Don't initialize if we're already initialized */
    if (filling)
        return true;

    /* Don't start buffer fill if buffer is already full. */
    if (FILEBUFUSED > conf_watermark && !filling)
        return false;
    
    logf("Starting buffer fill");

    fill_bytesleft = filebuflen - FILEBUFUSED;
    /* TODO: This doesn't look right, and might explain some problems with
     *       seeking in large files (to offsets larger than filebuflen).
     *       And what about buffer wraps?
     * 
     * This really doesn't look right, so don't use it.
     */
    // if (buf_ridx > CUR_TI->buf_idx)
    //    CUR_TI->start_pos = buf_ridx - CUR_TI->buf_idx;

    /* Set the filling flag true before calling audio_clear_tracks as that
     * function can yield and we start looping. */
    filling = true;
    
    if (clear_tracks)
        audio_clear_track_entries(true, false, true);

    /* Save the current resume position once. */
    playlist_update_resume_info(audio_current_track());
    
    return true;
}

static void audio_fill_file_buffer(
        bool start_play, bool rebuffer, size_t offset)
{
    bool had_next_track = audio_next_track() != NULL;

    if (!audio_initialize_buffer_fill(!start_play))
        return ;

    /* If we have a partially buffered track, continue loading,
     * otherwise load a new track */
    if (tracks[track_widx].filesize > 0)
        audio_read_file(false);
    else if (!audio_load_track(offset, start_play, rebuffer))
        fill_bytesleft = 0;

    if (!had_next_track && audio_next_track())
        track_changed = true;

    /* If we're done buffering */
    if (fill_bytesleft == 0)
    {
        audio_read_next_metadata();

        audio_generate_postbuffer_events();
        filling = false;

#ifndef SIMULATOR
        if (playing)
            ata_sleep();
#endif
    }
}

static void audio_rebuffer(void)
{
    logf("Forcing rebuffer");
    
    /* Notify the codec that this will take a while */
    /* Currently this can cause some problems (logf in reverse order):
     * Codec load error:-1
     * Codec load disk
     * Codec: Unsupported
     * Codec finished
     * New codec:0/3
     * Clearing tracks:7/7, 1
     * Forcing rebuffer
     * Check new track buffer
     * Request new track
     * Clearing tracks:5/5, 0
     * Starting buffer fill
     * Clearing tracks:5/5, 1
     * Re-buffering song w/seek
     */
    //if (!filling)
    //    queue_post(&codec_callback_queue, Q_CODEC_REQUEST_PENDING, 0);
    
    /* Stop in progress fill, and clear open file descriptor */
    if (current_fd >= 0)
    {
        close(current_fd);
        current_fd = -1;
    }
    filling = false;

    /* Reset buffer and track pointers */
    CUR_TI->buf_idx = buf_ridx = buf_widx = 0;
    track_widx = track_ridx;
    audio_clear_track_entries(true, true, false);
    CUR_TI->available = 0;

    /* Fill the buffer */
    last_peek_offset = -1;
    CUR_TI->filesize = 0;
    CUR_TI->start_pos = 0;
    ci.curpos = 0;

    if (!CUR_TI->taginfo_ready)
        memset(&CUR_TI->id3, 0, sizeof(struct mp3entry));

    audio_fill_file_buffer(false, true, 0);
}

static void audio_check_new_track(void)
{
    int track_count = audio_track_count();
    int old_track_ridx = track_ridx;
    bool forward;

    if (dir_skip)
    {
        dir_skip = false;
        if (playlist_next_dir(ci.new_track))
        {
            ci.new_track = 0;
            CUR_TI->taginfo_ready = false;
            audio_rebuffer();
            goto skip_done;
        }
        else
        {
            LOGFQUEUE("audio > codec Q_CODEC_REQUEST_FAILED");
            queue_post(&codec_callback_queue, Q_CODEC_REQUEST_FAILED, 0);
            return;
        }
    }

    if (new_playlist)
        ci.new_track = 0;

    /* If the playlist isn't that big */
    if (!playlist_check(ci.new_track))
    {
        if (ci.new_track >= 0)
        {
            LOGFQUEUE("audio > codec Q_CODEC_REQUEST_FAILED");
            queue_post(&codec_callback_queue, Q_CODEC_REQUEST_FAILED, 0);
            return;
        }
        /* Find the beginning backward if the user over-skips it */
        while (!playlist_check(++ci.new_track))
            if (ci.new_track >= 0)
            {
                LOGFQUEUE("audio > codec Q_CODEC_REQUEST_FAILED");
                queue_post(&codec_callback_queue, Q_CODEC_REQUEST_FAILED, 0);
                return;
            }
    }
    /* Update the playlist */
    last_peek_offset -= ci.new_track;

    if (playlist_next(ci.new_track) < 0)
    {
        LOGFQUEUE("audio > codec Q_CODEC_REQUEST_FAILED");
        queue_post(&codec_callback_queue, Q_CODEC_REQUEST_FAILED, 0);
        return;
    }

    if (new_playlist)
    {
        ci.new_track = 1;
        new_playlist = false;
    }

    /* Save the old track */
    prev_ti = CUR_TI;

    /* Move to the new track */
    track_ridx += ci.new_track;
    track_ridx &= MAX_TRACK_MASK;

    if (automatic_skip)
        playlist_end = false;

    track_changed = !automatic_skip;

    /* If it is not safe to even skip this many track entries */
    if (ci.new_track >= track_count || ci.new_track <= track_count - MAX_TRACK)
    {
        ci.new_track = 0;
        CUR_TI->taginfo_ready = false;
        audio_rebuffer();
        goto skip_done;
    }

    forward = ci.new_track > 0;
    ci.new_track = 0;

    /* If the target track is clearly not in memory */
    if (CUR_TI->filesize == 0 || !CUR_TI->taginfo_ready)
    {
        audio_rebuffer();
        goto skip_done;
    }

    /* The track may be in memory, see if it really is */
    if (forward)
    {
        if (!audio_buffer_wind_forward(track_ridx, old_track_ridx))
            audio_rebuffer();
    }
    else
    {
        int cur_idx = track_ridx;
        bool taginfo_ready = true;
        bool wrap = track_ridx > old_track_ridx;
        
        while (1) 
        {
            cur_idx++;
            cur_idx &= MAX_TRACK_MASK;
            if (!(wrap || cur_idx < old_track_ridx))
                break;

            /* If we hit a track in between without valid tag info, bail */
            if (!tracks[cur_idx].taginfo_ready)
            {
                taginfo_ready = false;
                break;
            }

            tracks[cur_idx].available = tracks[cur_idx].filesize;
            if (tracks[cur_idx].codecsize)
                tracks[cur_idx].has_codec = true;
        }
        if (taginfo_ready)
        {
            if (!audio_buffer_wind_backward(track_ridx, old_track_ridx))
                audio_rebuffer();
        }
        else
        {
            CUR_TI->taginfo_ready = false;
            audio_rebuffer();
        }
    }

skip_done:
    audio_update_trackinfo();
    LOGFQUEUE("audio > codec Q_CODEC_REQUEST_COMPLETE");
    queue_post(&codec_callback_queue, Q_CODEC_REQUEST_COMPLETE, 0);
}

static void audio_rebuffer_and_seek(size_t newpos)
{
    int fd;
    char *trackname;

    trackname = playlist_peek(0);
    /* (Re-)open current track's file handle. */

    fd = open(trackname, O_RDONLY);
    if (fd < 0) 
    {
        LOGFQUEUE("audio > codec Q_CODEC_REQUEST_FAILED");
        queue_post(&codec_callback_queue, Q_CODEC_REQUEST_FAILED, 0);
        return;
    }
    
    if (current_fd >= 0)
        close(current_fd);
    current_fd = fd;

    playlist_end = false;

    ci.curpos = newpos;

    /* Clear codec buffer. */
    track_widx = track_ridx;
    tracks[track_widx].buf_idx = buf_widx = buf_ridx = 0;

    last_peek_offset = 0;
    filling = false;
    audio_initialize_buffer_fill(true);
    filling = true;

    if (newpos > conf_preseek) {
        buf_ridx = RINGBUF_ADD(buf_ridx, conf_preseek);
        CUR_TI->start_pos = newpos - conf_preseek;
    } 
    else 
    {
        buf_ridx = RINGBUF_ADD(buf_ridx, newpos);
        CUR_TI->start_pos = 0;
    }

    CUR_TI->filerem = CUR_TI->filesize - CUR_TI->start_pos;
    CUR_TI->available = 0;

    lseek(current_fd, CUR_TI->start_pos, SEEK_SET);

    LOGFQUEUE("audio > codec Q_CODEC_REQUEST_COMPLETE");
    queue_post(&codec_callback_queue, Q_CODEC_REQUEST_COMPLETE, 0);
}

void audio_set_track_buffer_event(void (*handler)(struct mp3entry *id3,
                                                  bool last_track))
{
    track_buffer_callback = handler;
}

void audio_set_track_unbuffer_event(void (*handler)(struct mp3entry *id3,
                                                    bool last_track))
{
    track_unbuffer_callback = handler;
}

void audio_set_track_changed_event(void (*handler)(struct mp3entry *id3))
{
    track_changed_callback = handler;
}

static void audio_stop_codec_flush(void)
{
    ci.stop_codec = true;
    pcmbuf_pause(true);
    while (audio_codec_loaded)
        yield();
    /* If the audio codec is not loaded any more, and the audio is still
     * playing, it is now and _only_ now safe to call this function from the
     * audio thread */
    if (pcm_is_playing())
        pcmbuf_play_stop();
    pcmbuf_pause(paused);
}

static void audio_stop_playback(void)
{
    /* If we were playing, save resume information */
    if (playing)
    {
        /* Save the current playing spot, or NULL if the playlist has ended */
        playlist_update_resume_info(
            (playlist_end && ci.stop_codec)?NULL:audio_current_track());
    }

    playing = false;
    filling = false;
    paused = false;
    audio_stop_codec_flush();
    
    if (current_fd >= 0) 
    {
        close(current_fd);
        current_fd = -1;
    }

    /* Mark all entries null. */
    audio_clear_track_entries(true, false, false);
    memset(tracks, 0, sizeof(struct track_info) * MAX_TRACK);
}

static void audio_play_start(size_t offset)
{
#if defined(HAVE_RECORDING) || defined(CONFIG_TUNER)
    rec_set_source(AUDIO_SRC_PLAYBACK, SRCF_PLAYBACK);
#endif

    /* Wait for any previously playing audio to flush - TODO: Not necessary? */
    audio_stop_codec_flush();

    track_changed = true;
    playlist_end = false;

    playing = true;
    ci.new_track = 0;
    ci.seek_time = 0;
    wps_offset = 0;
    
    if (current_fd >= 0) 
    {
        close(current_fd);
        current_fd = -1;
    }

    sound_set_volume(global_settings.volume);
    track_widx = track_ridx = 0;
    buf_ridx = buf_widx = 0;

    /* Mark all entries null. */
    memset(tracks, 0, sizeof(struct track_info) * MAX_TRACK);
    
    last_peek_offset = -1;

    audio_fill_file_buffer(true, false, offset);
}


/* Invalidates all but currently playing track. */
void audio_invalidate_tracks(void)
{
    if (audio_have_tracks()) {
        last_peek_offset = 0;

        playlist_end = false;
        track_widx = track_ridx;
        audio_clear_track_entries(true, true, true);

        /* If the current track is fully buffered, advance the write pointer */
        if (tracks[track_widx].filerem == 0)
            track_widx = (track_widx + 1) & MAX_TRACK_MASK;

        /* Mark all other entries null (also buffered wrong metadata). */
        buf_widx = RINGBUF_ADD(buf_ridx, CUR_TI->available);

        audio_read_next_metadata();
    }
}

static void audio_new_playlist(void)
{
    /* Prepare to start a new fill from the beginning of the playlist */
    last_peek_offset = -1;
    if (audio_have_tracks()) {
        playlist_end = false;
        track_widx = track_ridx;
        audio_clear_track_entries(true, true, true);

        track_widx++;
        track_widx &= MAX_TRACK_MASK;

        /* Stop reading the current track */
        CUR_TI->filerem = 0;
        close(current_fd);
        current_fd = -1;

        /* Mark the current track as invalid to prevent skipping back to it */
        CUR_TI->taginfo_ready = false;

        /* Invalidate the buffer other than the playing track */
        buf_widx = RINGBUF_ADD(buf_ridx, CUR_TI->available);
    }
    
    /* Signal the codec to initiate a track change forward */
    new_playlist = true;
    ci.new_track = 1;
    audio_fill_file_buffer(false, true, 0);
}

static void audio_initiate_track_change(long direction)
{
    playlist_end = false;
    ci.new_track += direction;
    wps_offset -= direction;
}

static void audio_initiate_dir_change(long direction)
{
    playlist_end = false;
    dir_skip = true;
    ci.new_track = direction;
}

static void audio_reset_buffer(void)
{
    size_t offset;

    /* Set up file buffer as all space available */
    filebuf = (char *)&audiobuf[talk_get_bufsize()+MALLOC_BUFSIZE];
    filebuflen = audiobufend - (unsigned char *) filebuf - GUARD_BUFSIZE - 
        (pcmbuf_get_bufsize() + get_pcmbuf_descsize() + PCMBUF_MIX_CHUNK * 2);

    /* Allow for codec(s) at end of file buffer */
    if (talk_voice_required())
    {
        /* Allow 2 codecs at end of file buffer */
        filebuflen -= 2 * (CODEC_IRAM_SIZE + CODEC_SIZE);

        iram_buf[0] = &filebuf[filebuflen];
        iram_buf[1] = &filebuf[filebuflen+CODEC_IRAM_SIZE];
        dram_buf[0] = (unsigned char *)&filebuf[filebuflen+CODEC_IRAM_SIZE*2];
        dram_buf[1] = (unsigned char *)&filebuf[filebuflen+CODEC_IRAM_SIZE*2+CODEC_SIZE];
    }
    else
    {
        /* Allow for 1 codec at end of file buffer */
        filebuflen -= CODEC_IRAM_SIZE + CODEC_SIZE;

        iram_buf[0] = &filebuf[filebuflen];
        iram_buf[1] = NULL;
        dram_buf[0] = (unsigned char *)&filebuf[filebuflen+CODEC_IRAM_SIZE];
        dram_buf[1] = NULL;
    }

    /* Ensure that file buffer is aligned */
    offset = (-(size_t)filebuf) & 3;
    filebuf += offset;
    filebuflen -= offset;
    filebuflen &= ~3;
}


#ifdef ROCKBOX_HAS_LOGF
static void audio_test_track_changed_event(struct mp3entry *id3)
{
    (void)id3;

    logf("tce:%s", id3->path);
}
#endif

static void audio_playback_init(void)
{
#ifdef PLAYBACK_VOICE
    static bool voicetagtrue = true;
    static struct mp3entry id3_voice;
#endif
    struct event ev;

    logf("playback api init");
    pcm_init();

#if defined(HAVE_RECORDING) && !defined(SIMULATOR)
    /* Set the input multiplexer to Line In */
    pcm_rec_mux(0);
#endif

#ifdef ROCKBOX_HAS_LOGF
    audio_set_track_changed_event(audio_test_track_changed_event);
#endif

    /* Initialize codec api. */
    ci.read_filebuf = codec_filebuf_callback;
    ci.pcmbuf_insert = codec_pcmbuf_insert_callback;
    ci.pcmbuf_insert_split = codec_pcmbuf_insert_split_callback;
    ci.get_codec_memory = codec_get_memory_callback;
    ci.request_buffer = codec_request_buffer_callback;
    ci.advance_buffer = codec_advance_buffer_callback;
    ci.advance_buffer_loc = codec_advance_buffer_loc_callback;
    ci.request_next_track = codec_request_next_track_callback;
    ci.mp3_get_filepos = codec_mp3_get_filepos_callback;
    ci.seek_buffer = codec_seek_buffer_callback;
    ci.seek_complete = codec_seek_complete_callback;
    ci.set_elapsed = codec_set_elapsed_callback;
    ci.set_offset = codec_set_offset_callback;
    ci.configure = codec_configure_callback;
    ci.discard_codec = codec_discard_codec_callback;

    /* Initialize voice codec api. */
#ifdef PLAYBACK_VOICE
    memcpy(&ci_voice, &ci, sizeof(struct codec_api));
    memset(&id3_voice, 0, sizeof(struct mp3entry));
    ci_voice.read_filebuf = voice_filebuf_callback;
    ci_voice.pcmbuf_insert = voice_pcmbuf_insert_callback;
    ci_voice.pcmbuf_insert_split = voice_pcmbuf_insert_split_callback;
    ci_voice.get_codec_memory = voice_get_memory_callback;
    ci_voice.request_buffer = voice_request_buffer_callback;
    ci_voice.advance_buffer = voice_advance_buffer_callback;
    ci_voice.advance_buffer_loc = voice_advance_buffer_loc_callback;
    ci_voice.request_next_track = voice_request_next_track_callback;
    ci_voice.mp3_get_filepos = voice_mp3_get_filepos_callback;
    ci_voice.seek_buffer = voice_seek_buffer_callback;
    ci_voice.seek_complete = voice_do_nothing;
    ci_voice.set_elapsed = voice_set_elapsed_callback;
    ci_voice.set_offset = voice_set_offset_callback;
    ci_voice.discard_codec = voice_do_nothing;
    ci_voice.taginfo_ready = &voicetagtrue;
    ci_voice.id3 = &id3_voice;
    id3_voice.frequency = 11200;
    id3_voice.length = 1000000L;
#endif

    codec_thread_p = create_thread(codec_thread, codec_stack, 
                                   sizeof(codec_stack),
                                   codec_thread_name IF_PRIO(, PRIORITY_PLAYBACK));

    while (1)
    {
        queue_wait(&audio_queue, &ev);
        if (ev.id == Q_AUDIO_POSTINIT)
            break ;

#ifndef SIMULATOR
        if (ev.id == SYS_USB_CONNECTED)
        {
            logf("USB: Audio preinit");
            usb_acknowledge(SYS_USB_CONNECTED_ACK);
            usb_wait_for_disconnect(&audio_queue);
        }
#endif
    }

    filebuf = (char *)&audiobuf[MALLOC_BUFSIZE]; /* Will be reset by reset_buffer */

    audio_set_crossfade(global_settings.crossfade);

    audio_is_initialized = true;

    sound_settings_apply();
}

static void audio_thread(void)
{
    struct event ev;

    /* At first initialize audio system in background. */
    audio_playback_init();

    while (1) 
    {
        if (filling)
        {
            queue_wait_w_tmo(&audio_queue, &ev, 0);
            if (ev.id == SYS_TIMEOUT)
                ev.id = Q_AUDIO_FILL_BUFFER;
        }
        else
            queue_wait_w_tmo(&audio_queue, &ev, HZ/2);

        switch (ev.id) {
            case Q_AUDIO_FILL_BUFFER:
                LOGFQUEUE("audio < Q_AUDIO_FILL_BUFFER");
                if (!filling)
                    if (!playing || playlist_end || ci.stop_codec)
                        break;
                audio_fill_file_buffer(false, false, 0);
                break;

            case Q_AUDIO_PLAY:
                LOGFQUEUE("audio < Q_AUDIO_PLAY");
                audio_clear_track_entries(true, false, true);
                audio_play_start((size_t)ev.data);
                break ;

            case Q_AUDIO_STOP:
                LOGFQUEUE("audio < Q_AUDIO_STOP");
                audio_stop_playback();
                break ;

            case Q_AUDIO_PAUSE:
                LOGFQUEUE("audio < Q_AUDIO_PAUSE");
                pcmbuf_pause((bool)ev.data);
                paused = (bool)ev.data;
                break ;

            case Q_AUDIO_SKIP:
                LOGFQUEUE("audio < Q_AUDIO_SKIP");
                audio_initiate_track_change((long)ev.data);
                break;

            case Q_AUDIO_PRE_FF_REWIND:
                LOGFQUEUE("audio < Q_AUDIO_PRE_FF_REWIND");
                if (!playing)
                    break;
                pcmbuf_pause(true);
                break;

            case Q_AUDIO_FF_REWIND:
                LOGFQUEUE("audio < Q_AUDIO_FF_REWIND");
                if (!playing)
                    break ;
                ci.seek_time = (long)ev.data+1;
                break ;

            case Q_AUDIO_REBUFFER_SEEK:
                LOGFQUEUE("audio < Q_AUDIO_REBUFFER_SEEK");
                audio_rebuffer_and_seek((size_t)ev.data);
                break;

            case Q_AUDIO_CHECK_NEW_TRACK:
                LOGFQUEUE("audio < Q_AUDIO_CHECK_NEW_TRACK");
                audio_check_new_track();
                break;

            case Q_AUDIO_DIR_SKIP:
                LOGFQUEUE("audio < Q_AUDIO_DIR_SKIP");
                playlist_end = false;
                audio_initiate_dir_change((long)ev.data);
                break;

            case Q_AUDIO_NEW_PLAYLIST:
                LOGFQUEUE("audio < Q_AUDIO_NEW_PLAYLIST");
                audio_new_playlist();
                break;

            case Q_AUDIO_FLUSH:
                LOGFQUEUE("audio < Q_AUDIO_FLUSH");
                audio_invalidate_tracks();
                break ;

            case Q_AUDIO_TRACK_CHANGED:
                LOGFQUEUE("audio < Q_AUDIO_TRACK_CHANGED");
                if (track_changed_callback)
                    track_changed_callback(&CUR_TI->id3);
                track_changed = true;
                playlist_update_resume_info(audio_current_track());
                break ;

#ifndef SIMULATOR
            case SYS_USB_CONNECTED:
                LOGFQUEUE("audio < SYS_USB_CONNECTED");
                audio_stop_playback();
                usb_acknowledge(SYS_USB_CONNECTED_ACK);
                usb_wait_for_disconnect(&audio_queue);
                break ;
#endif

            case SYS_TIMEOUT:
                LOGFQUEUE("audio < SYS_TIMEOUT");
                break;

            default:
                LOGFQUEUE("audio < default");
        }
    }
}

