/* Copyright (c) 2008-2009, Code Aurora Forum. All rights reserved.
 *
 * All source code in this file is licensed under the following license except
 * where indicated.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published
 * by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *
 * See the GNU General Public License for more details.
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, you can find it at http://www.fsf.org.
 */


#include <mach/debug_audio_mm.h>
#include <linux/init.h>
#include <linux/err.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/time.h>
#include <linux/wait.h>
#include <linux/platform_device.h>
#include <sound/core.h>
#include <sound/soc.h>
#include <sound/soc-dapm.h>
#include <sound/pcm.h>
#include <sound/initval.h>
#include <sound/control.h>
#include <asm/dma.h>
#include <linux/dma-mapping.h>
#include <linux/android_pmem.h>

#include "msm7kv2-pcm.h"

#define HOSTPCM_STREAM_ID 5

struct snd_msm {
	struct snd_card *card;
	struct snd_pcm *pcm;
};

int copy_count;

static struct snd_pcm_hardware msm_pcm_playback_hardware = {
	.info =                 SNDRV_PCM_INFO_INTERLEAVED,
	.formats =              USE_FORMATS,
	.rates =                USE_RATE,
	.rate_min =             USE_RATE_MIN,
	.rate_max =             48000,
	.channels_min =         1,
	.channels_max =         1,
	.buffer_bytes_max =     MAX_BUFFER_PLAYBACK_SIZE,
	.period_bytes_min =     BUFSZ,
	.period_bytes_max =     BUFSZ,
	.periods_min =          2,
	.periods_max =          2,
	.fifo_size =            0,
};

static struct snd_pcm_hardware msm_pcm_capture_hardware = {
	.info =                 SNDRV_PCM_INFO_INTERLEAVED,
	.formats =              USE_FORMATS,
	.rates =                USE_RATE,
	.rate_min =             8000,
	.rate_max =             8000,
	.channels_min =         1,
	.channels_max =         1,
	.buffer_bytes_max =     MAX_BUFFER_CAPTURE_SIZE,
	.period_bytes_min =    	4096,
	.period_bytes_max =     4096,
	.periods_min =          4,
	.periods_max =          4,
	.fifo_size =            0,
};

/* Conventional and unconventional sample rate supported */
static unsigned int supported_sample_rates[] = {
	8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000
};

static struct snd_pcm_hw_constraint_list constraints_sample_rates = {
	.count = ARRAY_SIZE(supported_sample_rates),
	.list = supported_sample_rates,
	.mask = 0,
};

static void event_handler(void *data)
{
	struct msm_audio *prtd = data;
	MM_DBG("\n");
	snd_pcm_period_elapsed(prtd->substream);
}

static int msm_pcm_playback_prepare(struct snd_pcm_substream *substream)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct msm_audio *prtd = runtime->private_data;

	MM_DBG("\n");
	prtd->pcm_size = snd_pcm_lib_buffer_bytes(substream);
	prtd->pcm_count = snd_pcm_lib_period_bytes(substream);
	prtd->pcm_irq_pos = 0;
	prtd->pcm_buf_pos = 0;
	if (prtd->enabled)
		return 0;

	MM_DBG("\n");
	/* rate and channels are sent to audio driver */
	prtd->out_sample_rate = runtime->rate;
	prtd->out_channel_mode = runtime->channels;
	prtd->data = prtd->substream->dma_buffer.area;
	prtd->phys = prtd->substream->dma_buffer.addr;
	prtd->out[0].data = prtd->data + 0;
	prtd->out[0].addr = prtd->phys + 0;
	prtd->out[0].size = BUFSZ;
	prtd->out[1].data = prtd->data + BUFSZ;
	prtd->out[1].addr = prtd->phys + BUFSZ;
	prtd->out[1].size = BUFSZ;

	return 0;
}

