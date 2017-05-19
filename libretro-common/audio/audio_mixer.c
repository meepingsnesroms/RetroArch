/* Copyright  (C) 2010-2017 The RetroArch team
 *
 * ---------------------------------------------------------------------------------------
 * The following license statement only applies to this file (audio_mixer.c).
 * ---------------------------------------------------------------------------------------
 *
 * Permission is hereby granted, free of charge,
 * to any person obtaining a copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation the rights to
 * use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software,
 * and to permit persons to whom the Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include <audio/audio_mixer.h>
#include <audio/audio_resampler.h>

#include <streams/file_stream.h>
#include <formats/rwav.h>
#include <memalign.h>

#include <stdlib.h>
#include <string.h>
#include <math.h>

#ifdef HAVE_CONFIG_H
#include "../../config.h"
#endif

#ifdef HAVE_STB_VORBIS
#define STB_VORBIS_NO_PUSHDATA_API
#define STB_VORBIS_NO_STDIO
#define STB_VORBIS_NO_CRT

#include "../../deps/stb/stb_vorbis.h"
#endif

#define AUDIO_MIXER_MAX_VOICES      8
#define AUDIO_MIXER_TEMP_OGG_BUFFER 8192

enum audio_mixer_type
{
   AUDIO_MIXER_TYPE_NONE = 0,
   AUDIO_MIXER_TYPE_WAV,
   AUDIO_MIXER_TYPE_OGG
};

struct audio_mixer_sound
{
   enum audio_mixer_type type;
   
   union
   {
      struct
      {
         /* wav */
         unsigned frames;
         const float* pcm;
      } wav;
      
#ifdef HAVE_STB_VORBIS
      struct
      {
         /* ogg */
         unsigned size;
         const void* data;
      } ogg;
#endif
   } types;
};

struct audio_mixer_voice
{
   bool     repeat;
   unsigned type;
   float    volume;
   audio_mixer_sound_t *sound;
   audio_mixer_stop_cb_t stop_cb;
   
   union
   {
      struct
      {
         /* wav */
         unsigned position;
      } wav;
      
#ifdef HAVE_STB_VORBIS
      struct
      {
         /* ogg */
         unsigned    position;
         unsigned    samples;
         unsigned    buf_samples;
         float*      buffer;
         float       ratio;
         stb_vorbis *stream;
         void       *resampler_data;
         const retro_resampler_t *resampler;
      } ogg;
#endif
   } types;
};

static struct audio_mixer_voice s_voices[AUDIO_MIXER_MAX_VOICES];
static unsigned s_rate = 0;

static bool wav2float(const rwav_t* wav, float** pcm, size_t samples_out)
{
   size_t i;
   /* Allocate on a 16-byte boundary, and pad to a multiple of 16 bytes */
   float *f           = (float*)memalign_alloc(16,
         ((samples_out + 15) & ~15) * sizeof(float));
   
   if (!f)
      return false;
   
   *pcm = f;
   
   if (wav->bitspersample == 8)
   {
      float sample      = 0.0f;
      const uint8_t *u8 = (const uint8_t*)wav->samples;

      if (wav->numchannels == 1)
      {
         for (i = wav->numsamples; i != 0; i--)
         {
            sample = (float)*u8++ / 255.0f;
            sample = sample * 2.0f - 1.0f;
            *f++   = sample;
            *f++   = sample;
         }
      }
      else if (wav->numchannels == 2)
      {
         for (i = wav->numsamples; i != 0; i--)
         {
            sample = (float)*u8++ / 255.0f;
            sample = sample * 2.0f - 1.0f;
            *f++   = sample;
            sample = (float)*u8++ / 255.0f;
            sample = sample * 2.0f - 1.0f;
            *f++   = sample;
         }
      }
   }
   else
   {
      /* TODO/FIXME note to leiradel - can we use audio/conversion/s16_to_float
       * functions here? */

      float sample       = 0.0f;
      const int16_t *s16 = (const int16_t*)wav->samples;

      if (wav->numchannels == 1)
      {
         for (i = wav->numsamples; i != 0; i--)
         {
            sample = (float)((int)*s16++ + 32768) / 65535.0f;
            sample = sample * 2.0f - 1.0f;
            *f++   = sample;
            *f++   = sample;
         }
      }
      else if (wav->numchannels == 2)
      {
         for (i = wav->numsamples; i != 0; i--)
         {
            sample = (float)((int)*s16++ + 32768) / 65535.0f;
            sample = sample * 2.0f - 1.0f;
            *f++   = sample;
            sample = (float)((int)*s16++ + 32768) / 65535.0f;
            sample = sample * 2.0f - 1.0f;
            *f++   = sample;
         }
      }
   }

   return true;
}

