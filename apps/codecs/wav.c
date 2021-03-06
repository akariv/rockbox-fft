/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 * $Id$
 *
 * Copyright (C) 2005 Dave Chapman
 * Copyright (C) 2009 Yoshihisa Uchida
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 ****************************************************************************/

#include "codeclib.h"
#include "inttypes.h"
#include "codecs/libpcm/support_formats.h"

CODEC_HEADER

/* WAVE (RIFF) codec:
 * 
 *  For a good documentation on WAVE files, see:
 *  http://www.tsp.ece.mcgill.ca/MMSP/Documents/AudioFormats/WAVE/WAVE.html
 *  and
 *  http://www.sonicspot.com/guide/wavefiles.html
 *
 *  For sample WAV files, see:
 *  http://www.tsp.ece.mcgill.ca/MMSP/Documents/AudioFormats/WAVE/Samples.html
 *
 */

static int32_t samples[PCM_CHUNK_SIZE] IBSS_ATTR;

/* This codec support WAVE files with the following formats: */
enum
{
    WAVE_FORMAT_UNKNOWN = 0x0000, /* Microsoft Unknown Wave Format */
    WAVE_FORMAT_PCM = 0x0001,   /* Microsoft PCM Format */
    WAVE_FORMAT_IEEE_FLOAT = 0x0003, /* IEEE Float */
    WAVE_FORMAT_ALAW = 0x0006,  /* Microsoft ALAW */
    WAVE_FORMAT_MULAW = 0x0007, /* Microsoft MULAW */
    WAVE_FORMAT_DVI_ADPCM = 0x0011, /* Intel's DVI ADPCM */
    IBM_FORMAT_MULAW = 0x0101,  /* same as WAVE_FORMAT_MULAW */
    IBM_FORMAT_ALAW = 0x0102,   /* same as WAVE_FORMAT_ALAW */
    WAVE_FORMAT_EXTENSIBLE = 0xFFFE
};

const struct pcm_entry wave_codecs[] = {
    { WAVE_FORMAT_UNKNOWN,            0                            },
    { WAVE_FORMAT_PCM,                get_linear_pcm_codec         },
    { WAVE_FORMAT_IEEE_FLOAT,         get_ieee_float_codec         },
    { WAVE_FORMAT_ALAW,               get_itut_g711_alaw_codec     },
    { WAVE_FORMAT_MULAW,              get_itut_g711_mulaw_codec    },
    { WAVE_FORMAT_DVI_ADPCM,          get_dvi_adpcm_codec          },
    { IBM_FORMAT_MULAW,               get_itut_g711_mulaw_codec    },
    { IBM_FORMAT_ALAW,                get_itut_g711_alaw_codec     },
};

#define NUM_FORMATS 8

static const struct pcm_codec *get_wave_codec(uint32_t formattag)
{
    int i;

    for (i = 0; i < NUM_FORMATS; i++)
    {
        if (wave_codecs[i].format_tag == formattag)
        {
            if (wave_codecs[i].get_codec)
                return wave_codecs[i].get_codec();
            return 0;
        }
    }
    return 0;
}


/* this is the codec entry point */
enum codec_status codec_main(void)
{
    int status = CODEC_OK;
    struct pcm_format format;
    uint32_t bytesdone, decodedbytes;
    uint32_t i;
    size_t n;
    int bufcount;
    int endofstream;
    unsigned char *buf;
    uint8_t *wavbuf;
    off_t firstblockposn;     /* position of the first block in file */
    const struct pcm_codec *codec;

    /* Generic codec initialisation */
    ci->configure(DSP_SET_SAMPLE_DEPTH, 28);
  
next_track:
    if (codec_init()) {
        DEBUGF("codec_init() error\n");
        status = CODEC_ERROR;
        goto exit;
    }

    while (!*ci->taginfo_ready && !ci->stop_codec)
        ci->sleep(1);

    codec_set_replaygain(ci->id3);
    
    /* Need to save offset for later use (cleared indirectly by advance_buffer) */
    bytesdone = ci->id3->offset;

    /* get RIFF chunk header */
    buf = ci->request_buffer(&n, 12);
    if (n < 12) {
        DEBUGF("request_buffer error\n");
        status = CODEC_ERROR;
        goto done;
    }
    if ((memcmp(buf, "RIFF", 4) != 0) || (memcmp(&buf[8], "WAVE", 4) != 0)) {
        status = CODEC_ERROR;
        goto done;
    }

    /* advance to first WAVE chunk */
    ci->advance_buffer(12);

    firstblockposn = 12;
    ci->memset(&format, 0, sizeof(struct pcm_format));
    format.is_signed = true;
    format.is_little_endian = true;

    decodedbytes = 0;
    codec = 0;