static int msm_pcm_capture_prepare(struct snd_pcm_substream *substream)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct msm_audio *prtd = runtime->private_data;
	int ret = 0;

	MM_DBG("\n");
	prtd->pcm_size = snd_pcm_lib_buffer_bytes(substream);
	prtd->pcm_count = snd_pcm_lib_period_bytes(substream);
	prtd->pcm_irq_pos = 0;
	prtd->pcm_buf_pos = 0;

	/* rate and channels are sent to audio driver */
	prtd->type = ENC_TYPE_WAV;
	prtd->source = INTERNAL_CODEC_TX_SOURCE_MIX_MASK;
	prtd->samp_rate = runtime->rate;
	prtd->channel_mode = (runtime->channels - 1);
	prtd->buffer_size = prtd->channel_mode ? STEREO_DATA_SIZE : \
							MONO_DATA_SIZE;

	if (prtd->enabled)
		return 0;

	prtd->data = prtd->substream->dma_buffer.area;
	prtd->phys = prtd->substream->dma_buffer.addr;
	MM_DBG("prtd->data =%08x\n", (unsigned int)prtd->data);
	MM_DBG("prtd->phys =%08x\n", (unsigned int)prtd->phys);

	mutex_lock(&the_locks.lock);
	ret = alsa_audio_configure(prtd);
	mutex_unlock(&the_locks.lock);
	if (ret)
		return ret;
	ret = wait_event_interruptible(the_locks.enable_wait,
				prtd->running != 0);
	MM_DBG("state prtd->running = %d ret = %d\n", prtd->running, ret);

	if (prtd->running == 0)
		ret = -ENODEV;
	else
		ret = 0;
	prtd->enabled = 1;

	return ret;
}

static int msm_pcm_trigger(struct snd_pcm_substream *substream, int cmd)
{
	int ret = 0;

	MM_DBG("\n");
	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
		break;
	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_SUSPEND:
	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
		break;
	default:
		ret = -EINVAL;
	}

	return ret;
}

struct  msm_audio_event_callbacks snd_msm_audio_ops = {
	.playback = event_handler,
	.capture = event_handler,
};

static int msm_pcm_open(struct snd_pcm_substream *substream)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct msm_audio *prtd;
	int ret = 0;
	int i = 0;
	int session_attrb, sessionid;

	MM_DBG("\n");
	prtd = kzalloc(sizeof(struct msm_audio), GFP_KERNEL);
	if (prtd == NULL) {
		ret = -ENOMEM;
		return ret;
	}
	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
		runtime->hw = msm_pcm_playback_hardware;
		prtd->dir = SNDRV_PCM_STREAM_PLAYBACK;
		prtd->eos_ack = 0;
		prtd->session_id = HOSTPCM_STREAM_ID;
	} else if (substream->stream == SNDRV_PCM_STREAM_CAPTURE) {
		runtime->hw = msm_pcm_capture_hardware;
		prtd->dir = SNDRV_PCM_STREAM_CAPTURE;
		session_attrb = ENC_TYPE_WAV;
		sessionid = audpreproc_aenc_alloc(session_attrb,
				&prtd->module_name, &prtd->queue_id);
		if (sessionid < 0) {
			MM_ERR("No free decoder available\n");
			kfree(prtd);
			return -ENODEV;
		}
		prtd->session_id = sessionid;
		MM_DBG("%s\n", prtd->module_name);
		ret = msm_adsp_get(prtd->module_name, &prtd->audrec,
				&alsa_audrec_adsp_ops, prtd);
		if (ret < 0) {
			audpreproc_aenc_free(prtd->session_id);
			kfree(prtd);
			return -ENODEV;
		}
	}
	prtd->substream = substream;
	ret = snd_pcm_hw_constraint_list(runtime, 0,
						SNDRV_PCM_HW_PARAM_RATE,
						&constraints_sample_rates);
	if (ret < 0)
		MM_ERR("snd_pcm_hw_constraint_list failed\n");
	/* Ensure that buffer size is a multiple of period size */
	ret = snd_pcm_hw_constraint_integer(runtime,
					    SNDRV_PCM_HW_PARAM_PERIODS);
	if (ret < 0)
		MM_ERR("snd_pcm_hw_constraint_integer failed\n");

	prtd->ops = &snd_msm_audio_ops;
	prtd->out[0].used = BUF_INVALID_LEN;
	prtd->out[1].used = 0;
	prtd->out_head = 1; /* point to second buffer on startup */
	prtd->out_tail = 0;
	prtd->dsp_cnt = 0;
	prtd->in_head = 0;
	prtd->in_tail = 0;
	prtd->in_count = 0;
	prtd->out_needed = 0;
	for (i = 0; i < FRAME_NUM; i++) {
		prtd->in[i].size = 0;
		prtd->in[i].read = 0;
	}
	prtd->vol_pan.volume = 0x2000;
	prtd->vol_pan.pan = 0x0;
	runtime->private_data = prtd;

	copy_count = 0;
	return 0;

}

