/*
 * BlueALSA - rfcomm.c
 * Copyright (c) 2016-2017 Arkadiusz Bokowy
 *               2017 Juha Kuikka
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#include "rfcomm.h"

#include <errno.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/eventfd.h>

#include "bluealsa.h"
#include "utils.h"
#include "shared/log.h"


/**
 * Convenient wrapper for writing to the RFCOMM socket. */
static ssize_t rfcomm_write(int fd, const char *msg) {

	size_t len = strlen(msg);
	ssize_t ret;

retry:
	if ((ret = write(fd, msg, len)) == -1) {
		if (errno == EINTR)
			goto retry;
		error("RFCOMM write error: %s", strerror(errno));
	}

	return ret;
}

/**
 * Write AT message (command, response) to the RFCOMM. */
static ssize_t rfcomm_write_at_msg(int fd, enum bt_at_type type,
		const char *command, const char *value) {
	char buffer[256];
	debug("Sending AT message: %s: command:%s, value:%s", at_type2str(type), command, value);
	return rfcomm_write(fd, at_build(buffer, type, command, value));
}

/**
 * Unsolicited response handler. */
static void rfcomm_handler_resp(struct ba_transport *t, struct bt_at *at, int fd) {
	(void)at;
	(void)fd;

	if (t->profile == BLUETOOTH_PROFILE_HFP_HF && t->rfcomm.hfp_slcs != HFP_CONNECTED)
		/* advance service level connection state */
		if (strcmp(at->value, "OK") == 0)
			t->rfcomm.hfp_slcs++;

}

/**
 * TEST: Standard indicator update AT command */
static void rfcomm_handler_cind_test(struct ba_transport *t, struct bt_at *at, int fd) {
	(void)t;
	(void)at;
	rfcomm_write_at_msg(fd, AT_TYPE_RESP, "+CIND",
			"(\"call\",(0,1))"
			",(\"callsetup\",(0-3))"
			",(\"service\",(0-1))"
			",(\"signal\",(0-5))"
			",(\"roam\",(0,1))"
			",(\"battchg\",(0-5))"
			",(\"callheld\",(0-2))");
	rfcomm_write_at_msg(fd, AT_TYPE_RESP, NULL, "OK");
}

/**
 * GET: Standard indicator update AT command */
static void rfcomm_handler_cind_get(struct ba_transport *t, struct bt_at *at, int fd) {
	(void)t;
	(void)at;
	rfcomm_write_at_msg(fd, AT_TYPE_RESP, "+CIND", "0,0,1,5,0,5,0");
	rfcomm_write_at_msg(fd, AT_TYPE_RESP, NULL, "OK");
}

/**
 * RESP: Standard indicator update AT command */
static void rfcomm_handler_cind_resp(struct ba_transport *t, struct bt_at *at, int fd) {
	(void)at;
	(void)fd;
	if (t->rfcomm.hfp_slcs == HFP_SLC_CIND_TEST) {
		t->rfcomm.hfp_slcs = HFP_SLC_CIND_TEST_OK;
	}
	else if (t->rfcomm.hfp_slcs == HFP_SLC_CIND_GET) {
		t->rfcomm.hfp_slcs = HFP_SLC_CIND_GET_OK;
	}
}

/**
 * SET: Standard event reporting activation/deactivation AT command */
static void rfcomm_handler_cmer_set(struct ba_transport *t, struct bt_at *at, int fd) {
	(void)t;
	(void)at;
	rfcomm_write_at_msg(fd, AT_TYPE_RESP, NULL, "OK");
}

/**
 * SET: Bluetooth Indicators Activation */
static void rfcomm_handler_bia_set(struct ba_transport *t, struct bt_at *at, int fd) {
	(void)t;
	(void)at;
	(void)fd;
}

/**
 * SET: Bluetooth Retrieve Supported Features */