static bool one_shot_resample(const float* in, size_t samples_in,
      unsigned rate, float** out, size_t* samples_out)
{
   struct resampler_data info;
   void* data                         = NULL;
   const retro_resampler_t* resampler = NULL;
   float ratio                        = (double)s_rate / (double)rate;

   if (!retro_resampler_realloc(&data, &resampler, NULL, ratio))
      return false;
   
   /*
    * Allocate on a 16-byte boundary, and pad to a multiple of 16 bytes. We
    * add four more samples in the formula below just as safeguard, because
    * resampler->process sometimes reports more output samples than the
    * formula below calculates. Ideally, audio resamplers should have a
    * function to return the number of samples they will output given a
    * count of input samples.
    */
   *samples_out                       = samples_in * ratio + 4;
   *out                               = (float*)memalign_alloc(16,
         ((*samples_out + 15) & ~15) * sizeof(float));
   
   if (*out == NULL)
      return false;

   info.data_in                       = in;
   info.data_out                      = *out;
   info.input_frames                  = samples_in / 2;
   info.output_frames                 = 0;
   info.ratio                         = ratio;

   resampler->process(data, &info);
   resampler->free(data);
   return true;
}

void audio_mixer_init(unsigned rate)
{
   unsigned i;
   
   s_rate = rate;
   
   for (i = 0; i < AUDIO_MIXER_MAX_VOICES; i++)
      s_voices[i].type = AUDIO_MIXER_TYPE_NONE;
}

void audio_mixer_done(void)
{
   unsigned i;
   
   for (i = 0; i < AUDIO_MIXER_MAX_VOICES; i++)
      s_voices[i].type = AUDIO_MIXER_TYPE_NONE;
}

audio_mixer_sound_t* audio_mixer_load_wav(const char* path)
{
   /* WAV data */
   rwav_t wav;
   /* Raw WAV bytes */
   void* buffer               = NULL;
   ssize_t size               = 0;
   /* WAV samples converted to float */
   float* pcm                 = NULL;
   size_t samples             = 0;
   /* Result */
   audio_mixer_sound_t* sound = NULL;
   enum rwav_state rwav_ret   = RWAV_ITERATE_ERROR;
   
   if (filestream_read_file(path, &buffer, &size) == 0)
      return NULL;
   
   rwav_ret = rwav_load(&wav, buffer, size);

   free(buffer);

   if (rwav_ret != RWAV_ITERATE_DONE)
      return NULL;
   
   samples       = wav.numsamples * 2;
   if (!wav2float(&wav, &pcm, samples))
      return NULL;
   
   if (wav.samplerate != s_rate)
   {
      float* resampled           = NULL;

      if (!one_shot_resample(pcm, samples,
               wav.samplerate, &resampled, &samples))
         return NULL;
      
      memalign_free((void*)pcm);
      pcm = resampled;
   }
   
   sound = (audio_mixer_sound_t*)malloc(sizeof(audio_mixer_sound_t));
   
   if (!sound)
   {
      memalign_free((void*)pcm);
      return NULL;
   }
   
   sound->type             = AUDIO_MIXER_TYPE_WAV;
   sound->types.wav.frames = (unsigned)(samples / 2);
   sound->types.wav.pcm    = pcm;
   
   rwav_free(&wav);

   return sound;
}

audio_mixer_sound_t* audio_mixer_load_ogg(const char* path)
{
#ifdef HAVE_STB_VORBIS
   ssize_t size;
   void* buffer               = NULL;
   audio_mixer_sound_t* sound = NULL;
   
   if (filestream_read_file(path, &buffer, &size) == 0)
      return NULL;
   
   sound = (audio_mixer_sound_t*)malloc(sizeof(audio_mixer_sound_t));
   
   if (!sound)
   {
      free(buffer);
      return NULL;
   }
   
   sound->type           = AUDIO_MIXER_TYPE_OGG;
   sound->types.ogg.size = size;
   sound->types.ogg.data = buffer;
   
   return sound;
#else
   return NULL;
#endif
}