static int msm_pcm_playback_copy(struct snd_pcm_substream *substream, int a,
	snd_pcm_uframes_t hwoff, void __user *buf, snd_pcm_uframes_t frames)
{
	int ret = 0;
	int fbytes = 0;

	struct snd_pcm_runtime *runtime = substream->runtime;
	struct msm_audio *prtd = runtime->private_data;

	MM_DBG("%d\n", fbytes);
	fbytes = frames_to_bytes(runtime, frames);
	ret = alsa_send_buffer(prtd, buf, fbytes, NULL);
	++copy_count;
	prtd->pcm_buf_pos += fbytes;
	if (copy_count == 1) {
		mutex_lock(&the_locks.lock);
		ret = alsa_audio_configure(prtd);
		mutex_unlock(&the_locks.lock);
	}
	return  ret;
}

static int msm_pcm_playback_close(struct snd_pcm_substream *substream)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct msm_audio *prtd = runtime->private_data;

	int ret = 0;

	MM_DBG("\n");

	/* pcm dmamiss message is sent continously
	 * when decoder is starved so no race
	 * condition concern
	 */
	if (prtd->enabled)
		ret = wait_event_interruptible(the_locks.eos_wait,
					prtd->eos_ack);

	alsa_audio_disable(prtd);
	audpp_adec_free(prtd->session_id);
	kfree(prtd);

	return 0;
}

static int msm_pcm_capture_copy(struct snd_pcm_substream *substream,
		 int channel, snd_pcm_uframes_t hwoff, void __user *buf,
						 snd_pcm_uframes_t frames)
{
	int ret = 0, rc1 = 0, rc2 = 0;
	int fbytes = 0;
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct msm_audio *prtd = substream->runtime->private_data;

	int monofbytes = 0;
	char *bufferp = NULL;

	MM_DBG("%d\n", fbytes);
	fbytes = frames_to_bytes(runtime, frames);
	monofbytes = fbytes / 2;
	if (runtime->channels == 2) {
		ret = alsa_buffer_read(prtd, buf, fbytes, NULL);
	} else {
		bufferp = buf;
		rc1 = alsa_buffer_read(prtd, bufferp, monofbytes, NULL);
		bufferp = buf + monofbytes ;
		rc2 = alsa_buffer_read(prtd, bufferp, monofbytes, NULL);
		ret = rc1 + rc2;
	}
	prtd->pcm_buf_pos += fbytes;
	return ret;
}

static int msm_pcm_capture_close(struct snd_pcm_substream *substream)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct msm_audio *prtd = runtime->private_data;

	MM_DBG("\n");
	wake_up(&the_locks.enable_wait);
	alsa_audrec_disable(prtd);
	audpreproc_aenc_free(prtd->session_id);
	msm_adsp_put(prtd->audrec);
	kfree(prtd);
	return 0;
}

static int msm_pcm_copy(struct snd_pcm_substream *substream, int a,
	 snd_pcm_uframes_t hwoff, void __user *buf, snd_pcm_uframes_t frames)
{
	int ret = 0;

	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK)
		ret = msm_pcm_playback_copy(substream, a, hwoff, buf, frames);
	else if (substream->stream == SNDRV_PCM_STREAM_CAPTURE)
		ret = msm_pcm_capture_copy(substream, a, hwoff, buf, frames);
	return ret;
}

static int msm_pcm_close(struct snd_pcm_substream *substream)
{
	int ret = 0;

	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK)
		ret = msm_pcm_playback_close(substream);
	else if (substream->stream == SNDRV_PCM_STREAM_CAPTURE)
		ret = msm_pcm_capture_close(substream);
	return ret;
}
static int msm_pcm_prepare(struct snd_pcm_substream *substream)
{
	int ret = 0;

	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK)
		ret = msm_pcm_playback_prepare(substream);
	else if (substream->stream == SNDRV_PCM_STREAM_CAPTURE)
		ret = msm_pcm_capture_prepare(substream);
	return ret;
}

