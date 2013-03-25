#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h> // memcpy
#include <unistd.h> // getopt
#include <netinet/tcp.h> // setsockopt
#include <ev.h>
#include <jack/jack.h>
#include <lame/lame.h>
#include <librtmp/rtmp.h>
#include "flv.h"




struct ev_loop *loop;
struct ev_signal sigint_sig;
static ev_async async;
jack_client_t *client;
jack_port_t *output_port;
lame_global_flags *mp3;
RTMP *rtmp_o;
RTMPPacket rtmp_r;
RTMPPacket rtmp_w;
int rtmp_i;
unsigned char *buffer;
int buffer_size = 30000;
int buffer_writed = 0;
int buffer_samples;


static char *put_amf_string(char *pb, const char *str) {
	char *pb2 = pb;
	uint16_t len = strlen(str);
	pb2[1] = len & 0xff;
	pb2[0] = len >> 8;
	pb2 += 2;
	strcpy(pb2, str);
	pb2 += len;
	return pb2;
}

static void sigint_cb (struct ev_loop *loop, struct ev_signal *w, int revents) {
	jack_client_close(client);
	if (RTMP_IsConnected(rtmp_o)) RTMP_Close(rtmp_o);
	//exit(EXIT_SUCCESS);
	ev_unloop(loop, EVUNLOOP_ALL);
}

void jack_shutdown (void *arg) {
	exit(1);
}

int jack_callback (jack_nframes_t nframes, void *arg) {
	int jack_bsize = nframes * sizeof(jack_default_audio_sample_t);
	jack_default_audio_sample_t *jack_b = jack_port_get_buffer(output_port, nframes);
	memcpy(buffer + buffer_writed, jack_b, jack_bsize);
	buffer_writed += jack_bsize;
	if (buffer_writed + jack_bsize < buffer_size) {
		return 0;
	}
	buffer_samples = buffer_writed >> 2;
	buffer_writed = 0;
	if ( !rtmp_i ) {
		return 0;
	}
	ev_async_send(loop, &async);
	return 0;
}


static void send_cb(EV_P_ ev_async *w, int revents) {
	fprintf(stderr, "sending %d bytes of data\n", buffer_samples << 2);
	int rtmp_size = RTMP_MAX_HEADER_SIZE + 1 + 7200 + buffer_samples * 5 / 4;
	unsigned char *rtmp_buffer = malloc(rtmp_size);
	int mp3size = lame_encode_buffer_ieee_float(mp3, (const float *) buffer, NULL, buffer_samples, rtmp_buffer + RTMP_MAX_HEADER_SIZE + 1, rtmp_size - RTMP_MAX_HEADER_SIZE - 1);
	if ( mp3size < 0 ) {
		fprintf(stderr, "mp3 encoding error %d . buffer_samples %d\n", mp3size, buffer_samples);
	} else {
		rtmp_buffer[RTMP_MAX_HEADER_SIZE] = FLV_CODECID_MP3 | FLV_SAMPLERATE_44100HZ | FLV_SAMPLESSIZE_16BIT | FLV_MONO;
		rtmp_w.m_headerType = RTMP_PACKET_SIZE_LARGE;
		rtmp_w.m_nChannel = 0x04; // source channel
		rtmp_w.m_packetType = RTMP_PACKET_TYPE_AUDIO;
		rtmp_w.m_nInfoField2 = rtmp_o->m_stream_id;
		rtmp_w.m_body = (char *) rtmp_buffer + RTMP_MAX_HEADER_SIZE;
		rtmp_w.m_nBodySize = mp3size + 1;
		if ( !RTMP_SendPacket(rtmp_o, &rtmp_w, FALSE) ) { // queue учитывается только для INVOKE
			perror("RTMP_SendPacket error\n");
		}
	}
	free(rtmp_buffer);
}


