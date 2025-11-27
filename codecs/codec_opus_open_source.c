/*
 * Asterisk -- An open source telephony toolkit.
 *
 * Copyright (C) 2014, Lorenzo Miniero
 * Copyright (C) 2023-2025, Amical
 *
 * Lorenzo Miniero <lorenzo@meetecho.com>
 * Additional modifications by Yoann Vanitou <yvanitou@amical-ai.com>
 * - Added support for codecs.conf configuration
 * - Added packet loss estimation parameter
 * - Added configuration reload capability
 * - Enhanced CLI output
 *
 * See http://www.asterisk.org for more information about
 * the Asterisk project. Please do not directly contact
 * any of the maintainers of this project for assistance;
 * the project provides a web site, mailing lists and IRC
 * channels for your use.
 *
 * This program is free software, distributed under the terms of
 * the GNU General Public License Version 2. See the LICENSE file
 * at the top of the source tree.
 */

/*! \file
 *
 * \brief Translate between signed linear and Opus (Open Codec)
 *
 * \author Lorenzo Miniero <lorenzo@meetecho.com>
 *
 * \note This work was motivated by Mozilla
 *
 * \ingroup codecs
 *
 * \extref http://www.opus-codec.org/docs/html_api-1.1.0/
 *
 */

/*** MODULEINFO
	 <depend>opus</depend>
	 <conflict>codec_opus</conflict>
	 <defaultenabled>yes</defaultenabled>
***/

#include "asterisk.h"

#if defined(ASTERISK_REGISTER_FILE)
ASTERISK_REGISTER_FILE()
#elif defined(ASTERISK_FILE_VERSION)
ASTERISK_FILE_VERSION(__FILE__, "$Revision: $")
#endif

#include "asterisk/astobj2.h"	  /* for ao2_ref */
#include "asterisk/cli.h"		  /* for ast_cli_entry, ast_cli, etc */
#include "asterisk/codec.h"		  /* for ast_codec_get */
#include "asterisk/format.h"	  /* for ast_format_get_attribute_data */
#include "asterisk/frame.h"		  /* for ast_frame, etc */
#include "asterisk/linkedlists.h" /* for AST_LIST_NEXT, etc */
#include "asterisk/lock.h"		  /* for ast_atomic_fetchadd_int */
#include "asterisk/logger.h"	  /* for ast_log, LOG_ERROR, etc */
#include "asterisk/module.h"
#include "asterisk/translate.h" /* for ast_trans_pvt, etc */
#include "asterisk/utils.h"		/* for ARRAY_LEN */
#include "asterisk/config.h"	/* for ast_config_load and ast_variable_browse */

#include <opus/opus.h>

#include "asterisk/opus.h" /* for CODEC_OPUS_DEFAULT_* */

#define BUFFER_SAMPLES 5760
#define MAX_CHANNELS 2
#define OPUS_SAMPLES 960

/* Configuration structure for thread-safe access */
struct opus_config {
	int complexity;           /* Computational complexity (0-10) */
	opus_int32 bitrate;       /* "auto" = 510000 */
	int fec;                  /* Forward Error Correction (0 or 1) */
	int dtx;                  /* Discontinuous Transmission (0 or 1) */
	int cbr;                  /* Constant Bit Rate (0=VBR, 1=CBR) */
	int max_playback_rate;    /* Max sampling rate */
	int packet_loss;          /* Packet loss percentage (-1=disabled, 0-100) */
	char max_bandwidth[16];   /* narrow, medium, wide, super_wide, full */
	char signal_type[8];      /* auto, voice, music */
	char application[12];     /* voip, audio, low_delay */
};

/* Global configuration with default values */
static struct opus_config config = {
	.complexity = 10,
	.bitrate = CODEC_OPUS_DEFAULT_BITRATE,
	.fec = CODEC_OPUS_DEFAULT_FEC,
	.dtx = CODEC_OPUS_DEFAULT_DTX,
	.cbr = CODEC_OPUS_DEFAULT_CBR,
	.max_playback_rate = CODEC_OPUS_DEFAULT_MAX_PLAYBACK_RATE,
	.packet_loss = -1,
	.max_bandwidth = "full",
	.signal_type = "auto",
	.application = "voip"
};

/* RWLock to protect configuration access during reload */
static ast_rwlock_t config_lock;

/* Sample frame data */
#include "asterisk/slin.h"
#include "ex_opus.h"

static struct codec_usage
{
	int encoder_id;
	int decoder_id;
	int encoders;
	int decoders;
} usage;

/*
 * Stores the function pointer 'sample_count' of the cached ast_codec
 * before this module was loaded. Allows to restore this previous
 * function pointer, when this module in unloaded.
 */
static struct ast_codec *opus_codec; /* codec of the cached format */
static int (*opus_samples_previous)(struct ast_frame *frame);

/* Private structures */
struct opus_coder_pvt
{
	void *opus; /* May be encoder or decoder */
	int sampling_rate;
	int multiplier;
	int id;
	int16_t buf[BUFFER_SAMPLES];
	int framesize;
	int inited;
	int channels;
	int decode_fec_incoming;
	int previous_lost;
};

struct opus_attr
{
	unsigned int maxbitrate;
	unsigned int maxplayrate;
	unsigned int unused; /* was minptime */
	unsigned int stereo;
	unsigned int cbr;
	unsigned int fec;
	unsigned int dtx;
	unsigned int spropmaxcapturerate; /* FIXME: not utilised, yet */
	unsigned int spropstereo;		  /* FIXME: currently, we are just mono */
};