static void rfcomm_handler_brsf_set(struct ba_transport *t, struct bt_at *at, int fd) {

	char tmp[16];

	t->rfcomm.hfp_features = strtoul(at->value, NULL, 10);
	debug("Got HFP HF features: 0x%X", t->rfcomm.hfp_features);

	/* Codec negotiation is not supported in the HF, hence no
	 * wideband audio support. AT+BAC will not be sent. */
	if ((t->rfcomm.hfp_features & HFP_HF_FEAT_CODEC) == 0)
		t->rfcomm.sco->codec = HFP_CODEC_CVSD;

	sprintf(tmp, "%u", BA_HFP_AG_FEATURES);
	rfcomm_write_at_msg(fd, AT_TYPE_RESP, "+BRSF", tmp);
	rfcomm_write_at_msg(fd, AT_TYPE_RESP, NULL, "OK");
}

/**
 * RESP: Bluetooth Retrieve Supported Features */
static void rfcomm_handler_brsf_resp(struct ba_transport *t, struct bt_at *at, int fd) {
	(void)fd;
	t->rfcomm.hfp_slcs = HFP_SLC_BRSF_SET_OK;
	t->rfcomm.hfp_features = strtoul(at->value, NULL, 10);
	debug("Got HFP AG features: 0x%X", t->rfcomm.hfp_features);
}

/**
 * SET: Noise Reduction and Echo Canceling */
static void rfcomm_handler_nrec_set(struct ba_transport *t, struct bt_at *at, int fd) {
	(void)t;
	(void)at;
	(void)fd;
}

/**
 * SET: Gain of Microphone */
static void rfcomm_handler_vgm_set(struct ba_transport *t, struct bt_at *at, int fd) {
	t->rfcomm.sco->sco.mic_gain = t->rfcomm.mic_gain = atoi(at->value);
	rfcomm_write_at_msg(fd, AT_TYPE_RESP, NULL, "OK");
	bluealsa_event();
}

/**
 * SET: Gain of Speaker */
static void rfcomm_handler_vgs_set(struct ba_transport *t, struct bt_at *at, int fd) {
	t->rfcomm.sco->sco.spk_gain = t->rfcomm.spk_gain = atoi(at->value);
	rfcomm_write_at_msg(fd, AT_TYPE_RESP, NULL, "OK");
	bluealsa_event();
}

/**
 * SET: Bluetooth Response and Hold Feature */
static void rfcomm_handler_btrh_get(struct ba_transport *t, struct bt_at *at, int fd) {
	(void)t;
	(void)at;
	rfcomm_write_at_msg(fd, AT_TYPE_RESP, NULL, "OK");
}

/**
 * SET: Bluetooth Codec Selection */
static void rfcomm_handler_bcs_set(struct ba_transport *t, struct bt_at *at, int fd) {
	(void)t;
	debug("Selected codec: %u", atoi(at->value));
	rfcomm_write_at_msg(fd, AT_TYPE_RESP, NULL, "OK");
}

/**
 * SET: Bluetooth Available Codecs */
static void rfcomm_handler_bac_set(struct ba_transport *t, struct bt_at *at, int fd) {
	(void)t;
	/* In case some headsets send BAC even if we don't advertise
	 * support for it. In such case, just OK and ignore. */
	debug("Supported codecs: %s", at->value);
	rfcomm_write_at_msg(fd, AT_TYPE_RESP, NULL, "OK");
}

/**
 * SET: Apple Ext: Report a headset state change */
static void rfcomm_handler_iphoneaccev_set(struct ba_transport *t, struct bt_at *at, int fd) {

	char *ptr = at->value;
	size_t count = atoi(strsep(&ptr, ","));
	char tmp;

	while (count-- && ptr != NULL)
		switch (tmp = *strsep(&ptr, ",")) {
		case '1':
			if (ptr != NULL)
				t->device->xapl.accev_battery = atoi(strsep(&ptr, ","));
				bluealsa_event();
			break;
		case '2':
			if (ptr != NULL)
				t->device->xapl.accev_docked = atoi(strsep(&ptr, ","));
			break;
		default:
			warn("Unsupported IPHONEACCEV key: %c", tmp);
			strsep(&ptr, ",");
		}

	rfcomm_write_at_msg(fd, AT_TYPE_RESP, NULL, "OK");
}