int main (int argc, char **argv) {
	char *url;
	int brate = 32;
	int c;
	while ((c = getopt(argc, argv, "u:b:")) != -1)
		switch (c) {
			case 'u':
				url = strdup(optarg);
				break;
			case 'b':
				brate = atoi(optarg);
				break;
			default:
				abort();
		}
// libev
	loop = ev_default_loop(0);
	ev_signal_init(&sigint_sig, sigint_cb, SIGINT);
	ev_signal_start (loop, &sigint_sig);
	ev_async_init(&async, send_cb);
	ev_async_start(loop, &async);

// jack
	jack_status_t status;
	buffer = malloc(buffer_size);
	client = jack_client_open("jack2rtmp", JackNullOption, &status);
	if (client == NULL) {
		perror("jack server not running?\n");
		return EXIT_FAILURE;
	}
	jack_on_shutdown(client, jack_shutdown, NULL);
	jack_set_process_callback(client, jack_callback, NULL);
	//jack_set_latency_callback(client, jack_latency_callback, NULL);
	output_port = jack_port_register(client, "1", JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput, 0);
	//jack_port_register(client, "2", JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput, 0);
	if (jack_activate(client)) {
		perror("cannot activate client");
	}

// lame
	mp3 = lame_init();
	lame_set_num_channels(mp3, 1);
	lame_set_mode(mp3, MONO);
	lame_set_in_samplerate(mp3, 48000);
	lame_set_out_samplerate(mp3, 44100);
	lame_set_quality(mp3, 2); // 0=best (very slow), 9=worst.
	lame_set_brate(mp3, brate);
	lame_init_params(mp3);

// rtmp
	rtmp_o = RTMP_Alloc();
	if (rtmp_o == NULL) {
		perror("RTMP_Alloc\n");
		return EXIT_FAILURE;
	}
	RTMP_Init(rtmp_o);
	RTMP_SetupURL(rtmp_o, url);
	RTMP_EnableWrite(rtmp_o);
	if (!RTMP_Connect(rtmp_o, NULL) || !RTMP_ConnectStream(rtmp_o, 0)) {
		RTMP_Free(rtmp_o);
		perror("Can not connect.\n");
		return EXIT_FAILURE;
	} else {
		perror("Connected.\n");
	}

	char *rtmp_buffer = malloc(4096);
	char *rtmp_top = rtmp_buffer + RTMP_MAX_HEADER_SIZE;
	char *outend = rtmp_buffer + 4096;
	*rtmp_top = (uint8_t) AMF_STRING;
	rtmp_top++;
	rtmp_top = put_amf_string(rtmp_top, "@setDataFrame");
	*rtmp_top = (uint8_t) AMF_STRING;
	rtmp_top++;
	rtmp_top = put_amf_string(rtmp_top, "onMetaData");

	// описание объекта
	*rtmp_top = (uint8_t) 8;
	rtmp_top++;
	rtmp_top = AMF_EncodeInt32(rtmp_top, outend, 8);
	// содержимое объекта
	rtmp_top = put_amf_string(rtmp_top, "duration");
	rtmp_top = AMF_EncodeNumber(rtmp_top, outend, 0);
	rtmp_top = put_amf_string(rtmp_top, "audiodatarate");
	rtmp_top = AMF_EncodeNumber(rtmp_top, outend, brate);
	rtmp_top = put_amf_string(rtmp_top, "audiosamplerate");
	rtmp_top = AMF_EncodeNumber(rtmp_top, outend, 44100);
	rtmp_top = put_amf_string(rtmp_top, "audiosamplesize");
	rtmp_top = AMF_EncodeNumber(rtmp_top, outend, 16);
	rtmp_top = put_amf_string(rtmp_top, "stereo");
	rtmp_top = AMF_EncodeBoolean(rtmp_top, outend, FALSE);
	rtmp_top = put_amf_string(rtmp_top, "audiocodecid");
	rtmp_top = AMF_EncodeNumber(rtmp_top, outend, 2);
	rtmp_top = put_amf_string(rtmp_top, "encoder");
	*rtmp_top = (uint8_t) AMF_STRING;
	rtmp_top++;
	rtmp_top = put_amf_string(rtmp_top, "Lavf53.21.1");
	rtmp_top = put_amf_string(rtmp_top, "filesize");
	rtmp_top = AMF_EncodeNumber(rtmp_top, outend, 0);
	// конец объекта
	rtmp_top = put_amf_string(rtmp_top, "");
	*rtmp_top = (uint8_t) AMF_OBJECT_END;
	rtmp_top++;

	rtmp_w.m_headerType = RTMP_PACKET_SIZE_LARGE;
	rtmp_w.m_nChannel = 0x04; // source channel
	rtmp_w.m_nBodySize = rtmp_top - rtmp_buffer - RTMP_MAX_HEADER_SIZE;
	rtmp_w.m_packetType = RTMP_PACKET_TYPE_INFO;
	rtmp_w.m_nInfoField2 = rtmp_o->m_stream_id;
	rtmp_w.m_body = rtmp_buffer + RTMP_MAX_HEADER_SIZE;
	if ( !RTMP_SendPacket(rtmp_o, &rtmp_w, TRUE) ) {
		perror("RTMP_SendPacket error\n");
	}
	free(rtmp_buffer);


	int off = 0;
	setsockopt(RTMP_Socket(rtmp_o), SOL_TCP, TCP_NODELAY, &off, sizeof(off));




	rtmp_i = TRUE;
	ev_loop(loop, 0);


	//jack_client_close(client);
	return EXIT_SUCCESS;
}