/* Helper methods */
static int opus_encoder_construct(struct ast_trans_pvt *pvt, int sampling_rate)
{
	struct opus_coder_pvt *opvt = pvt->pvt;
	struct opus_attr *attr = pvt->explicit_dst ? ast_format_get_attribute_data(pvt->explicit_dst) : NULL;
	struct opus_config local_config;
	opus_int32 bitrate_val;
	int playrate;
	int channels;
	opus_int32 vbr;
	opus_int32 use_fec;
	opus_int32 use_dtx;
	int app;
	int status = 0;

	/* Take a snapshot of the configuration under read lock */
	ast_rwlock_rdlock(&config_lock);
	local_config = config;
	ast_rwlock_unlock(&config_lock);

	/* Use SDP attributes if available, otherwise use local config */
	bitrate_val = attr ? attr->maxbitrate : local_config.bitrate;
	playrate = attr ? attr->maxplayrate : local_config.max_playback_rate;
	channels = attr ? attr->stereo + 1 : CODEC_OPUS_DEFAULT_STEREO + 1;
	vbr = attr ? !(attr->cbr) : !local_config.cbr;
	use_fec = attr ? attr->fec : local_config.fec;
	use_dtx = attr ? attr->dtx : local_config.dtx;

	/* Set application type based on configuration */
	if (!strcasecmp(local_config.application, "audio"))
	{
		app = OPUS_APPLICATION_AUDIO;
	}
	else if (!strcasecmp(local_config.application, "low_delay"))
	{
		app = OPUS_APPLICATION_RESTRICTED_LOWDELAY;
	}
	else
	{
		app = OPUS_APPLICATION_VOIP;
	}

	opvt->opus = opus_encoder_create(sampling_rate, channels, app, &status);

	if (status != OPUS_OK)
	{
		ast_log(LOG_ERROR, "Error creating the Opus encoder: %s\n", opus_strerror(status));
		return -1;
	}

	/* Priority to explicit max_bandwidth option */
	if (!strcasecmp(local_config.max_bandwidth, "narrow"))
	{
		status = opus_encoder_ctl(opvt->opus, OPUS_SET_MAX_BANDWIDTH(OPUS_BANDWIDTH_NARROWBAND));
	}
	else if (!strcasecmp(local_config.max_bandwidth, "medium"))
	{
		status = opus_encoder_ctl(opvt->opus, OPUS_SET_MAX_BANDWIDTH(OPUS_BANDWIDTH_MEDIUMBAND));
	}
	else if (!strcasecmp(local_config.max_bandwidth, "wide"))
	{
		status = opus_encoder_ctl(opvt->opus, OPUS_SET_MAX_BANDWIDTH(OPUS_BANDWIDTH_WIDEBAND));
	}
	else if (!strcasecmp(local_config.max_bandwidth, "super_wide"))
	{
		status = opus_encoder_ctl(opvt->opus, OPUS_SET_MAX_BANDWIDTH(OPUS_BANDWIDTH_SUPERWIDEBAND));
	}
	else if (!strcasecmp(local_config.max_bandwidth, "full"))
	{
		/* Explicit "full" setting: use fullband regardless of sampling rate */
		status = opus_encoder_ctl(opvt->opus, OPUS_SET_MAX_BANDWIDTH(OPUS_BANDWIDTH_FULLBAND));
	}
	else
	{
		/* Fallback: use sampling_rate/playrate based logic for unrecognized values */
		if (sampling_rate <= 8000 || playrate <= 8000)
		{
			status = opus_encoder_ctl(opvt->opus, OPUS_SET_MAX_BANDWIDTH(OPUS_BANDWIDTH_NARROWBAND));
		}
		else if (sampling_rate <= 12000 || playrate <= 12000)
		{
			status = opus_encoder_ctl(opvt->opus, OPUS_SET_MAX_BANDWIDTH(OPUS_BANDWIDTH_MEDIUMBAND));
		}
		else if (sampling_rate <= 16000 || playrate <= 16000)
		{
			status = opus_encoder_ctl(opvt->opus, OPUS_SET_MAX_BANDWIDTH(OPUS_BANDWIDTH_WIDEBAND));
		}
		else if (sampling_rate <= 24000 || playrate <= 24000)
		{
			status = opus_encoder_ctl(opvt->opus, OPUS_SET_MAX_BANDWIDTH(OPUS_BANDWIDTH_SUPERWIDEBAND));
		}
		/* else we use the default: OPUS_BANDWIDTH_FULLBAND */
	}

	/* Set the signal type */
	if (!strcasecmp(local_config.signal_type, "voice"))
	{
		status = opus_encoder_ctl(opvt->opus, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
	}
	else if (!strcasecmp(local_config.signal_type, "music"))
	{
		status = opus_encoder_ctl(opvt->opus, OPUS_SET_SIGNAL(OPUS_SIGNAL_MUSIC));
	}
	/* For "auto", do nothing - this is the default behavior */

	if (bitrate_val > 0 && bitrate_val != 510000)
	{
		status = opus_encoder_ctl(opvt->opus, OPUS_SET_BITRATE(bitrate_val));
	} /* else we use the default: OPUS_AUTO */

	if (local_config.complexity != 10)
	{
		status = opus_encoder_ctl(opvt->opus, OPUS_SET_COMPLEXITY(local_config.complexity));
	}

	status = opus_encoder_ctl(opvt->opus, OPUS_SET_VBR(vbr));
	status = opus_encoder_ctl(opvt->opus, OPUS_SET_INBAND_FEC(use_fec));
	status = opus_encoder_ctl(opvt->opus, OPUS_SET_DTX(use_dtx));

	/* Set packet loss percentage if enabled */
	if (local_config.packet_loss >= 0)
	{
		status = opus_encoder_ctl(opvt->opus, OPUS_SET_PACKET_LOSS_PERC(local_config.packet_loss));
	}

	opvt->sampling_rate = sampling_rate;
	opvt->multiplier = 48000 / sampling_rate;
	opvt->framesize = sampling_rate / 50;
	opvt->id = ast_atomic_fetchadd_int(&usage.encoder_id, 1) + 1;

	ast_atomic_fetchadd_int(&usage.encoders, +1);

	ast_debug(3, "Created encoder #%d (%d -> opus)\n", opvt->id, sampling_rate);

	return 0;
}

static int opus_decoder_construct(struct ast_trans_pvt *pvt, struct ast_frame *f)
{
	struct opus_coder_pvt *opvt = pvt->pvt;
	/* struct opus_attr *attr = ast_format_get_attribute_data(f->subclass.format); */
	int error = 0;

	opvt->sampling_rate = pvt->t->dst_codec.sample_rate;
	opvt->multiplier = 48000 / opvt->sampling_rate;
	opvt->channels = /* attr ? attr->spropstereo + 1 :*/ 1; /* FIXME */

	opvt->opus = opus_decoder_create(opvt->sampling_rate, opvt->channels, &error);

	if (error != OPUS_OK)
	{
		ast_log(LOG_ERROR, "Error creating the Opus decoder: %s\n", opus_strerror(error));
		return -1;
	}

	opvt->id = ast_atomic_fetchadd_int(&usage.decoder_id, 1) + 1;

	ast_atomic_fetchadd_int(&usage.decoders, +1);

	ast_debug(3, "Created decoder #%d (opus -> %d)\n", opvt->id, opvt->sampling_rate);

	return 0;
}

/* Translator callbacks */
static int lintoopus_new(struct ast_trans_pvt *pvt)
{
	return opus_encoder_construct(pvt, pvt->t->src_codec.sample_rate);
}

static int opustolin_new(struct ast_trans_pvt *pvt)
{
	struct opus_coder_pvt *opvt = pvt->pvt;

	opvt->previous_lost = 0; /* we are new and have not lost anything */
	opvt->inited = 0;		 /* we do not know the "sprop" values, yet */

	return 0;
}

static int lintoopus_framein(struct ast_trans_pvt *pvt, struct ast_frame *f)
{
	struct opus_coder_pvt *opvt = pvt->pvt;

	/* XXX We should look at how old the rest of our stream is, and if it
	   is too old, then we should overwrite it entirely, otherwise we can
	   get artifacts of earlier talk that do not belong */
	memcpy(opvt->buf + pvt->samples, f->data.ptr, f->datalen);
	pvt->samples += f->samples;

	return 0;
}

static struct ast_frame *lintoopus_frameout(struct ast_trans_pvt *pvt)
{
	struct opus_coder_pvt *opvt = pvt->pvt;
	struct ast_frame *result = NULL;
	struct ast_frame *last = NULL;
	int samples = 0; /* output samples */

	while (pvt->samples >= opvt->framesize)
	{
		/* status is either error or output bytes */
		const int status = opus_encode(opvt->opus,
									   opvt->buf + samples,
									   opvt->framesize,
									   pvt->outbuf.uc,
									   BUFFER_SAMPLES);

		samples += opvt->framesize;
		pvt->samples -= opvt->framesize;

		if (status < 0)
		{
			ast_log(LOG_ERROR, "Error encoding the Opus frame: %s\n", opus_strerror(status));
		}
		else
		{
			struct ast_frame *current = ast_trans_frameout(pvt,
														   status,
														   OPUS_SAMPLES);

			if (!current)
			{
				continue;
			}
			else if (last)
			{
				AST_LIST_NEXT(last, frame_list) = current;
			}
			else
			{
				result = current;
			}
			last = current;
		}
	}

	/* Move the data at the end of the buffer to the front */
	if (samples)
	{
		memmove(opvt->buf, opvt->buf + samples, pvt->samples * 2);
	}

	return result;
}

static int opustolin_framein(struct ast_trans_pvt *pvt, struct ast_frame *f)
{
	struct opus_coder_pvt *opvt = pvt->pvt;
	int decode_fec;
	int frame_size;
	opus_int16 *dst;
	opus_int32 len;
	unsigned char *src;
	int status;

	if (!opvt->inited && f->datalen == 0)
	{
		return 0; /* we cannot start without data */
	}
	else if (!opvt->inited)
	{ /* 0 < f->datalen */
		status = opus_decoder_construct(pvt, f);
		opvt->inited = 1;
		if (status)
		{
			return status;
		}
	}

	/*
	 * When we get a frame indicator (ast_null_frame), format is NULL. Because FEC
	 * status can change any time (SDP re-negotiation), we save again and again.
	 */
	if (f->subclass.format)
	{
		struct opus_attr *attr = ast_format_get_attribute_data(f->subclass.format);

		if (attr)
		{
			opvt->decode_fec_incoming = attr->fec;
		}
	}
	decode_fec = opvt->decode_fec_incoming;

	/*
	 * The Opus Codec, actually its library allows
	 * - Forward-Error Correction (FEC), and
	 * - native Packet-Loss Concealment (PLC).
	 * The sender might include FEC. If there is no FEC, because it was not send
	 * or the FEC data got lost, the API of the Opus library does PLC instead.
	 * Therefore we have three boolean variables:
	 * - current frame got lost: f->datalen == 0,
	 * - previous frame got lost: opvt->previous_lost, and
	 * - FEC negotiated on SDP layer: decode_fec.
	 * Now, we go through all cases. Because some cases use the same source code
	 * we have less than 8 (2^3) cases.
	 *
	 * Some notes on the coding style of this section:
	 * This code section is passed for each incoming frame, normally every
	 * 20 milliseconds. For each channel, this code is passed individually.
	 * Therefore, this code should be as performant as possible. On the other
	 * hand, PLC plus FEC is complicated. Therefore, code readability is one
	 * prerequisite to understand, debug, and review this code section. Because
	 * we do have optimising compilers, we are able to sacrify optimised code
	 * for code readability. If you find an error or unnecessary calculation
	 * which is not optimised = removed by your compiler, please, create an
	 * issue on <https://github.com/traud/asterisk-opus/issues>. I am just
	 * a human and human do mistakes. However, humans love to learn.
	 *
	 * Source-code examples are
	 * - <https://git.xiph.org/?p=opus.git;a=history;f=src/opus_demo.c>,
	 * - <https://freeswitch.org/stash/projects/FS/repos/freeswitch/browse/src/mod/codecs/mod_opus/mod_opus.c>
	 * and the official mailing list itself:
	 * <https://www.google.de/search?q=site:lists.xiph.org+opus>.
	 */

	/* Case 1 and 2 */
	if (f->datalen == 0 && opvt->previous_lost)
	{
		/*
		 * If this frame and the previous frame got lost, we do not have any
		 * data for FEC. Therefore, we go for PLC on the previous frame. However,
		 * the next frame could include FEC for the currently lost frame.
		 * Therefore, we "wait" for the next frame to fix the current frame.
		 */
		decode_fec = 0; /* = do PLC */
		opus_decoder_ctl(opvt->opus, OPUS_GET_LAST_PACKET_DURATION(&frame_size));
		dst = pvt->outbuf.i16 + (pvt->samples * opvt->channels);
		len = 0;
		src = NULL;
		status = opus_decode(opvt->opus, src, len, dst, frame_size, decode_fec);
		if (status < 0)
		{
			ast_log(LOG_ERROR, "%s\n", opus_strerror(status));
		}
		else
		{
			pvt->samples += status;
			pvt->datalen += status * opvt->channels * sizeof(int16_t);
		}
		/*
		 * Save the state of the current frame, whether it is lost = "wait".
		 * That way, we are able to decide whether to do FEC next time.
		 */
		opvt->previous_lost = (f->datalen == 0 || status < 0);
		return 0;
	}

	/* Case 3 */
	if (f->datalen == 0 && !decode_fec)
	{ /* !opvt->previous_lost */
		/*
		 * The sender stated in SDP: "I am not going to provide FEC". Therefore,
		 * we do not wait for the next frame and do PLC right away.
		 */
		decode_fec = 0;
		opus_decoder_ctl(opvt->opus, OPUS_GET_LAST_PACKET_DURATION(&frame_size));
		dst = pvt->outbuf.i16 + (pvt->samples * opvt->channels);
		len = f->datalen;
		src = NULL;
		status = opus_decode(opvt->opus, src, len, dst, frame_size, decode_fec);
		if (status < 0)
		{
			ast_log(LOG_ERROR, "%s\n", opus_strerror(status));
		}
		else
		{
			pvt->samples += status;
			pvt->datalen += status * opvt->channels * sizeof(int16_t);
		}
		opvt->previous_lost = (f->datalen == 0 || status < 0);
		return 0;
	}

	/* Case 4 */
	if (f->datalen == 0)
	{ /* decode_fec && !opvt->previous_lost */
		/*
		 * The previous frame was of no issue. Therefore, we do not have to
		 * reconstruct it. We do not have any data in the current frame but the
		 * sender might give us FEC with the next frame. We cannot do anything
		 * but wait for the next frame. Till Asterisk 13.7, this creates the
		 * warning "opustolin48 did not update samples 0". Please, ignore this
		 * warning or apply the patch included in the GitHub repository.
		 */
		status = 0; /* no samples to add currently */
		if (status < 0)
		{
			ast_log(LOG_ERROR, "%s\n", opus_strerror(status));
		}
		else
		{
			pvt->samples += status;
			pvt->datalen += status * opvt->channels * sizeof(int16_t);
		}
		opvt->previous_lost = (f->datalen == 0 || status < 0);
		return 0;
	}

	/* Case 5 and 6 */
	if (!opvt->previous_lost)
	{ /* 0 < f->datalen */
		/*
		 * The perfect case - the previous frame was not lost and we have data
		 * in the current frame. Therefore, neither FEC nor PLC are required.
		 */
		decode_fec = 0;
		frame_size = BUFFER_SAMPLES / opvt->multiplier; /* parse everything */
		dst = pvt->outbuf.i16 + (pvt->samples * opvt->channels);
		len = f->datalen;
		src = f->data.ptr;
		status = opus_decode(opvt->opus, src, len, dst, frame_size, decode_fec);
		if (status < 0)
		{
			ast_log(LOG_ERROR, "%s\n", opus_strerror(status));
		}
		else
		{
			pvt->samples += status;
			pvt->datalen += status * opvt->channels * sizeof(int16_t);
		}
		opvt->previous_lost = (f->datalen == 0 || status < 0);
		return 0;
	}

	/* Case 7 */
	if (!decode_fec)
	{ /* 0 < f->datalen && opvt->previous_lost */
		/*
		 * The previous frame got lost and the sender stated in SDP: "I am not
		 * going to provide FEC". Therefore, we do PLC. Furthermore, we try to
		 * decode the current frame because we have data. This creates jitter
		 * because we create double the amount of frames as normal, see
		 * <https://issues.asterisk.org/jira/browse/ASTERISK-25483>. If this is
		 * an issue for your use-case, please, file and issue report on
		 * <https://github.com/traud/asterisk-opus/issues>.
		 */
		decode_fec = 0;
		opus_decoder_ctl(opvt->opus, OPUS_GET_LAST_PACKET_DURATION(&frame_size));
		dst = pvt->outbuf.i16 + (pvt->samples * opvt->channels);
		len = 0;
		src = NULL;
		status = opus_decode(opvt->opus, src, len, dst, frame_size, decode_fec);
		if (status < 0)
		{
			ast_log(LOG_ERROR, "%s\n", opus_strerror(status));
		}
		else
		{
			pvt->samples += status;
			pvt->datalen += status * opvt->channels * sizeof(int16_t);
		}
		decode_fec = 0;
		frame_size = BUFFER_SAMPLES / opvt->multiplier;			 /* parse everything */
		dst = pvt->outbuf.i16 + (pvt->samples * opvt->channels); /* append after PLC data */
		len = f->datalen;
		src = f->data.ptr;
		status = opus_decode(opvt->opus, src, len, dst, frame_size, decode_fec);
		if (status < 0)
		{
			ast_log(LOG_ERROR, "%s\n", opus_strerror(status));
		}
		else
		{
			pvt->samples += status;
			pvt->datalen += status * opvt->channels * sizeof(int16_t);
		}
		opvt->previous_lost = (f->datalen == 0 || status < 0);
		return 0;
	}

	/* Case 8; Last Case */
	{ /* 0 < f->datalen && opvt->previous_lost && decode_fec */
		decode_fec = 1;
		opus_decoder_ctl(opvt->opus, OPUS_GET_LAST_PACKET_DURATION(&frame_size));
		dst = pvt->outbuf.i16 + (pvt->samples * opvt->channels);
		len = f->datalen;
		src = f->data.ptr;
		status = opus_decode(opvt->opus, src, len, dst, frame_size, decode_fec);
		if (status < 0)
		{
			ast_log(LOG_ERROR, "%s\n", opus_strerror(status));
		}
		else
		{
			pvt->samples += status;
			pvt->datalen += status * opvt->channels * sizeof(int16_t);
		}
		decode_fec = 0;
		frame_size = BUFFER_SAMPLES / opvt->multiplier;			 /* parse everything */
		dst = pvt->outbuf.i16 + (pvt->samples * opvt->channels); /* append after FEC data */
		len = f->datalen;
		src = f->data.ptr;
		status = opus_decode(opvt->opus, src, len, dst, frame_size, decode_fec);
		if (status < 0)
		{
			ast_log(LOG_ERROR, "%s\n", opus_strerror(status));
		}
		else
		{
			pvt->samples += status;
			pvt->datalen += status * opvt->channels * sizeof(int16_t);
		}
		opvt->previous_lost = (f->datalen == 0 || status < 0);
		return 0;
	}
}

static void lintoopus_destroy(struct ast_trans_pvt *arg)
{
	struct opus_coder_pvt *opvt = arg->pvt;

	if (!opvt || !opvt->opus)
	{
		return;
	}

	opus_encoder_destroy(opvt->opus);
	opvt->opus = NULL;

	ast_atomic_fetchadd_int(&usage.encoders, -1);

	ast_debug(3, "Destroyed encoder #%d (%d->opus)\n", opvt->id, opvt->sampling_rate);
}

static void opustolin_destroy(struct ast_trans_pvt *arg)
{
	struct opus_coder_pvt *opvt = arg->pvt;

	if (!opvt || !opvt->opus)
	{
		return;
	}

	opus_decoder_destroy(opvt->opus);
	opvt->opus = NULL;

	ast_atomic_fetchadd_int(&usage.decoders, -1);

	ast_debug(3, "Destroyed decoder #%d (opus->%d)\n", opvt->id, opvt->sampling_rate);
}

static char *handle_cli_opus_show(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a)
{
	struct codec_usage copy;
	struct opus_config local_config;

	switch (cmd)
	{
	case CLI_INIT:
		e->command = "opus show";
		e->usage =
			"Usage: opus show\n"
			"       Displays Opus encoder/decoder utilization and configuration.\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;
	}

	if (a->argc != 2)
	{
		return CLI_SHOWUSAGE;
	}

	copy = usage;

	/* Take a snapshot of configuration under read lock */
	ast_rwlock_rdlock(&config_lock);
	local_config = config;
	ast_rwlock_unlock(&config_lock);

	ast_cli(a->fd, "%d/%d encoders/decoders are in use.\n", copy.encoders, copy.decoders);

	ast_cli(a->fd, "\nCurrent Opus Configuration:\n");
	ast_cli(a->fd, "-------------------------\n");
	ast_cli(a->fd, "Complexity:       %d\n", local_config.complexity);
	ast_cli(a->fd, "Bitrate:          %d bit/s\n", local_config.bitrate);
	ast_cli(a->fd, "Max Playback Rate: %d Hz\n", local_config.max_playback_rate);
	ast_cli(a->fd, "Max Bandwidth:    %s\n", local_config.max_bandwidth);
	ast_cli(a->fd, "Signal Type:      %s\n", local_config.signal_type);
	ast_cli(a->fd, "Application:      %s\n", local_config.application);
	ast_cli(a->fd, "FEC:              %s\n", local_config.fec ? "enabled" : "disabled");
	ast_cli(a->fd, "DTX:              %s\n", local_config.dtx ? "enabled" : "disabled");
	ast_cli(a->fd, "CBR:              %s\n", local_config.cbr ? "enabled" : "disabled (VBR)");
	if (local_config.packet_loss >= 0)
	{
		ast_cli(a->fd, "Packet Loss:      %d%%\n", local_config.packet_loss);
	}
	else
	{
		ast_cli(a->fd, "Packet Loss:      disabled\n");
	}
	ast_cli(a->fd, "\nNote: SDP negotiated parameters from clients may override these settings.\n");

	return CLI_SUCCESS;
}

/* Translators */
static struct ast_translator opustolin = {
	.table_cost = AST_TRANS_COST_LY_LL_ORIGSAMP,
	.name = "opustolin",
	.src_codec = {
		.name = "opus",
		.type = AST_MEDIA_TYPE_AUDIO,
		.sample_rate = 48000,
	},
	.dst_codec = {
		.name = "slin",
		.type = AST_MEDIA_TYPE_AUDIO,
		.sample_rate = 8000,
	},
	.format = "slin",
	.newpvt = opustolin_new,
	.framein = opustolin_framein,
	.destroy = opustolin_destroy,
	.sample = opus_sample,
	.desc_size = sizeof(struct opus_coder_pvt),
	.buffer_samples = (BUFFER_SAMPLES / (48000 / 8000)) * 2, /* because of possible FEC */
	.buf_size = (BUFFER_SAMPLES / (48000 / 8000)) * MAX_CHANNELS * sizeof(opus_int16) * 2,
	.native_plc = 1,
};

static struct ast_translator lintoopus = {
	.table_cost = AST_TRANS_COST_LL_LY_ORIGSAMP,
	.name = "lintoopus",
	.src_codec = {
		.name = "slin",
		.type = AST_MEDIA_TYPE_AUDIO,
		.sample_rate = 8000,
	},
	.dst_codec = {
		.name = "opus",
		.type = AST_MEDIA_TYPE_AUDIO,
		.sample_rate = 48000,
	},
	.format = "opus",
	.newpvt = lintoopus_new,
	.framein = lintoopus_framein,
	.frameout = lintoopus_frameout,
	.destroy = lintoopus_destroy,
	.sample = slin8_sample,
	.desc_size = sizeof(struct opus_coder_pvt),
	.buffer_samples = BUFFER_SAMPLES,
	.buf_size = BUFFER_SAMPLES * 2,
};

static struct ast_translator opustolin12 = {
	.table_cost = AST_TRANS_COST_LY_LL_ORIGSAMP - 1,
	.name = "opustolin12",
	.src_codec = {
		.name = "opus",
		.type = AST_MEDIA_TYPE_AUDIO,
		.sample_rate = 48000,
	},
	.dst_codec = {
		.name = "slin",
		.type = AST_MEDIA_TYPE_AUDIO,
		.sample_rate = 12000,
	},
	.format = "slin12",
	.newpvt = opustolin_new,
	.framein = opustolin_framein,
	.destroy = opustolin_destroy,
	.sample = opus_sample,
	.desc_size = sizeof(struct opus_coder_pvt),
	.buffer_samples = (BUFFER_SAMPLES / (48000 / 12000)) * 2, /* because of possible FEC */
	.buf_size = (BUFFER_SAMPLES / (48000 / 12000)) * MAX_CHANNELS * sizeof(opus_int16) * 2,
	.native_plc = 1,
};

static struct ast_translator lin12toopus = {
	.table_cost = AST_TRANS_COST_LL_LY_ORIGSAMP - 1,
	.name = "lin12toopus",
	.src_codec = {
		.name = "slin",
		.type = AST_MEDIA_TYPE_AUDIO,
		.sample_rate = 12000,
	},
	.dst_codec = {
		.name = "opus",
		.type = AST_MEDIA_TYPE_AUDIO,
		.sample_rate = 48000,
	},
	.format = "opus",
	.newpvt = lintoopus_new,
	.framein = lintoopus_framein,
	.frameout = lintoopus_frameout,
	.destroy = lintoopus_destroy,
	.desc_size = sizeof(struct opus_coder_pvt),
	.buffer_samples = BUFFER_SAMPLES,
	.buf_size = BUFFER_SAMPLES * 2,
};

static struct ast_translator opustolin16 = {
	.table_cost = AST_TRANS_COST_LY_LL_ORIGSAMP - 2,
	.name = "opustolin16",
	.src_codec = {
		.name = "opus",
		.type = AST_MEDIA_TYPE_AUDIO,
		.sample_rate = 48000,
	},
	.dst_codec = {
		.name = "slin",
		.type = AST_MEDIA_TYPE_AUDIO,
		.sample_rate = 16000,
	},
	.format = "slin16",
	.newpvt = opustolin_new,
	.framein = opustolin_framein,
	.destroy = opustolin_destroy,
	.sample = opus_sample,
	.desc_size = sizeof(struct opus_coder_pvt),
	.buffer_samples = (BUFFER_SAMPLES / (48000 / 16000)) * 2, /* because of possible FEC */
	.buf_size = (BUFFER_SAMPLES / (48000 / 16000)) * MAX_CHANNELS * sizeof(opus_int16) * 2,
	.native_plc = 1,
};

static struct ast_translator lin16toopus = {
	.table_cost = AST_TRANS_COST_LL_LY_ORIGSAMP - 2,
	.name = "lin16toopus",
	.src_codec = {
		.name = "slin",
		.type = AST_MEDIA_TYPE_AUDIO,
		.sample_rate = 16000,
	},
	.dst_codec = {
		.name = "opus",
		.type = AST_MEDIA_TYPE_AUDIO,
		.sample_rate = 48000,
	},
	.format = "opus",
	.newpvt = lintoopus_new,
	.framein = lintoopus_framein,
	.frameout = lintoopus_frameout,
	.destroy = lintoopus_destroy,
	.sample = slin16_sample,
	.desc_size = sizeof(struct opus_coder_pvt),
	.buffer_samples = BUFFER_SAMPLES,
	.buf_size = BUFFER_SAMPLES * 2,
};

static struct ast_translator opustolin24 = {
	.table_cost = AST_TRANS_COST_LY_LL_ORIGSAMP - 4,
	.name = "opustolin24",
	.src_codec = {
		.name = "opus",
		.type = AST_MEDIA_TYPE_AUDIO,
		.sample_rate = 48000,
	},
	.dst_codec = {
		.name = "slin",
		.type = AST_MEDIA_TYPE_AUDIO,
		.sample_rate = 24000,
	},
	.format = "slin24",
	.newpvt = opustolin_new,
	.framein = opustolin_framein,
	.destroy = opustolin_destroy,
	.sample = opus_sample,
	.desc_size = sizeof(struct opus_coder_pvt),
	.buffer_samples = (BUFFER_SAMPLES / (48000 / 24000)) * 2, /* because of possible FEC */
	.buf_size = (BUFFER_SAMPLES / (48000 / 24000)) * MAX_CHANNELS * sizeof(opus_int16) * 2,
	.native_plc = 1,
};

static struct ast_translator lin24toopus = {
	.table_cost = AST_TRANS_COST_LL_LY_ORIGSAMP - 4,
	.name = "lin24toopus",
	.src_codec = {
		.name = "slin",
		.type = AST_MEDIA_TYPE_AUDIO,
		.sample_rate = 24000,
	},
	.dst_codec = {
		.name = "opus",
		.type = AST_MEDIA_TYPE_AUDIO,
		.sample_rate = 48000,
	},
	.format = "opus",
	.newpvt = lintoopus_new,
	.framein = lintoopus_framein,
	.frameout = lintoopus_frameout,
	.destroy = lintoopus_destroy,
	.desc_size = sizeof(struct opus_coder_pvt),
	.buffer_samples = BUFFER_SAMPLES,
	.buf_size = BUFFER_SAMPLES * 2,
};

static struct ast_translator opustolin48 = {
	.table_cost = AST_TRANS_COST_LY_LL_ORIGSAMP - 8,
	.name = "opustolin48",
	.src_codec = {
		.name = "opus",
		.type = AST_MEDIA_TYPE_AUDIO,
		.sample_rate = 48000,
	},
	.dst_codec = {
		.name = "slin",
		.type = AST_MEDIA_TYPE_AUDIO,
		.sample_rate = 48000,
	},
	.format = "slin48",
	.newpvt = opustolin_new,
	.framein = opustolin_framein,
	.destroy = opustolin_destroy,
	.sample = opus_sample,
	.desc_size = sizeof(struct opus_coder_pvt),
	.buffer_samples = BUFFER_SAMPLES * 2, /* twice, because of possible FEC */
	.buf_size = BUFFER_SAMPLES * MAX_CHANNELS * sizeof(opus_int16) * 2,
	.native_plc = 1,
};

static struct ast_translator lin48toopus = {
	.table_cost = AST_TRANS_COST_LL_LY_ORIGSAMP - 8,
	.name = "lin48toopus",
	.src_codec = {
		.name = "slin",
		.type = AST_MEDIA_TYPE_AUDIO,
		.sample_rate = 48000,
	},
	.dst_codec = {
		.name = "opus",
		.type = AST_MEDIA_TYPE_AUDIO,
		.sample_rate = 48000,
	},
	.format = "opus",
	.newpvt = lintoopus_new,
	.framein = lintoopus_framein,
	.frameout = lintoopus_frameout,
	.destroy = lintoopus_destroy,
	.desc_size = sizeof(struct opus_coder_pvt),
	.buffer_samples = BUFFER_SAMPLES,
	.buf_size = BUFFER_SAMPLES * 2,
};

static struct ast_cli_entry cli[] = {
	AST_CLI_DEFINE(handle_cli_opus_show, "Display Opus codec utilization.")};

static int opus_samples(struct ast_frame *frame)
{
	opus_int32 sampling_rate = 48000; /* FIXME */

	return opus_packet_get_nb_samples(frame->data.ptr, frame->datalen, sampling_rate);
}

static int parse_config(int reload)
{
	struct ast_flags config_flags = {reload ? CONFIG_FLAG_FILEUNCHANGED : 0};
	struct ast_config *cfg = ast_config_load("codecs.conf", config_flags);
	struct ast_variable *var;
	struct opus_config new_config = {0};  /* Zero-initialize for defensive programming */
	int res = 0;

	/* Handle special config status values
	 * Return codes: 0 = success (config loaded), 1 = error, 2 = no change needed */
	if (cfg == CONFIG_STATUS_FILEINVALID)
	{
		ast_log(LOG_ERROR, "codecs.conf is malformed or invalid, using existing configuration\n");
		return 1;
	}
	if (cfg == CONFIG_STATUS_FILEMISSING)
	{
		/* Missing file: use defaults on initial load, no action on reload */
		return 2;
	}
	if (cfg == CONFIG_STATUS_FILEUNCHANGED)
	{
		/* File unchanged since last load: no action needed */
		return 2;
	}

	/* Start with current config as base (parse-then-swap pattern) */
	ast_rwlock_rdlock(&config_lock);
	new_config = config;
	ast_rwlock_unlock(&config_lock);

	/* Parse all values into temporary config first */
	for (var = ast_variable_browse(cfg, "opus"); var; var = var->next)
	{
		if (!strcasecmp(var->name, "complexity"))
		{
			char *endptr;
			long val = strtol(var->value, &endptr, 10);
			if (endptr == var->value || *endptr != '\0')
			{
				res = 1;
				ast_log(LOG_ERROR, "complexity must be a numeric value (got '%s')\n", var->value);
				break;
			}
			if (val < 0 || val > 10)
			{
				res = 1;
				ast_log(LOG_ERROR, "complexity must be in 0-10\n");
				break;
			}
			new_config.complexity = (int) val;
		}
		else if (!strcasecmp(var->name, "bitrate") || !strcasecmp(var->name, "max_average_bitrate"))
		{
			if (!strcasecmp(var->value, "auto") || !strcasecmp(var->value, "max"))
			{
				new_config.bitrate = CODEC_OPUS_DEFAULT_BITRATE;
			}
			else
			{
				char *endptr;
				long val = strtol(var->value, &endptr, 10);
				if (endptr == var->value || *endptr != '\0')
				{
					res = 1;
					ast_log(LOG_ERROR, "bitrate must be a numeric value or 'auto'/'max' (got '%s')\n", var->value);
					break;
				}
				if (val < 500 || val > 512000)
				{
					res = 1;
					ast_log(LOG_ERROR, "bitrate must be in 500-512000 or 'auto'/'max'\n");
					break;
				}
				new_config.bitrate = (opus_int32) val;
			}
		}
		else if (!strcasecmp(var->name, "fec"))
		{
			/* Normalize to 0 or 1 since ast_true() returns -1 for true */
			new_config.fec = !!ast_true(var->value);
		}
		else if (!strcasecmp(var->name, "dtx"))
		{
			/* Normalize to 0 or 1 since ast_true() returns -1 for true */
			new_config.dtx = !!ast_true(var->value);
		}
		else if (!strcasecmp(var->name, "cbr"))
		{
			/* Normalize to 0 or 1 since ast_true() returns -1 for true */
			new_config.cbr = !!ast_true(var->value);
		}
		else if (!strcasecmp(var->name, "max_playback_rate") || !strcasecmp(var->name, "maxplaybackrate"))
		{
			char *endptr;
			long val = strtol(var->value, &endptr, 10);
			if (endptr == var->value || *endptr != '\0')
			{
				res = 1;
				ast_log(LOG_ERROR, "max_playback_rate must be a numeric value (got '%s')\n", var->value);
				break;
			}
			/* Opus only supports specific sampling rates */
			if (val != 8000 && val != 12000 && val != 16000 && val != 24000 && val != 48000)
			{
				res = 1;
				ast_log(LOG_ERROR, "max_playback_rate must be one of: 8000, 12000, 16000, 24000, 48000\n");
				break;
			}
			new_config.max_playback_rate = (int) val;
		}
		else if (!strcasecmp(var->name, "packet_loss"))
		{
			char *endptr;
			long val = strtol(var->value, &endptr, 10);
			if (endptr == var->value || *endptr != '\0')
			{
				res = 1;
				ast_log(LOG_ERROR, "packet_loss must be a numeric value (got '%s')\n", var->value);
				break;
			}
			if (val < -1 || val > 100)
			{
				res = 1;
				ast_log(LOG_ERROR, "packet_loss must be in -1-100\n");
				break;
			}
			if (val == 0)
			{
				ast_debug(1, "packet_loss=0 forces FEC for all packets (no loss estimation)\n");
			}
			new_config.packet_loss = (int) val;
		}
		else if (!strcasecmp(var->name, "max_bandwidth"))
		{
			if (strcasecmp(var->value, "narrow") &&
				strcasecmp(var->value, "medium") &&
				strcasecmp(var->value, "wide") &&
				strcasecmp(var->value, "super_wide") &&
				strcasecmp(var->value, "full"))
			{
				res = 1;
				ast_log(LOG_ERROR, "max_bandwidth must be one of: narrow, medium, wide, super_wide, full\n");
				break;
			}
			ast_copy_string(new_config.max_bandwidth, var->value, sizeof(new_config.max_bandwidth));
		}
		else if (!strcasecmp(var->name, "signal"))
		{
			if (strcasecmp(var->value, "auto") &&
				strcasecmp(var->value, "voice") &&
				strcasecmp(var->value, "music"))
			{
				res = 1;
				ast_log(LOG_ERROR, "signal must be one of: auto, voice, music\n");
				break;
			}
			ast_copy_string(new_config.signal_type, var->value, sizeof(new_config.signal_type));
		}
		else if (!strcasecmp(var->name, "application"))
		{
			if (strcasecmp(var->value, "voip") &&
				strcasecmp(var->value, "audio") &&
				strcasecmp(var->value, "low_delay"))
			{
				res = 1;
				ast_log(LOG_ERROR, "application must be one of: voip, audio, low_delay\n");
				break;
			}
			ast_copy_string(new_config.application, var->value, sizeof(new_config.application));
		}
	}
	ast_config_destroy(cfg);

	/* Only apply changes if all parsing succeeded (atomic swap) */
	if (!res)
	{
		ast_rwlock_wrlock(&config_lock);
		config = new_config;
		ast_rwlock_unlock(&config_lock);
	}

	return res;
}

static int reload(void)
{
	int res = parse_config(1);

	if (res == 1)
	{
		/* Error parsing configuration */
		return AST_MODULE_LOAD_DECLINE;
	}

	if (res == 0)
	{
		/* Configuration was actually reloaded */
		ast_verbose(VERBOSE_PREFIX_2 "Opus codec configuration reloaded\n");
	}
	/* res == 2 means no change needed (file missing or unchanged), stay silent */

	return AST_MODULE_LOAD_SUCCESS;
}

static int unload_module(void)
{
	int res;

	opus_codec->samples_count = opus_samples_previous;
	ao2_ref(opus_codec, -1);

	res = ast_unregister_translator(&opustolin);
	res |= ast_unregister_translator(&lintoopus);
	res |= ast_unregister_translator(&opustolin12);
	res |= ast_unregister_translator(&lin12toopus);
	res |= ast_unregister_translator(&opustolin16);
	res |= ast_unregister_translator(&lin16toopus);
	res |= ast_unregister_translator(&opustolin24);
	res |= ast_unregister_translator(&lin24toopus);
	res |= ast_unregister_translator(&opustolin48);
	res |= ast_unregister_translator(&lin48toopus);

	ast_cli_unregister_multiple(cli, ARRAY_LEN(cli));

	ast_rwlock_destroy(&config_lock);

	return res;
}

static int load_module(void)
{
	int res;

	ast_rwlock_init(&config_lock);

	/* parse_config returns: 0 = loaded, 1 = error, 2 = no file (use defaults) */
	if (parse_config(0) == 1)
	{
		ast_rwlock_destroy(&config_lock);
		return AST_MODULE_LOAD_DECLINE;
	}

	opus_codec = ast_codec_get("opus", AST_MEDIA_TYPE_AUDIO, 48000);
	if (!opus_codec)
	{
		ast_log(LOG_ERROR, "Failed to get opus codec\n");
		ast_rwlock_destroy(&config_lock);
		return AST_MODULE_LOAD_DECLINE;
	}
	opus_samples_previous = opus_codec->samples_count;
	opus_codec->samples_count = opus_samples;

	res = ast_register_translator(&opustolin);
	res |= ast_register_translator(&lintoopus);
	res |= ast_register_translator(&opustolin12);
	res |= ast_register_translator(&lin12toopus);
	res |= ast_register_translator(&opustolin16);
	res |= ast_register_translator(&lin16toopus);
	res |= ast_register_translator(&opustolin24);
	res |= ast_register_translator(&lin24toopus);
	res |= ast_register_translator(&opustolin48);
	res |= ast_register_translator(&lin48toopus);

	if (res)
	{
		ast_log(LOG_ERROR, "Failed to register one or more Opus translators\n");
		/* Unregister any translators that may have succeeded */
		ast_unregister_translator(&opustolin);
		ast_unregister_translator(&lintoopus);
		ast_unregister_translator(&opustolin12);
		ast_unregister_translator(&lin12toopus);
		ast_unregister_translator(&opustolin16);
		ast_unregister_translator(&lin16toopus);
		ast_unregister_translator(&opustolin24);
		ast_unregister_translator(&lin24toopus);
		ast_unregister_translator(&opustolin48);
		ast_unregister_translator(&lin48toopus);
		/* Restore codec and cleanup */
		opus_codec->samples_count = opus_samples_previous;
		ao2_ref(opus_codec, -1);
		ast_rwlock_destroy(&config_lock);
		return AST_MODULE_LOAD_DECLINE;
	}

	ast_cli_register_multiple(cli, ARRAY_LEN(cli));

	return AST_MODULE_LOAD_SUCCESS;
}

AST_MODULE_INFO(ASTERISK_GPL_KEY, AST_MODFLAG_DEFAULT, "Opus Coder/Decoder",
				.load = load_module,
				.unload = unload_module,
				.reload = reload, );