void audio_mixer_destroy(audio_mixer_sound_t* sound)
{
   if (!sound)
      return;

   switch (sound->type)
   {
      case AUDIO_MIXER_TYPE_WAV:
         memalign_free((void*)sound->types.wav.pcm);
         break;
      case AUDIO_MIXER_TYPE_OGG:
#ifdef HAVE_STB_VORBIS
         memalign_free((void*)sound->types.ogg.data);
#endif
         break;
      case AUDIO_MIXER_TYPE_NONE:
         break;
   }
   
   free(sound);
}

static bool audio_mixer_play_wav(audio_mixer_sound_t* sound,
      audio_mixer_voice_t* voice, bool repeat, float volume,
      audio_mixer_stop_cb_t stop_cb)
{
   voice->types.wav.position = 0;
   
   return true;
}

#ifdef HAVE_STB_VORBIS
static bool audio_mixer_play_ogg(
      audio_mixer_sound_t* sound,
      audio_mixer_voice_t* voice,
      bool repeat, float volume,
      audio_mixer_stop_cb_t stop_cb)
{
   stb_vorbis_info info;
   int res                         = 0;
   float ratio                     = 0.0f;
   unsigned samples                = 0;
   void *ogg_buffer                = NULL;
   void *resampler_data            = NULL;
   const retro_resampler_t* resamp = NULL;
   stb_vorbis *stb_vorbis          = stb_vorbis_open_memory(
         (const unsigned char*)sound->types.ogg.data,
         sound->types.ogg.size, &res, NULL);

   if (!stb_vorbis)
      return false;

   info                    = stb_vorbis_get_info(stb_vorbis);
   
   /* Only stereo supported for now */
   if (info.channels != 2)
      goto error;
   
   if (info.sample_rate != s_rate)
   {
      ratio = (double)s_rate / (double)info.sample_rate;
      
      if (!retro_resampler_realloc(&resampler_data,
               &resamp, NULL, ratio))
         goto error;
   }

   samples                         = (unsigned)(AUDIO_MIXER_TEMP_OGG_BUFFER * ratio);
   ogg_buffer                      = (float*)memalign_alloc(16,
         ((samples + 15) & ~15) * sizeof(float));

   if (!ogg_buffer)
   {
      resamp->free(resampler_data);
      goto error;
   }

   voice->types.ogg.resampler      = resamp;
   voice->types.ogg.resampler_data = resampler_data;
   voice->types.ogg.buffer         = ogg_buffer;
   voice->types.ogg.buf_samples    = samples;
   voice->types.ogg.ratio          = ratio;
   voice->types.ogg.stream         = stb_vorbis;
   voice->types.ogg.position       = 0;
   voice->types.ogg.samples        = 0;
   voice->type                     = AUDIO_MIXER_TYPE_OGG;

   return true;

error:
   stb_vorbis_close(stb_vorbis);
   return false;
}
#endif

audio_mixer_voice_t* audio_mixer_play(audio_mixer_sound_t* sound, bool repeat,
      float volume, audio_mixer_stop_cb_t stop_cb)
{
   unsigned i;
   bool res                   = false;
   audio_mixer_voice_t* voice = s_voices;

   if (!sound)
      return NULL;

   for (i = 0; i < AUDIO_MIXER_MAX_VOICES; i++, voice++)
   {
      if (voice->type != AUDIO_MIXER_TYPE_NONE)
         continue;

      switch (sound->type)
      {
         case AUDIO_MIXER_TYPE_WAV:
            res = audio_mixer_play_wav(sound, voice, repeat, volume, stop_cb);
            break;
         case AUDIO_MIXER_TYPE_OGG:
#ifdef HAVE_STB_VORBIS
            res = audio_mixer_play_ogg(sound, voice, repeat, volume, stop_cb);
#endif
            break;
         case AUDIO_MIXER_TYPE_NONE:
            break;
      }

      break;
   }

   if (!res)
      return NULL;

   voice->type     = sound->type;
   voice->repeat   = repeat;
   voice->volume   = volume;
   voice->sound    = sound;
   voice->stop_cb  = stop_cb;

   return voice;
}

void audio_mixer_stop(audio_mixer_voice_t* voice)
{
   if (voice && voice->stop_cb)
      voice->stop_cb(voice, AUDIO_MIXER_SOUND_STOPPED);
}