/**
 * SET: Apple Ext: Enable custom AT commands from an accessory */
static void rfcomm_handler_xapl_set(struct ba_transport *t, struct bt_at *at, int fd) {

	const char *resp = "+XAPL=BlueALSA,0";
	unsigned int vendor, product;
	unsigned int version, features;

	if (sscanf(at->value, "%x-%x-%u,%u", &vendor, &product, &version, &features) == 4) {
		t->device->xapl.vendor_id = vendor;
		t->device->xapl.product_id = product;
		t->device->xapl.version = version;
		t->device->xapl.features = features;
	}
	else {
		warn("Invalid XAPL value: %s", at->value);
		resp = "ERROR";
	}

	rfcomm_write_at_msg(fd, AT_TYPE_RESP, NULL, resp);
}

static struct {
	const char *command;
	enum bt_at_type type;
	rfcomm_callback *callback;
} handlers[] = {
	{ "", AT_TYPE_RESP, rfcomm_handler_resp },
	{ "+CIND", AT_TYPE_CMD_TEST, rfcomm_handler_cind_test },
	{ "+CIND", AT_TYPE_CMD_GET, rfcomm_handler_cind_get },
	{ "+CIND", AT_TYPE_RESP, rfcomm_handler_cind_resp },
	{ "+CMER", AT_TYPE_CMD_SET, rfcomm_handler_cmer_set },
	{ "+BRSF", AT_TYPE_CMD_SET, rfcomm_handler_brsf_set },
	{ "+BRSF", AT_TYPE_RESP, rfcomm_handler_brsf_resp },
	{ "+VGM", AT_TYPE_CMD_SET, rfcomm_handler_vgm_set },
	{ "+VGS", AT_TYPE_CMD_SET, rfcomm_handler_vgs_set },
	{ "+BTRH", AT_TYPE_CMD_GET, rfcomm_handler_btrh_get },
	{ "+BCS", AT_TYPE_CMD_SET, rfcomm_handler_bcs_set },
	{ "+BAC", AT_TYPE_CMD_SET, rfcomm_handler_bac_set },
	{ "+IPHONEACCEV", AT_TYPE_CMD_SET, rfcomm_handler_iphoneaccev_set },
	{ "+XAPL", AT_TYPE_CMD_SET, rfcomm_handler_xapl_set },
};

/**
 * Get callback (if available) for given AT message. */
static rfcomm_callback *rfcomm_get_callback(const struct bt_at *at) {

	size_t i;

	for (i = 0; i < sizeof(handlers) / sizeof(*handlers); i++) {
		if (handlers[i].type != at->type)
			continue;
		if (strcmp(handlers[i].command, at->command) != 0)
			continue;
		return handlers[i].callback;
	}

	return NULL;
}