static snd_pcm_uframes_t msm_pcm_pointer(struct snd_pcm_substream *substream)
{

	struct snd_pcm_runtime *runtime = substream->runtime;
	struct msm_audio *prtd = runtime->private_data;

	MM_DBG("\n");
	if (prtd->pcm_irq_pos == prtd->pcm_size)
		prtd->pcm_irq_pos = 0;
	return bytes_to_frames(runtime, (prtd->pcm_irq_pos));
}

int msm_pcm_hw_params(struct snd_pcm_substream *substream,
				struct snd_pcm_hw_params *params)
{
	struct snd_pcm_runtime *runtime = substream->runtime;

	if (substream->pcm->device & 1) {
		runtime->hw.info &= ~SNDRV_PCM_INFO_INTERLEAVED;
		runtime->hw.info |= SNDRV_PCM_INFO_NONINTERLEAVED;
	}
	return 0;
}

static struct snd_pcm_ops msm_pcm_ops = {
	.open           = msm_pcm_open,
	.copy		= msm_pcm_copy,
	.hw_params	= msm_pcm_hw_params,
	.close          = msm_pcm_close,
	.ioctl          = snd_pcm_lib_ioctl,
	.prepare        = msm_pcm_prepare,
	.trigger        = msm_pcm_trigger,
	.pointer        = msm_pcm_pointer,
};



static int msm_pcm_remove(struct platform_device *devptr)
{
	struct snd_soc_device *socdev = platform_get_drvdata(devptr);
	snd_soc_free_pcms(socdev);
	kfree(socdev->codec);
	platform_set_drvdata(devptr, NULL);
	return 0;
}

static int pcm_preallocate_buffer(struct snd_pcm *pcm,
	int stream)
{
	struct snd_pcm_substream *substream = pcm->streams[stream].substream;
	struct snd_dma_buffer *buf = &substream->dma_buffer;
	size_t size;
	if (!stream)
		size = PLAYBACK_DMASZ;
	else
		size = CAPTURE_DMASZ;

	buf->dev.type = SNDRV_DMA_TYPE_DEV;
	buf->dev.dev = pcm->card->dev;
	buf->private_data = NULL;
	buf->area = dma_alloc_coherent(pcm->card->dev, size,
					   &buf->addr, GFP_KERNEL);
	if (!buf->area)
		return -ENOMEM;

	buf->bytes = size;
	return 0;
}

static void msm_pcm_free_buffers(struct snd_pcm *pcm)
{
	struct snd_pcm_substream *substream;
	struct snd_dma_buffer *buf;
	int stream;

	for (stream = 0; stream < 2; stream++) {
		substream = pcm->streams[stream].substream;
		if (!substream)
			continue;

		buf = &substream->dma_buffer;
		if (!buf->area)
			continue;

		dma_free_coherent(pcm->card->dev, buf->bytes,
				      buf->area, buf->addr);
		buf->area = NULL;
	}
}

static int msm_pcm_new(struct snd_card *card,
			struct snd_soc_dai *codec_dai,
			struct snd_pcm *pcm)
{
	int ret = 0;
	if (!card->dev->coherent_dma_mask)
		card->dev->coherent_dma_mask = DMA_32BIT_MASK;

	if (codec_dai->playback.channels_min) {
		ret = pcm_preallocate_buffer(pcm,
			SNDRV_PCM_STREAM_PLAYBACK);
		if (ret)
			return ret;
	}
	if (codec_dai->playback.channels_min) {
		ret = pcm_preallocate_buffer(pcm,
			SNDRV_PCM_STREAM_CAPTURE);
		if (ret)
			msm_pcm_free_buffers(pcm);
	}

	return ret;
}

struct snd_soc_platform msm_soc_platform = {
	.name		= "msm-audio",
	.remove         = msm_pcm_remove,
	.pcm_ops 	= &msm_pcm_ops,
	.pcm_new	= msm_pcm_new,
	.pcm_free	= msm_pcm_free_buffers,
};
EXPORT_SYMBOL(msm_soc_platform);

static int __init msm_soc_platform_init(void)
{
	return snd_soc_register_platform(&msm_soc_platform);
}
module_init(msm_soc_platform_init);

static void __exit msm_soc_platform_exit(void)
{
	snd_soc_unregister_platform(&msm_soc_platform);
}
module_exit(msm_soc_platform_exit);

MODULE_DESCRIPTION("PCM module platform driver");
MODULE_LICENSE("GPL v2");