static void audio_mixer_mix_wav(float* buffer, size_t num_frames,
      audio_mixer_voice_t* voice,
      float volume)
{
   int i;
   unsigned buf_free                = (unsigned)(num_frames * 2);
   const audio_mixer_sound_t* sound = voice->sound;
   unsigned pcm_available           = sound->types.wav.frames 
      * 2 - voice->types.wav.position;
   const float* pcm                 = sound->types.wav.pcm + 
      voice->types.wav.position;
   
again:
   if (pcm_available < buf_free)
   {
      for (i = pcm_available; i != 0; i--)
         *buffer++ += *pcm++ * volume;

      if (voice->repeat)
      {
         if (voice->stop_cb)
            voice->stop_cb(voice, AUDIO_MIXER_SOUND_REPEATED);

         buf_free                  -= pcm_available;
         pcm_available              = sound->types.wav.frames * 2;
         pcm                        = sound->types.wav.pcm;
         voice->types.wav.position  = 0;
         goto again;
      }

      if (voice->stop_cb)
         voice->stop_cb(voice, AUDIO_MIXER_SOUND_FINISHED);

      voice->type = AUDIO_MIXER_TYPE_NONE;
   }
   else
   {
      for (i = buf_free; i != 0; i--)
         *buffer++ += *pcm++ * volume;

      voice->types.wav.position += buf_free;
   }
}

#ifdef HAVE_STB_VORBIS
static void audio_mixer_mix_ogg(float* buffer, size_t num_frames,
      audio_mixer_voice_t* voice,
      float volume)
{
   int i;
   struct resampler_data info;
   float temp_buffer[AUDIO_MIXER_TEMP_OGG_BUFFER];
   unsigned buf_free                = num_frames * 2;
   unsigned temp_samples            = 0;
   float* pcm                       = NULL;
   
   if (voice->types.ogg.position == voice->types.ogg.samples)
   {
again:
      temp_samples = stb_vorbis_get_samples_float_interleaved(
            voice->types.ogg.stream, 2, temp_buffer,
            AUDIO_MIXER_TEMP_OGG_BUFFER) * 2;

      if (temp_samples == 0)
      {
         if (voice->repeat)
         {
            if (voice->stop_cb)
               voice->stop_cb(voice, AUDIO_MIXER_SOUND_REPEATED);

            stb_vorbis_seek_start(voice->types.ogg.stream);
            goto again;
         }
         else
         {
            if (voice->stop_cb)
               voice->stop_cb(voice, AUDIO_MIXER_SOUND_FINISHED);

            voice->type = AUDIO_MIXER_TYPE_NONE;
            return;
         }
      }

      info.data_in              = temp_buffer;
      info.data_out             = voice->types.ogg.buffer;
      info.input_frames         = temp_samples / 2;
      info.output_frames        = 0;
      info.ratio                = voice->types.ogg.ratio;

      voice->types.ogg.resampler->process(voice->types.ogg.resampler_data, &info);
      voice->types.ogg.position = 0;
      voice->types.ogg.samples  = voice->types.ogg.buf_samples;
   }

   pcm = voice->types.ogg.buffer + voice->types.ogg.position;

   if (voice->types.ogg.samples < buf_free)
   {
      for (i = voice->types.ogg.samples; i != 0; i--)
         *buffer++ += *pcm++ * volume;

      buf_free -= voice->types.ogg.samples;
      goto again;
   }
   else
   {
      int i;
      for (i = buf_free; i != 0; --i )
         *buffer++ += *pcm++ * volume;

      voice->types.ogg.position += buf_free;
      voice->types.ogg.samples  -= buf_free;
   }
}
#endif

void audio_mixer_mix(float* buffer, size_t num_frames)
{
   unsigned i;
   size_t j                   = 0;
   float* sample              = NULL;
   audio_mixer_voice_t* voice = s_voices;
   
   for (i = 0; i < AUDIO_MIXER_MAX_VOICES; i++, voice++)
   {
      float volume = voice->volume;

      switch (voice->type)
      {
         case AUDIO_MIXER_TYPE_WAV:
            audio_mixer_mix_wav(buffer, num_frames, voice, volume);
            break;
         case AUDIO_MIXER_TYPE_OGG:
#ifdef HAVE_STB_VORBIS
            audio_mixer_mix_ogg(buffer, num_frames, voice, volume);
#endif
            break;
         case AUDIO_MIXER_TYPE_NONE:
            break;
      }
   }
   
   for (j = 0, sample = buffer; j < num_frames; j++, sample++)
   {
      if (*sample < -1.0f)
         *sample = -1.0f;
      else if (*sample > 1.0f)
         *sample = 1.0f;
   }
}
