/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef W202_AUDIO_H
#define W202_AUDIO_H

#include <linux/device.h>
#include <linux/types.h>

#define W202_DSP_CHANNEL_VBC	0
#define W202_DSP_CMD_STARTUP	4
#define W202_DSP_CMD_SHUTDOWN	5
#define W202_DSP_CMD_HW_PARAMS	6
#define W202_DSP_CMD_TRIGGER	7
#define W202_DSP_CMD_CONTROL	9
#define W202_DSP_CMD_PROFILE	11
#define W202_DSP_STARTUP_BYTES	0x26c
#define W202_DSP_PROFILE_BYTES	0x6c4
#define W202_PCM_FRAGMENT	80

/* Layouts verified against the factory Qogirn6pro modules with REA. */
struct w202_dsp_message {
	__le16 command;
	__le16 channel;
	__le32 parameter[4];
};

struct w202_dsp_stream {
	u8 name[32];
	__le32 scene;
	__le32 stream;
};

struct w202_dsp_pair {
	__le32 id;
	__le32 value;
};

struct w202_dsp_startup {
	struct w202_dsp_stream stream;
	__le32 dac_id;
	__le32 adc_id;
	__le32 ref_adc_id;
	struct w202_dsp_pair adc_source[4];
	struct w202_dsp_pair dac_out[2];
	struct w202_dsp_pair mux_tx[3];
	struct w202_dsp_pair mux_rx[4];
	struct w202_dsp_pair iis_do[5];
	struct w202_dsp_pair tx_width[3];
	struct w202_dsp_pair tx_lr[3];
	struct w202_dsp_pair rx_width[4];
	struct w202_dsp_pair rx_lr[4];
	__le32 loopback[4];
	__le32 master[2];
	__le32 mute_dg[4][4];
	__le32 smooth_dg[6];
	__le32 mixer_dg[2][8];
	__le32 mixer_dg_step;
	__le32 mixer[5][3];
	__le32 sbc[7];
	__le32 ivsense[2];
	struct w202_dsp_pair master_select[4];
	__le16 voice_record_type;
	__le16 reserved;
};

struct w202_audio_dsp;
struct w202_audio_dsp *w202_audio_dsp_get(struct device *consumer);
/*
 * stream is the factory aud_send_cmd parameter[1]: SNDRV_PCM_STREAM_PLAYBACK (0)
 * or SNDRV_PCM_STREAM_CAPTURE (1) of the normal AP01 scene (parameter[0] = 0).
 */
int w202_audio_dsp_command(struct w202_audio_dsp *dsp, u16 command, u32 stream,
			   const void *data, size_t size);
int w202_audio_dsp_trigger(struct w202_audio_dsp *dsp, u32 stream);
int w202_audio_dsp_control(struct w202_audio_dsp *dsp, u32 id,
			    const void *data, size_t size);
int w202_audio_dsp_profile(struct w202_audio_dsp *dsp, u32 mode,
			    const void *data, size_t size);
bool w202_audio_dsp_ready(struct w202_audio_dsp *dsp);

/* Private interface between the VBC DAI and PCM component, not userspace ABI. */
struct w202_pcm_dma_params {
	dma_addr_t fifo[2];
};

#endif