    /* iterate over WAVE chunks until the 'data' chunk, which should be after the 'fmt ' chunk */
    while (true) {
        /* get WAVE chunk header */
        buf = ci->request_buffer(&n, 1024);
        if (n < 8) {
            DEBUGF("data chunk request_buffer error\n");
            /* no more chunks, 'data' chunk must not have been found */
            status = CODEC_ERROR;
            goto done;
        }

        /* chunkSize */
        i = (buf[4]|(buf[5]<<8)|(buf[6]<<16)|(buf[7]<<24));
        if (memcmp(buf, "fmt ", 4) == 0) {
            if (i < 16) {
                DEBUGF("CODEC_ERROR: 'fmt ' chunk size=%lu < 16\n",
                       (unsigned long)i);
                status = CODEC_ERROR;
                goto done;
            }
            /* wFormatTag */
            format.formattag=buf[8]|(buf[9]<<8);
            /* wChannels */
            format.channels=buf[10]|(buf[11]<<8);
            /* skipping dwSamplesPerSec */
            /* dwAvgBytesPerSec */
            format.avgbytespersec = buf[16]|(buf[17]<<8)|(buf[18]<<16)|(buf[19]<<24);
            /* wBlockAlign */
            format.blockalign=buf[20]|(buf[21]<<8);
            /* wBitsPerSample */
            format.bitspersample=buf[22]|(buf[23]<<8);
            if (format.formattag != WAVE_FORMAT_PCM) {
                if (i < 18) {
                    /* this is not a fatal error with some formats,
                     * we'll see later if we can't decode it */
                    DEBUGF("CODEC_WARNING: non-PCM WAVE (formattag=0x%x) "
                           "doesn't have ext. fmt descr (chunksize=%d<18).\n",
                           (unsigned int)format.formattag, (int)i);
                }
                else
                {
                    format.size = buf[24]|(buf[25]<<8);
                    if (format.formattag != WAVE_FORMAT_EXTENSIBLE)
                        format.samplesperblock = buf[26]|(buf[27]<<8);
                    else {
                        if (format.size < 22) {
                            DEBUGF("CODEC_ERROR: WAVE_FORMAT_EXTENSIBLE is "
                                   "missing extension\n");
                            status = CODEC_ERROR;
                            goto done;
                        }
                        /* wValidBitsPerSample */
                        format.bitspersample = buf[26]|(buf[27]<<8);
                        /* skipping dwChannelMask (4bytes) */
                        /* SubFormat (only get the first two bytes) */
                        format.formattag = buf[32]|(buf[33]<<8);
                    }
                }
            }

            /* get codec */
            codec = get_wave_codec(format.formattag);
            if (!codec)
            {
                DEBUGF("CODEC_ERROR: unsupported wave format %x\n", 
                    (unsigned int) format.formattag);
                status = CODEC_ERROR;
                goto done;
            }

            /* riff 8bit linear pcm is unsigned */
            if (format.formattag == WAVE_FORMAT_PCM && format.bitspersample == 8)
                format.is_signed = false;

            /* set format, parse codec specific tag, check format, and calculate chunk size */
            if (!codec->set_format(&format, buf))
            {
                status = CODEC_ERROR;
                goto done;
            }
        } else if (memcmp(buf, "data", 4) == 0) {
            format.numbytes = i;
            /* advance to start of data */
            ci->advance_buffer(8);
            firstblockposn += 8;
            break;
        } else if (memcmp(buf, "fact", 4) == 0) {
            /* dwSampleLength */
            if (i >= 4)
                format.totalsamples =
                             (buf[8]|(buf[9]<<8)|(buf[10]<<16)|(buf[11]<<24));
        } else {
            DEBUGF("unknown WAVE chunk: '%c%c%c%c', size=%lu\n",
                   buf[0], buf[1], buf[2], buf[3], (unsigned long)i);
        }

        /* go to next chunk (even chunk sizes must be padded) */
        if (i & 0x01)
            i++;
        ci->advance_buffer(i+8);
        firstblockposn += i + 8;
    }

    if (!codec)
    {
        DEBUGF("CODEC_ERROR: 'fmt ' chunk not found\n");
        status = CODEC_ERROR;
        goto done;
    }

    /* common format check */
    if (format.channels == 0) {
        DEBUGF("CODEC_ERROR: 'fmt ' chunk not found or 0-channels file\n");
        status = CODEC_ERROR;
        goto done;
    }
    if (format.numbytes == 0) {
        DEBUGF("CODEC_ERROR: 'data' chunk not found or has zero-length\n");
        status = CODEC_ERROR;
        goto done;
    }

    ci->configure(DSP_SWITCH_FREQUENCY, ci->id3->frequency);
    if (format.channels == 2) {
        ci->configure(DSP_SET_STEREO_MODE, STEREO_INTERLEAVED);
    } else if (format.channels == 1) {
        ci->configure(DSP_SET_STEREO_MODE, STEREO_MONO);
    } else {
        DEBUGF("CODEC_ERROR: more than 2 channels\n");
        status = CODEC_ERROR;
        goto done;
    }

    /* make sure we're at the correct offset */
    if (bytesdone > (uint32_t) firstblockposn) {
        /* Round down to previous block */
        uint32_t offset = bytesdone - bytesdone % format.blockalign;

        ci->advance_buffer(offset-firstblockposn);
        bytesdone = offset - firstblockposn;
    } else {
        /* already where we need to be */
        bytesdone = 0;
    }

    /* The main decoder loop */
    endofstream = 0;

    while (!endofstream) {
        ci->yield();
        if (ci->stop_codec || ci->new_track) {
            break;
        }

        if (ci->seek_time) {
            uint32_t newpos = codec->get_seek_pos(ci->seek_time);

            if (newpos > format.numbytes)
                break;
            if (ci->seek_buffer(firstblockposn + newpos))
            {
                bytesdone = newpos;
            }
            ci->seek_complete();
        }

        wavbuf = (uint8_t *)ci->request_buffer(&n, format.chunksize);
        if (n == 0)
            break; /* End of stream */
        if (bytesdone + n > format.numbytes) {
            n = format.numbytes - bytesdone;
            endofstream = 1;
        }

        status = codec->decode(wavbuf, n, samples, &bufcount);
        if (status == CODEC_ERROR)
        {
            DEBUGF("codec error\n");
            goto done;
        }

        ci->pcmbuf_insert(samples, NULL, bufcount);
        ci->advance_buffer(n);
        bytesdone += n;
        decodedbytes += bufcount;

        if (bytesdone >= format.numbytes)
            endofstream = 1;
        ci->set_elapsed(decodedbytes*1000LL/ci->id3->frequency);
    }
    status = CODEC_OK;

done:
    if (ci->request_next_track())
        goto next_track;

exit:
    return status;
}