void *rfcomm_thread(void *arg) {
	struct ba_transport *t = (struct ba_transport *)arg;

	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
	pthread_cleanup_push(transport_pthread_cleanup, t);

	/* initialize variables used for synchronization */
	t->rfcomm.hfp_slcs = HFP_SLC_BRSF_SET;
	t->rfcomm.mic_gain = t->rfcomm.sco->sco.mic_gain;
	t->rfcomm.spk_gain = t->rfcomm.sco->sco.spk_gain;

	/* XXX: Currently we do not support codec negotiation. */
	t->rfcomm.sco->codec = HFP_CODEC_CVSD;

	struct pollfd pfds[] = {
		{ t->event_fd, POLLIN, 0 },
		{ t->bt_fd, POLLIN, 0 },
	};

	debug("Starting RFCOMM loop: %s",
			bluetooth_profile_to_string(t->profile, t->codec));
	for (;;) {

		/* During normal operation, RFCOMM should block indefinitely. However,
		 * in the HFP-HF mode, service level connection has to be initialized
		 * by ourself. In order to do this reliably, we have to assume, that
		 * AG might not receive our message and will not send proper response.
		 * Hence, we will incorporate timeout, after which we will send our
		 * AT command once more. */
		int timeout = -1;

		rfcomm_callback *callback;
		struct bt_at at;
		char buffer[256];

		/* HFP-HF service level connection finite-state machine. */
		if (t->profile == BLUETOOTH_PROFILE_HFP_HF && t->rfcomm.hfp_slcs != HFP_CONNECTED) {
			timeout = 1000;
			switch (t->rfcomm.hfp_slcs) {
			case HFP_SLC_BRSF_SET:
				sprintf(buffer, "%u", BA_HFP_HF_FEATURES);
				rfcomm_write_at_msg(pfds[1].fd, AT_TYPE_CMD_SET, "+BRSF", buffer);
				break;
			case HFP_SLC_BRSF_SET_OK:
				break;
			case HFP_SLC_CIND_TEST:
				rfcomm_write_at_msg(pfds[1].fd, AT_TYPE_CMD_TEST, "+CIND", NULL);
				break;
			case HFP_SLC_CIND_TEST_OK:
				break;
			case HFP_SLC_CIND_GET:
				rfcomm_write_at_msg(pfds[1].fd, AT_TYPE_CMD_GET, "+CIND", NULL);
				break;
			case HFP_SLC_CIND_GET_OK:
				break;
			case HFP_SLC_CMER_SET:
				/* Deactivate indicator events reporting. The +CMER specification is
				 * as follows: AT+CMER=[<mode>[,<keyp>[,<disp>[,<ind>[,<bfr>]]]]] */
				rfcomm_write_at_msg(pfds[1].fd, AT_TYPE_CMD_SET, "+CMER", "3,0,0,0,0");
				break;
			case HFP_CONNECTED:
				break;
			}
		}

		pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);

		switch (poll(pfds, sizeof(pfds) / sizeof(*pfds), timeout)) {
		case 0:
			debug("RFCOMM poll timeout");
			continue;
		case -1:
			error("Transport poll error: %s", strerror(errno));
			goto fail;
		}

		pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);

		if (pfds[0].revents & POLLIN) {
			/* dispatch incoming event */

			eventfd_t event;
			eventfd_read(pfds[0].fd, &event);

			if (t->rfcomm.mic_gain != t->rfcomm.sco->sco.mic_gain) {
				int gain = t->rfcomm.mic_gain = t->rfcomm.sco->sco.mic_gain;
				debug("Setting microphone gain: %d", gain);
				sprintf(buffer, "+VGM=%d", gain);
				rfcomm_write_at_msg(pfds[1].fd, AT_TYPE_RESP, NULL, buffer);
			}
			if (t->rfcomm.spk_gain != t->rfcomm.sco->sco.spk_gain) {
				int gain = t->rfcomm.spk_gain = t->rfcomm.sco->sco.spk_gain;
				debug("Setting speaker gain: %d", gain);
				sprintf(buffer, "+VGS=%d", gain);
				rfcomm_write_at_msg(pfds[1].fd, AT_TYPE_RESP, NULL, buffer);
			}

			continue;
		}

		if (read(pfds[1].fd, buffer, sizeof(buffer)) == -1) {
			switch (errno) {
			case ECONNABORTED:
			case ECONNRESET:
			case ENOTCONN:
			case ETIMEDOUT:
				/* exit the thread upon socket disconnection */
				debug("RFCOMM disconnected: %s", strerror(errno));
				transport_set_state(t, TRANSPORT_ABORTED);
				goto fail;
			default:
				error("RFCOMM read error: %s", strerror(errno));
				continue;
			}
		}

		/* parse AT message received from the RFCOMM */
		if (at_parse(buffer, &at) == NULL) {
			warn("Invalid AT message: %s", buffer);
			continue;
		}

		if ((callback = rfcomm_get_callback(&at)) != NULL)
			callback(t, &at, pfds[1].fd);
		else {
			warn("Unsupported AT message: %s", buffer);
			if (at.type != AT_TYPE_RESP)
				rfcomm_write_at_msg(pfds[1].fd, AT_TYPE_RESP, NULL, "ERROR");
		}

	}

fail:
	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
	pthread_cleanup_pop(1);
	return NULL;
}