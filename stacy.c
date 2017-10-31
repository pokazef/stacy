/*
**    This file is part of Stacy, the algebraic audio workstation.
**    Copyright (C) 2013-2014 Mikael Bouillot
**
**    Stacy is free software: you can redistribute it and/or modify
**    it under the terms of the GNU General Public License as published by
**    the Free Software Foundation, either version 3 of the License, or
**    (at your option) any later version.
**
**    Stacy is distributed in the hope that it will be useful,
**    but WITHOUT ANY WARRANTY; without even the implied warranty of
**    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
**    GNU General Public License for more details.
**
**    You should have received a copy of the GNU General Public License
**    along with Stacy.  If not, see <http://www.gnu.org/licenses/>.
*/

/********************************************************************
** For documentation, please see http://www.corbac.com/page44.html **
********************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <alsa/asoundlib.h>
#include <assert.h>
#include <sys/stat.h>
#include "libpolyseg.h"

#define VERSION "0.1.4b"

#define SAMPLE_RATE 48000
#define MAX_COMP_ARGS 8

// Odroid-U2: We get xruns while reading the micro-SD if less than 256.
//            Otherwise, 64 is fine.
#define PSIZE 60

#define BB_OVERSAMPLE 6
#define BB_SIZE (PSIZE / BB_OVERSAMPLE)

// ALSA
snd_seq_t *seq;
int a1port, a2port;
snd_seq_event_t *evp, aev;

int input[19][9];
int output[19][9];

typedef float sig_audio;

sig_audio silence [PSIZE];
char empty_array [8][8];
int silence_bb [BB_SIZE];

unsigned long session_timer = 0;
unsigned long dump_timer = 0;

//---------

enum COLOR
{
	C_BLACK = 0x00,
	C_ORANGE = 0x23,
	C_RED = 0x03,
	C_GREEN = 0x30,
	C_YELLOW = 0x32
};

enum STATE
{
	S_DEFAULT = 0,
	S_USER,
	S_INSTANTIATE,
	S_INSTANTIATE2,
	S_INSTANTIATE3,
	S_DELETE,
	S_UTIL
};

//==============================================================================
// Signal buffers

typedef enum
{
	SIG_ERROR,
	SIG_AUDIO,
	SIG_UI,
	SIG_BYTEBEAT,
	SIG_PAIR
} sig_type;

typedef struct
{
	sig_type type;
	int size;	// In bytes, including this header
} sig_head;

typedef sig_audio *sig_t_audio;	// FIXME: refs in the mallocs

typedef char t_line[8];
typedef t_line *sig_t_ui;		// FIXME: refs in the mallocs

typedef int *sig_t_bytebeat;	// FIXME: refs in the mallocs

// Instance graph

typedef struct {
	int x;
	int y;
} coord;

typedef sig_head *(*compop) (sig_head **, void **);

typedef struct component
{
	int empty;
	coord p;
	compop op;
	int num_inputs;
} component;

typedef struct instance
{
	int empty;
	component c;
	coord inputs[MAX_COMP_ARGS];
	void *state;
} instance;

// Global tables
component comp_table[8][8];
instance inst_table[8][64];
sig_head *sig_table[8][64];
sig_head *sig_table2[8][64];

int inst_page;

sig_head sig_error_c;

sig_head *sig_error (void)
{
	return &sig_error_c;
}

sig_head *sig_dup (sig_head *in)
{
	sig_head *out;

	if (in->type == SIG_ERROR)
		out = in;
	else
	{
		out = malloc (in->size);
		memcpy (out, in, in->size);
	}

	return out;
}

void compute_signals (void)
{
	int x, y, a;
	instance *inst;
	sig_head *in[MAX_COMP_ARGS];
	sig_head *out;

	// Compute new buffers
	for (x=0; x<8; x++) for (y=0; y<64; y++)
	{
		inst = &inst_table[x][y];
		if (! inst->empty)
		{
			for (a = 0; a < inst->c.num_inputs; a++)
				in[a] = sig_table [inst->inputs[a].x] [inst->inputs[a].y];

			out = (*(inst->c.op)) (in, &inst->state);
			sig_table2[x][y] = out;
		}
		else
		{
			sig_table2[x][y] = sig_error();
		}
	}

	// Free old buffers
	for (x=0; x<8; x++) for (y=0; y<64; y++)
	{
		if (sig_table[x][y]->type != SIG_ERROR)
			free (sig_table[x][y]);
	}

	// Copy back new buffers
	for (x=0; x<8; x++) for (y=0; y<64; y++)
	{
		sig_table[x][y] = sig_table2[x][y];
	}
}

//==============================================================================
// Launchpad interface

typedef struct {
	coord p;
	int v;
} button_evt;

typedef struct {
	coord p, q;
} zone;

zone z_left = {{1,1}, {8,8}};
zone z_right = {{10,1}, {17,8}};
zone z_lup = {{1,0}, {8,0}};
zone z_lside = {{0,1}, {0,8}};
zone z_rup = {{10,0}, {17,0}};
zone z_rside = {{18,1}, {18,8}};

int in_zone (coord c, zone z)
{
	if (c.x >= z.p.x && c.x <= z.q.x && c.y >= z.p.y && c.y <= z.q.y)
		return 1;
	else
		return 0;
}

int array2pad (coord pos)
{
	return (pos.y * 16) + pos.x;
}

button_evt *get_input (void)
{
	int val;
	int x, y;
	int px, py;
	int pad;
	button_evt *res = NULL;
	snd_seq_event_t *evp;

	val = -1;

	snd_seq_event_input (seq, &evp);

	if (evp == NULL) return NULL;

	if (evp->type == SND_SEQ_EVENT_CONTROLLER)
	{
		val = evp->data.control.value > 0;
		px = evp->data.control.param - 104;
		py = 0;
	}
	else if (evp->type == SND_SEQ_EVENT_NOTEON)
	{
		val = evp->data.note.velocity > 0;
		px = evp->data.note.note % 16;
		py = evp->data.note.note / 16 + 1;
	}
	else return NULL;

//	for (pad=0; pad<2; pad++)
//		if (evp->dest.port == ports[pad])
//			break;

	pad = 0;
	if (evp->dest.port == a1port) pad = 0;
	if (evp->dest.port == a2port) pad = 1;

	switch (pad)
	{
		case 0:
			x = py;
			y = 8 - px;
			break;
		case 1:
			x = 10 + px;
			y = py;
			break;
		default:
			return NULL;
	}

	if (val >= 0)
	{
		res = malloc (sizeof (button_evt));
		// printf ("%d, %d, %d\n", x, y, val);
		res->p.x = x;
		res->p.y = y;
		res->v = val;
	}

	return res;
}

static int output_old[19][9];

void update_output (void)
{
	coord pos;
	int x, y, note;

	for (x=0; x<19; x++) for (y=0; y<9; y++)
	{
		if (output[x][y] != output_old[x][y])
		{
			pos.x = x;
			pos.y = y;

			if (x == 0)
			{
				if (y != 0)
				{
		        	snd_seq_ev_set_controller (&aev, 0, 112-y, output[x][y]);
					snd_seq_ev_set_source (&aev, a2port);
					snd_seq_event_output (seq, &aev);
				}
			}
			else if (x < 9)
			{
				pos.x = 8 - y;
				pos.y = x - 1;

				note = array2pad (pos);
				snd_seq_ev_set_noteon (&aev, 0, note, output[x][y]);

				snd_seq_ev_set_source (&aev, a2port);
				snd_seq_event_output (seq, &aev);
			}
			else if (x > 9)
			{
				if (y == 0)
				{
					if (x != 18)
					{
		        		snd_seq_ev_set_controller (&aev, 0, 104+x-10, output[x][y]);
						snd_seq_ev_set_source (&aev, a1port);
						snd_seq_event_output (seq, &aev);
					}
				}
				else
				{
					pos.x = x - 10;
					pos.y = y - 1;

					note = array2pad (pos);
					snd_seq_ev_set_noteon (&aev, 0, note, output[x][y]);

					snd_seq_ev_set_source (&aev, a1port);
					snd_seq_event_output (seq, &aev);
				}
			}
		}
	}

	snd_seq_drain_output (seq);

	for (x = 0; x < 19; x++) for (y = 0; y < 9; y++)
		output_old[x][y] = output[x][y];
}

void put_color (coord p, int c)
{
	output[p.x][p.y] = c;
}

void display_editor (void)
{
	int x, y;

	for (x=0; x<8; x++) for (y=0; y<8; y++)
	{
		if (comp_table[x][y].empty)
			output[x+1][y+1] = C_BLACK;
		else
			output[x+1][y+1] = C_GREEN;
	}

	for (x=0; x<8; x++) for (y=0; y<8; y++)
	{
		if (inst_table[x][y+inst_page*8].empty)
			output[x+10][y+1] = C_BLACK;
		else
			output[x+10][y+1] = C_GREEN;
	}

	for (y=0; y<8; y++)
		output[18][y+1] = C_BLACK;

	output[18][inst_page+1] = C_YELLOW;
}

//==============================================================================
// ALSA interface (MIDI and audio)

int audio_ok = 0;
snd_pcm_t *handle;

void user_init (void)
{
	int res;

	// MIDI
	snd_seq_open (&seq, "default", SND_SEQ_OPEN_DUPLEX, 0);
	snd_seq_set_client_name (seq, "Stacy");

	a1port   = snd_seq_create_simple_port (seq, "Array 1", SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE | SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ, SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);
	a2port   = snd_seq_create_simple_port (seq, "Array 2", SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE | SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ, SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);
	snd_seq_nonblock (seq, 1);

	snd_seq_connect_from (seq, a1port, 20, 0);
	snd_seq_connect_to (seq, a1port, 20, 0);
	snd_seq_connect_from (seq, a2port, 24, 0);
	snd_seq_connect_to (seq, a2port, 24, 0);

	snd_seq_ev_set_fixed (&aev);
	snd_seq_ev_set_direct (&aev);
	snd_seq_ev_set_dest (&aev, SND_SEQ_ADDRESS_SUBSCRIBERS, 0);

	// Reset controllers
	snd_seq_ev_set_controller (&aev, 0, 0, 0);
	snd_seq_ev_set_source (&aev, a1port);
	snd_seq_event_output (seq, &aev);
	snd_seq_ev_set_source (&aev, a2port);
	snd_seq_event_output (seq, &aev);
	snd_seq_drain_output (seq);

	// Audio
	snd_pcm_hw_params_t *params;

	res = snd_pcm_open (&handle, "hw:0,0", SND_PCM_STREAM_PLAYBACK, 0);

	if (res == 0)
	{
		snd_pcm_hw_params_alloca (&params);

		snd_pcm_hw_params_any (handle, params);
		snd_pcm_hw_params_set_access (handle, params, SND_PCM_ACCESS_RW_INTERLEAVED);
		snd_pcm_hw_params_set_format(handle, params, SND_PCM_FORMAT_S16_LE);
		snd_pcm_hw_params_set_channels (handle, params, 2);
		snd_pcm_hw_params_set_rate (handle, params, SAMPLE_RATE, 0);
		snd_pcm_hw_params_set_period_size (handle, params, PSIZE, 0);
		snd_pcm_hw_params_set_buffer_size (handle, params, PSIZE*4);
		snd_pcm_hw_params (handle, params);

		audio_ok = 1;
	}
	else
	{
		puts ("Warning: could not initialize ALSA audio.");
	}
}

void user_process_audio (void)
{
	sig_audio l,r;
	signed short samples[2*PSIZE];
	int a;
	int res;
	static double time = 0;
	sig_audio *input;
	sig_head *px;

	input = silence;

	if (sig_table[7][0] != NULL && sig_table[7][0]->type == SIG_PAIR)
	{
		px = (sig_table[7][0] + 1);
		if (px->type == SIG_AUDIO)
		{
			input = (sig_audio *) (px + 1);
		}
	}

	for (a = 0; a < PSIZE; a++)
	{
		l = input[a];
		r = l;

		time += 1.0/SAMPLE_RATE;

		// FIXME: why is my waveform upside-down?
		l *= -0.2;
		r *= -0.2;

		if (l < -1) l = -1;
		if (r < -1) r = -1;
		if (l > 1) l = 1;
		if (r > 1) r = 1;

		samples[2*a] = (signed short) (l * 32767);
		samples[2*a+1] = (signed short) (r * 32767);
	}

	if (audio_ok)
	{
		a = PSIZE;
		while (a > 0)
		{
			res = snd_pcm_writei (handle, samples, a);	// Blocking
			if (res == -EAGAIN)
				continue;
			if (res < 0)
			{
				if (res == -EPIPE)
				{
					printf ("xrun\n");
					snd_pcm_prepare (handle);
					snd_pcm_writei (handle, samples, a);
					snd_pcm_writei (handle, samples, a);
					break;
				}
				else
				{
					printf("Bleh: %s\n", snd_strerror (res));
					exit (1);
				}
			}
			a -= res;
		}
	}
	else
	{
		usleep (1000);
	}
}

void user_display_arrays (void)
{
	sig_t_ui arr1, arr2;
	sig_head *px;
	int x, y;

	arr1 = empty_array;
	arr2 = empty_array;

	if (sig_table[7][0] != NULL && sig_table[7][0]->type == SIG_PAIR)
	{
		px = sig_table[7][0] + 1;
		px = ((void *) px) + px->size;

		if (px->type == SIG_PAIR)
		{
			px = px + 1;
			if (px->type == SIG_UI)
			{
				arr1 = (void *) (px + 1);
			}

			px = ((void *) px) + px->size;
			if (px->type == SIG_UI)
			{
				arr2 = (void *) (px + 1);
			}
		}
	}

	for (x=0; x<8; x++) for (y=0; y<8; y++)
	{
		output[x+1][y+1] = arr2[x][y]?C_GREEN:C_BLACK;
		output[x+10][y+1] = arr1[x][y]?C_GREEN:C_BLACK;
	}

	for (y=0; y<8; y++)
		output[18][y+1] = C_BLACK;
}

//==============================================================================
// Generic components

sig_head *op_identity (sig_head *in[], void **state)
{
	sig_head *out;

	out = sig_dup (in[0]);

	return out;
}

#define DELAY 115

typedef struct
{
	int pos;
	sig_head *buf[DELAY];
} delay_state;

sig_head *op_delay (sig_head *in[], void **state)
{
	sig_head *out;
	delay_state *ds;
	int size;
	int a;

	if (! *state)
	{
		*state = malloc (sizeof (delay_state));
		ds = *state;
		ds->pos = 0;
		for (a = 0; a < DELAY; a++)
			ds->buf[a] = sig_error();
	}

	ds = *state;

	out = ds->buf[ds->pos];
	ds->buf[ds->pos] = sig_dup (in[0]);

	ds->pos++;
	if (ds->pos >= DELAY)
		ds->pos = 0;

	return out;
}

sig_head *op_delay_sync (sig_head *in[], void **state)
{
	sig_head *out;
	delay_state *ds;
	int size;
	int a;

	if (! *state)
	{
		*state = malloc (sizeof (delay_state));
		ds = *state;
		ds->pos = 0;
		for (a = 0; a < DELAY; a++)
			ds->buf[a] = sig_error();
	}

	ds = *state;

	out = ds->buf[ds->pos];
	if (ds->pos == 0)
		for (a = 0; a < DELAY; a++)
			ds->buf[a] = sig_dup (in[0]);

	ds->pos++;
	if (ds->pos >= DELAY)
		ds->pos = 0;

	return out;
}

//==============================================================================
// Cartesian product components (AKA "ordered pair", "multiplexer")

sig_head *op_pair (sig_head *in[], void **state)
{
	sig_head *e1, *e2, *out;
	int size;

	e1 = in[0];
	e2 = in[1];

	size = sizeof (sig_head) + e1->size + e2->size;
	out = malloc (size);
	out->type = SIG_PAIR;
	out->size = size;

	memcpy (((void *) out) + sizeof (sig_head), e1, e1->size);
	memcpy (((void *) out) + sizeof (sig_head) + e1->size, e2, e2->size);

	return out;
}

sig_head *op_elem1 (sig_head *in[], void **state)
{
	sig_head *px, *out;
	int size;

	if (in[0]->type != SIG_PAIR)
	{
		out = sig_error();
	}
	else
	{
		px = (((void *) in[0]) + sizeof (sig_head));
		out = sig_dup (px);
	}

	return out;
}

sig_head *op_elem2 (sig_head *in[], void **state)
{
	sig_head *px, *out;
	int size;

	if (in[0]->type != SIG_PAIR)
	{
		out = sig_error();
	}
	else
	{
		px = ((void *) in[0]) + sizeof (sig_head);
		px = ((void *) px) + px->size;
		out = sig_dup (px);
	}

	return out;
}

//==============================================================================
// Audio components

signed short *sample;
int s_size = 0;

void playback_init (void)
{
	FILE *f;

	f = fopen ("Data/sample.raw", "r");

	if (f == NULL)
	{
		puts ("Warning: could not load Data/sample.raw");
		s_size = 1024;
		sample = malloc (s_size * 4);
		bzero (sample, s_size * 4);
		return;
	}

	fseek (f, 0L, SEEK_END);
	s_size = ftell (f) / 4;
	fseek(f, 0L, SEEK_SET);

	sample = malloc (s_size * 4);

	fread (sample, 1, s_size*4, f);

	fclose (f);
}

sig_head *op_playback (sig_head *in[], void **state)
{
	sig_head *out;
	sig_t_audio s;
	int *pos;
	int size;
	int a;

	if (! *state)
	{
		*state = malloc (sizeof (int));
		pos = *state;
		*pos = 0;
	}

	size = sizeof (sig_head) + PSIZE * sizeof (sig_audio);
	out = malloc (size);
	out->type = SIG_AUDIO;
	out->size = size;

	pos = *state;
	s = (void *) (out + 1);

	for (a = 0; a < PSIZE; a++)
	{
		s[a] = (sig_audio) sample[*pos] / 32768;
		*pos += 2;
		if (*pos >= s_size * 2)
			*pos = 0;
	}

	return out;
}

sig_head *op_attenuate (sig_head *in[], void **state)
{
	sig_head *out;
	sig_t_audio s_out;
	int size;
	int a;

	if (in[0]->type != SIG_AUDIO)
	{
		out = sig_error();
	}
	else
	{
		out = sig_dup (in[0]);

		s_out = (void *) (out + 1);

		for (a = 0; a < PSIZE; a++)
		{
			s_out[a] = s_out[a] * 0.7;
		}
	}

	return out;
}

sig_head *op_saturate (sig_head *in[], void **state)
{
	sig_head *out;
	sig_t_audio s_out;
	int size;
	int a;

	if (in[0]->type != SIG_AUDIO)
	{
		out = sig_error();
	}
	else
	{
		out = sig_dup (in[0]);

		s_out = (void *) (out + 1);

		for (a = 0; a < PSIZE; a++)
		{
			s_out[a] = sin (s_out[a]);
		}
	}

	return out;
}

sig_head *op_inverse (sig_head *in[], void **state)
{
	sig_head *out;
	sig_t_audio s_out;
	int size;
	int a;

	if (in[0]->type != SIG_AUDIO)
	{
		out = sig_error();
	}
	else
	{
		out = sig_dup (in[0]);

		s_out = (void *) (out + 1);

		for (a = 0; a < PSIZE; a++)
		{
			s_out[a] = 0.0 - (s_out[a]);
		}
	}

	return out;
}

sig_head *op_add (sig_head *in[], void **state)
{
	sig_head *out;
	sig_t_audio s_in1, s_in2, s_out;
	sig_t_bytebeat ss_in1, ss_in2, ss_out;
	int size;
	int a;

	if (in[0]->type == SIG_AUDIO || in[1]->type == SIG_AUDIO)
	{
		size = sizeof (sig_head) + PSIZE * sizeof (sig_audio);
		out = malloc (size);
		out->type = SIG_AUDIO;
		out->size = size;

		s_out = (void *) (out + 1);
		if (in[0]->type == SIG_AUDIO)
			s_in1 = (void *) (in[0] + 1);
		else
			s_in1 = silence;
		if (in[1]->type == SIG_AUDIO)
			s_in2 = (void *) (in[1] + 1);
		else
			s_in2 = silence;

		for (a = 0; a < PSIZE; a++)
		{
			s_out[a] = s_in1[a] + s_in2[a];
		}
	}
	else if (in[0]->type == SIG_BYTEBEAT || in[1]->type == SIG_BYTEBEAT)
	{
		size = sizeof (sig_head) + BB_SIZE * sizeof (int);
		out = malloc (size);
		out->type = SIG_BYTEBEAT;
		out->size = size;

		ss_out = (void *) (out + 1);
		if (in[0]->type == SIG_BYTEBEAT)
			ss_in1 = (void *) (in[0] + 1);
		else
			ss_in1 = silence_bb;
		if (in[1]->type == SIG_BYTEBEAT)
			ss_in2 = (void *) (in[1] + 1);
		else
			ss_in2 = silence_bb;

		for (a = 0; a < BB_SIZE; a++)
		{
			ss_out[a] = ss_in1[a] + ss_in2[a];
		}
	}
	else
		out = sig_error();

	return out;
}

sig_head *op_mult (sig_head *in[], void **state)
{
	sig_head *out;
	sig_t_audio s_in1, s_in2, s_out;
	sig_t_bytebeat ss_in1, ss_in2, ss_out;
	int size;
	int a;

	if (in[0]->type == SIG_AUDIO || in[1]->type == SIG_AUDIO)
	{
		size = sizeof (sig_head) + PSIZE * sizeof (sig_audio);
		out = malloc (size);
		out->type = SIG_AUDIO;
		out->size = size;

		s_out = (void *) (out + 1);
		if (in[0]->type == SIG_AUDIO)
			s_in1 = (void *) (in[0] + 1);
		else
			s_in1 = silence;
		if (in[1]->type == SIG_AUDIO)
			s_in2 = (void *) (in[1] + 1);
		else
			s_in2 = silence;

		for (a = 0; a < PSIZE; a++)
		{
			s_out[a] = s_in1[a] * s_in2[a];
		}
	}
	else if (in[0]->type == SIG_BYTEBEAT || in[1]->type == SIG_BYTEBEAT)
	{
		size = sizeof (sig_head) + BB_SIZE * sizeof (int);
		out = malloc (size);
		out->type = SIG_BYTEBEAT;
		out->size = size;

		ss_out = (void *) (out + 1);
		if (in[0]->type == SIG_BYTEBEAT)
			ss_in1 = (void *) (in[0] + 1);
		else
			ss_in1 = silence_bb;
		if (in[1]->type == SIG_BYTEBEAT)
			ss_in2 = (void *) (in[1] + 1);
		else
			ss_in2 = silence_bb;

		for (a = 0; a < BB_SIZE; a++)
		{
			ss_out[a] = ss_in1[a] * ss_in2[a];
		}
	}
	else
		out = sig_error();

	return out;
}

#define LOW_SHELF 0.008
#define HIGH_SHELF 0.9

typedef struct
{
	double ay;
	double bx, by;
} eq_state;

sig_head *op_equalizer (sig_head *in[], void **state)
{
	sig_head *out;

	sig_t_audio s_in, s_out;
	sig_audio b_in[PSIZE], b_low[PSIZE], b_high[PSIZE];
	eq_state *ds;
	int size;
	int a;

	if (! *state)
	{
		*state = malloc (sizeof (eq_state));
		ds = *state;
		ds->ay = 0;
		ds->bx = 0;
		ds->by = 0;
	}

	ds = *state;
	if (in[0]->type != SIG_AUDIO)
	{
		out = sig_error();
		ds->ay = 0;
		ds->bx = 0;
		ds->by = 0;
	}
	else
	{
		out = sig_dup (in[0]);
		s_in  = (void *) (in[0] + 1);
		s_out = (void *) (out + 1);

		for (a = 0; a < PSIZE; a++)
		{
			b_in[a] = s_in[a] * 0.5;
		}

		b_low[0] = b_in[0] * LOW_SHELF + ds->ay * (1 - LOW_SHELF);
		for (a = 1; a < PSIZE; a++)
		{
			b_low[a] = b_in[a] * LOW_SHELF + b_low[a-1] * (1 - LOW_SHELF);
		}
		ds->ay = b_low[a-1];

		b_high[0] = HIGH_SHELF * ds->by + HIGH_SHELF * (b_in[0] - ds->bx);
		for (a = 1; a < PSIZE; a++)
		{
			b_high[a] = HIGH_SHELF * b_high[a-1] + HIGH_SHELF * (b_in[a] - b_in[a-1]);
		}
		ds->bx = b_in[a-1];
		ds->by = b_high[a-1];

		for (a = 0; a < PSIZE; a++)
		{
			s_out[a] = b_low[a] * 13.5 + b_high[a] * 3;
		}
	}

	return out;
}

//==============================================================================
// Bytebeat components

sig_head *op_bb_time (sig_head *in[], void **state)
{
	sig_head *out;
	sig_t_bytebeat s_out;
	int size;
	int a;
	int *time;

	if (! *state)
	{
		*state = malloc (sizeof (int));
		time = *state;
		*time = 0;
	}

	time = *state;

	size = sizeof (sig_head) + BB_SIZE * sizeof (int);
	out = malloc (size);
	out->type = SIG_BYTEBEAT;
	out->size = size;

	s_out = (void *) (out + 1);

	for (a = 0; a < BB_SIZE; a++)
	{
		s_out[a] = *time;
		*time = *time + 1;
	}

	return out;
}

sig_head *op_bb_rshift (sig_head *in[], void **state)
{
	sig_head *out;
	sig_t_bytebeat s_out;
	int size;
	int a;

	if (in[0]->type != SIG_BYTEBEAT)
	{
		out = sig_error();
	}
	else
	{
		out = sig_dup (in[0]);

		s_out = (void *) (out + 1);

		for (a = 0; a < BB_SIZE; a++)
		{
			s_out[a] = s_out[a] >> 1;
		}
	}

	return out;
}

sig_head *op_bb_not (sig_head *in[], void **state)
{
	sig_head *out;
	sig_t_bytebeat s_out;
	int size;
	int a;

	if (in[0]->type != SIG_BYTEBEAT)
	{
		out = sig_error();
	}
	else
	{
		out = sig_dup (in[0]);

		s_out = (void *) (out + 1);

		for (a = 0; a < BB_SIZE; a++)
		{
			s_out[a] = ~ s_out[a];
		}
	}

	return out;
}

sig_head *op_bb_or (sig_head *in[], void **state)
{
	sig_head *out;
	sig_t_bytebeat s_in1, s_in2, s_out;
	int size;
	int a;
	int i1, i2, o;

	if (in[0]->type != SIG_BYTEBEAT && in[1]->type != SIG_BYTEBEAT)
	{
		out = sig_error();
	}
	else
	{
		size = sizeof (sig_head) + PSIZE * sizeof (int);
		out = malloc (size);
		out->type = SIG_BYTEBEAT;
		out->size = size;

		s_out = (void *) (out + 1);
		if (in[0]->type == SIG_BYTEBEAT)
			s_in1 = (void *) (in[0] + 1);
		else
			s_in1 = silence_bb;
		if (in[1]->type == SIG_BYTEBEAT)
			s_in2 = (void *) (in[1] + 1);
		else
			s_in2 = silence_bb;

		for (a = 0; a < BB_SIZE; a++)
		{
			s_out[a] = s_in1[a] | s_in2[a];
		}
	}

	return out;
}

sig_head *op_bb_and (sig_head *in[], void **state)
{
	sig_head *out;
	sig_t_bytebeat s_in1, s_in2, s_out;
	int size;
	int a;
	int i1, i2, o;

	if (in[0]->type != SIG_BYTEBEAT && in[1]->type != SIG_BYTEBEAT)
	{
		out = sig_error();
	}
	else
	{
		size = sizeof (sig_head) + BB_SIZE * sizeof (int);
		out = malloc (size);
		out->type = SIG_BYTEBEAT;
		out->size = size;

		s_out = (void *) (out + 1);
		if (in[0]->type == SIG_BYTEBEAT)
			s_in1 = (void *) (in[0] + 1);
		else
			s_in1 = silence_bb;
		if (in[1]->type == SIG_BYTEBEAT)
			s_in2 = (void *) (in[1] + 1);
		else
			s_in2 = silence_bb;

		for (a = 0; a < BB_SIZE; a++)
		{
			s_out[a] = s_in1[a] & s_in2[a];
		}
	}

	return out;
}

sig_head *op_bb_xor (sig_head *in[], void **state)
{
	sig_head *out;
	sig_t_bytebeat s_in1, s_in2, s_out;
	int size;
	int a;
	int i1, i2, o;

	if (in[0]->type != SIG_BYTEBEAT && in[1]->type != SIG_BYTEBEAT)
	{
		out = sig_error();
	}
	else
	{
		size = sizeof (sig_head) + PSIZE * sizeof (int);
		out = malloc (size);
		out->type = SIG_BYTEBEAT;
		out->size = size;

		s_out = (void *) (out + 1);
		if (in[0]->type == SIG_BYTEBEAT)
			s_in1 = (void *) (in[0] + 1);
		else
			s_in1 = silence_bb;
		if (in[1]->type == SIG_BYTEBEAT)
			s_in2 = (void *) (in[1] + 1);
		else
			s_in2 = silence_bb;

		for (a = 0; a < BB_SIZE; a++)
		{
			s_out[a] = s_in1[a] ^ s_in2[a];
		}
	}

	return out;
}

sig_head *op_bb_onetwentyeight (sig_head *in[], void **state)
{
	sig_head *out;
	sig_t_bytebeat s_out;
	int size;
	int a;
	int i1, i2, o;

	size = sizeof (sig_head) + BB_SIZE * sizeof (int);
	out = malloc (size);
	out->type = SIG_BYTEBEAT;
	out->size = size;

	s_out = (void *) (out + 1);

	for (a = 0; a < BB_SIZE; a++)
	{
		s_out[a] = 128;
	}

	return out;
}

typedef struct
{
	int value;
	char old_plus;
	char old_minus;
} bb_slider_state;


sig_head *op_bb_slider (sig_head *in[], void **state)
{
	sig_head *out;
	bb_slider_state *ds;
	sig_t_ui s_in;
	sig_t_bytebeat s_out;
	int size;
	int a, x, y;

	double value, rate;

	if (! *state)
	{
		*state = malloc (sizeof (bb_slider_state));
		ds = *state;
		ds->value = 0;
		ds->old_plus = 0;
		ds->old_minus = 0;
	}

	ds = *state;

	size = sizeof (sig_head) + BB_SIZE * sizeof (int);
	out = malloc (size);
	out->type = SIG_BYTEBEAT;
	out->size = size;

	s_out = (void *) (out + 1);
	if (in[0]->type == SIG_UI)
		s_in = (void *) (in[0] + 1);
	else s_in = empty_array;

	if (s_in[0][0] == 1 && ds->old_plus == 0)
		ds->value += 1;
	if (s_in[0][1] == 1 && ds->old_minus == 0)
		ds->value -= 1;

	ds->old_plus = s_in[0][0];
	ds->old_minus = s_in[0][1];

	value = ds->value;

	for (a = 0; a < BB_SIZE; a++)
	{
		s_out[a] = value;
	}

	return out;
}

sig_head *op_bb_audio (sig_head *in[], void **state)
{
	sig_head *out;
	sig_t_audio s_out;
	sig_t_bytebeat s_in;
	int size;
	int a, b;
	sig_audio sample;

	if (in[0]->type != SIG_BYTEBEAT)
	{
		out = sig_error();
	}
	else
	{
		size = sizeof (sig_head) + PSIZE * sizeof (sig_audio);
		out = malloc (size);
		out->type = SIG_AUDIO;
		out->size = size;

		s_out = (void *) (out + 1);
		s_in = (void *) (in[0] + 1);

		for (a = 0; a < BB_SIZE; a++)
		{
			sample = ((sig_audio) (s_in[a] & 0xff)) / 256.0 * 2 - 1;
			for (b = 0; b < BB_OVERSAMPLE; b++)
			{
				s_out[a*BB_OVERSAMPLE+b] = sample;
			}
		}
	}

	return out;
}

//==============================================================================
// UI components

sig_head *op_array_1 (sig_head *in[], void **state)
{
	sig_head *out;
	sig_t_ui s;
	int size;
	int x, y;

	size = sizeof (sig_head) + 8 * sizeof (char[8]);
	out = malloc (size);
	out->type = SIG_UI;
	out->size = size;

	s = (void *) (out + 1);

	for (x=0; x<8; x++) for (y=0; y<8; y++)
	{
		s[x][y] = input[x+10][y+1];
	}

	return out;
}

sig_head *op_array_2 (sig_head *in[], void **state)
{
	sig_head *out;
	sig_t_ui s;
	int size;
	int x, y;

	size = sizeof (sig_head) + 8 * sizeof (char[8]);
	out = malloc (size);
	out->type = SIG_UI;
	out->size = size;

	s = (void *) (out + 1);

	for (x=0; x<8; x++) for (y=0; y<8; y++)
	{
		s[x][y] = input[x+1][y+1];
	}

	return out;
}

sig_head *op_ctrl1 (sig_head *in[], void **state)
{
	sig_head *out;
	sig_t_ui s;
	int size;
	int x, y;

	// FIXME: need to handle variable arrays
	size = sizeof (sig_head) + 8 * sizeof (char[8]);
	out = malloc (size);
	out->type = SIG_UI;
	out->size = size;

	s = (void *) (out + 1);

	for (x=0; x<8; x++) for (y=0; y<8; y++)
	{
		s[x][y] = 0;
	}

	s[0][0] = input[10][0];
	s[0][1] = input[11][0];

	return out;
}

sig_head *op_ctrl2 (sig_head *in[], void **state)
{
	sig_head *out;
	sig_t_ui s;
	int size;
	int x, y;

	// FIXME: need to handle variable arrays
	size = sizeof (sig_head) + 8 * sizeof (char[8]);
	out = malloc (size);
	out->type = SIG_UI;
	out->size = size;

	s = (void *) (out + 1);

	for (x=0; x<8; x++) for (y=0; y<8; y++)
	{
		s[x][y] = 0;
	}

	s[0][0] = input[12][0];
	s[0][1] = input[13][0];

	return out;
}

sig_head *op_ctrl3 (sig_head *in[], void **state)
{
	sig_head *out;
	sig_t_ui s;
	int size;
	int x, y;

	// FIXME: need to handle variable arrays
	size = sizeof (sig_head) + 8 * sizeof (char[8]);
	out = malloc (size);
	out->type = SIG_UI;
	out->size = size;

	s = (void *) (out + 1);

	for (x=0; x<8; x++) for (y=0; y<8; y++)
	{
		s[x][y] = 0;
	}

	s[0][0] = input[14][0];
	s[0][1] = input[15][0];

	return out;
}

sig_head *op_ctrl4 (sig_head *in[], void **state)
{
	sig_head *out;
	sig_t_ui s;
	int size;
	int x, y;

	// FIXME: need to handle variable arrays
	size = sizeof (sig_head) + 8 * sizeof (char[8]);
	out = malloc (size);
	out->type = SIG_UI;
	out->size = size;

	s = (void *) (out + 1);

	for (x=0; x<8; x++) for (y=0; y<8; y++)
	{
		s[x][y] = 0;
	}

	s[0][0] = input[16][0];
	s[0][1] = input[17][0];

	return out;
}

sig_head *op_mirror (sig_head *in[], void **state)
{
	sig_head *out;
	sig_t_ui s_in, s_out;
	int size;
	int x, y;

	if (in[0]->type != SIG_UI)
	{
		out = sig_error();
	}
	else
	{
		size = sizeof (sig_head) + 8 * sizeof (char[8]);
		out = malloc (size);
		out->type = SIG_UI;
		out->size = size;

		s_out = (void *) (out + 1);
		s_in = (void *) (in[0] + 1);

		for (x=0; x<8; x++) for (y=0; y<8; y++)
		{
			s_out[x][y] = s_in[7-x][y];
		}
	}

	return out;
}

typedef struct
{
	char in[8][8];	// sig_t_ui
	char out[8][8];	// sig_t_ui
} toggle_state;

sig_head *op_toggle (sig_head *in[], void **state)
{
	sig_head *out;
	toggle_state *ds;
	sig_t_ui s_in, s_out;
	int size;
	int x, y;

	if (! *state)
	{
		*state = malloc (sizeof (toggle_state));
		ds = *state;
		for (x=0; x<8; x++) for (y=0; y<8; y++)
			ds->in[x][y] = ds->out[x][y] = 0;
	}

	ds = *state;

	size = sizeof (sig_head) + 8 * sizeof (char[8]);
	out = malloc (size);
	out->type = SIG_UI;
	out->size = size;

	s_out = (void *) (out + 1);
	if (in[0]->type == SIG_UI)
		s_in = (void *) (in[0] + 1);
	else
		s_in = empty_array;

	for (x=0; x<8; x++) for (y=0; y<8; y++)
	{
		if (s_in[x][y] != ds->in[x][y])
		{
			if (s_in[x][y] == 1)
				ds->out[x][y] = ! ds->out[x][y];

			ds->in[x][y] = s_in[x][y];
		}

		s_out[x][y] = ds->out[x][y];
	}

	return out;
}

sig_head *op_logic_or (sig_head *in[], void **state)
{
	sig_head *out;
	sig_t_ui s_in1, s_in2, s_out;
	int size;
	int x, y;

	if (in[0]->type != SIG_UI && in[1]->type != SIG_UI)
	{
		out = sig_error();
	}
	else
	{
		size = sizeof (sig_head) + 8 * sizeof (char[8]);
		out = malloc (size);
		out->type = SIG_UI;
		out->size = size;

		s_out = (void *) (out + 1);
		if (in[0]->type == SIG_UI)
			s_in1 = (void *) (in[0] + 1);
		else
			s_in1 = empty_array;
		if (in[1]->type == SIG_UI)
			s_in2 = (void *) (in[1] + 1);
		else
			s_in2 = empty_array;

		for (x=0; x<8; x++) for (y=0; y<8; y++)
		{
			s_out[x][y] = s_in1[x][y] | s_in2[x][y];
		}
	}

	return out;
}

sig_head *op_note_wrap (sig_head *in[], void **state)
{
	sig_head *out;
	sig_t_ui s_in, s_out;
	int size;
	int x, y, note;
	int notes[128];

	if (in[0]->type != SIG_UI)
	{
		out = sig_error();
	}
	else
	{
		size = sizeof (sig_head) + 8 * sizeof (char[8]);
		out = malloc (size);
		out->type = SIG_UI;
		out->size = size;

		s_out = (void *) (out + 1);
		s_in = (void *) (in[0] + 1);

		bzero (notes, sizeof (int[128]));
		bzero (s_out, 8 * sizeof (char[8]));

		for (x=0; x<8; x++) for (y=0; y<8; y++)
		{
			if (s_in[x][y] == 1)
			{
				note = (8 - x) * 3 + (8 - y) * 4 + 46;
				notes[note] = 1;
			}
		}

		for (x=0; x<8; x++) for (y=0; y<8; y++)
		{
			note = (8 - x) * 3 + (8 - y) * 4 + 46;
			if (notes[note] == 1)
			{
				s_out[x][y] = 1;
			}
		}
	}

	return out;
}

sig_head *op_game_of_life (sig_head *in[], void **state)
{
	sig_head *out;
	sig_t_ui s_in, s_out;
	int size;
	int x, y, num;

	if (in[0]->type != SIG_UI)
	{
		out = sig_error();
	}
	else
	{
		size = sizeof (sig_head) + 8 * sizeof (char[8]);
		out = malloc (size);
		out->type = SIG_UI;
		out->size = size;

		s_out = (void *) (out + 1);
		s_in = (void *) (in[0] + 1);

		bzero (s_out, 8 * sizeof (char[8]));

		for (x=0; x<8; x++) for (y=0; y<8; y++)
		{
			num = 0;
			num += s_in[(x+0)&7][(y+1)&7];
			num += s_in[(x+1)&7][(y+1)&7];
			num += s_in[(x+1)&7][(y+0)&7];
			num += s_in[(x+1)&7][(y-1)&7];
			num += s_in[(x+0)&7][(y-1)&7];
			num += s_in[(x-1)&7][(y-1)&7];
			num += s_in[(x-1)&7][(y+0)&7];
			num += s_in[(x-1)&7][(y+1)&7];

			if (s_in[x][y] == 1)
				if (num == 2 || num == 3)
					s_out[x][y] = 1;
				else
					s_out[x][y] = 0;
			else
				if (num == 3)
					s_out[x][y] = 1;
				else
					s_out[x][y] = 0;
		}
	}

	return out;
}

//==============================================================================
// Synthesis components

typedef struct
{
	double time;
} synth_state;

sig_head *op_sine_synth (sig_head *in[], void **state)
{
	sig_head *out;
	synth_state *ds;
	sig_t_ui s_in;
	sig_t_audio s_out, s_offset;
	int size;
	int a, x, y;

	int note;
	double freq, t;

	if (! *state)
	{
		*state = malloc (sizeof (synth_state));
		ds = *state;
		ds->time = 0;
	}

	s_offset = silence;
	if (in[0]->type == SIG_AUDIO)
	{
		s_offset = (void *) (in[0] + 1);
	}

	if (in[1]->type != SIG_UI)
	{
		out = sig_error();
		// BUG: the time should still pass.
	}
	else
	{
		ds = *state;

		size = sizeof (sig_head) + PSIZE * sizeof (sig_audio);
		out = malloc (size);
		out->type = SIG_AUDIO;
		out->size = size;

		s_out = (void *) (out + 1);
		s_in = (void *) (in[1] + 1);

		for (a = 0; a < PSIZE; a++)
			s_out[a] = 0;

		for (x=0; x<8; x++) for (y=0; y<8; y++)
		{
			if (s_in[x][y] == 1)
			{
				note = (8 - x) * 3 + (8 - y) * 4 + 46;
				freq = 440 * exp (log (2) * (note - 69) / 12);
				t = ds->time;

				for (a = 0; a < PSIZE; a++)
				{
					s_out[a] += sin ((t * 2 * M_PI) * freq);
					t += exp (s_offset[a]) / SAMPLE_RATE;
				}
			}
		}

		for (a = 0; a < PSIZE; a++)
			ds->time += exp (s_offset[a]) / SAMPLE_RATE;
	}

	return out;
}

sig_head *op_square_synth (sig_head *in[], void **state)
{
	sig_head *out;
	synth_state *ds;
	sig_t_ui s_in;
	sig_t_audio s_out, s_offset;
	int size;
	int a, x, y;

	int note;
	double freq, t;

	if (! *state)
	{
		*state = malloc (sizeof (synth_state));
		ds = *state;
		ds->time = 0;
	}

	s_offset = silence;
	if (in[0]->type == SIG_AUDIO)
	{
		s_offset = (void *) (in[0] + 1);
	}

	if (in[1]->type != SIG_UI)
	{
		out = sig_error();
	}
	else
	{
		ds = *state;

		size = sizeof (sig_head) + PSIZE * sizeof (sig_audio);
		out = malloc (size);
		out->type = SIG_AUDIO;
		out->size = size;

		s_out = (void *) (out + 1);
		s_in = (void *) (in[1] + 1);

		for (a = 0; a < PSIZE; a++)
			s_out[a] = 0;

		for (x=0; x<8; x++) for (y=0; y<8; y++)
		{
			if (s_in[x][y] == 1)
			{
				note = (8 - x) * 3 + (8 - y) * 4 + 46;
				freq = 440 * exp (log (2) * (note - 69) / 12);
				t = ds->time;

				for (a = 0; a < PSIZE; a++)
				{
					s_out[a] += (sin ((t * 2 * M_PI) * freq) > 0)?1:-1;
					t += exp (s_offset[a]) / SAMPLE_RATE;
				}
			}
		}

		for (a = 0; a < PSIZE; a++)
			ds->time += exp (s_offset[a]) / SAMPLE_RATE;
	}

	return out;
}

sig_head *op_sawtooth_synth (sig_head *in[], void **state)
{
	sig_head *out;
	synth_state *ds;
	sig_t_ui s_in;
	sig_t_audio s_out, s_offset;
	int size;
	int a, x, y;

	int note;
	double freq, t;

	if (! *state)
	{
		*state = malloc (sizeof (synth_state));
		ds = *state;
		ds->time = 0;
	}

	s_offset = silence;
	if (in[0]->type == SIG_AUDIO)
	{
		s_offset = (void *) (in[0] + 1);
	}

	if (in[1]->type != SIG_UI)
	{
		out = sig_error();
	}
	else
	{
		ds = *state;

		size = sizeof (sig_head) + PSIZE * sizeof (sig_audio);
		out = malloc (size);
		out->type = SIG_AUDIO;
		out->size = size;

		s_out = (void *) (out + 1);
		s_in = (void *) (in[1] + 1);

		for (a = 0; a < PSIZE; a++)
			s_out[a] = 0;

		for (x=0; x<8; x++) for (y=0; y<8; y++)
		{
			if (s_in[x][y] == 1)
			{
				note = (8 - x) * 3 + (8 - y) * 4 + 46;
				freq = 440 * exp (log (2) * (note - 69) / 12);
				t = ds->time;

				for (a = 0; a < PSIZE; a++)
				{
					s_out[a] += ((t * freq) - floor (t * freq)) * 2 - 1;
					t += exp (s_offset[a]) / SAMPLE_RATE;
				}
			}
		}

		for (a = 0; a < PSIZE; a++)
			ds->time += exp (s_offset[a]) / SAMPLE_RATE;
	}

	return out;
}

typedef struct
{
	double value;
} slider_state;

#define SLIDE_RATE 0.1

sig_head *op_slider (sig_head *in[], void **state)
{
	sig_head *out;
	slider_state *ds;
	sig_t_ui s_in;
	sig_t_audio s_out;
	int size;
	int a, x, y;

	double value, rate;

	if (! *state)
	{
		*state = malloc (sizeof (slider_state));
		ds = *state;
		ds->value = 0;
	}

	ds = *state;

	size = sizeof (sig_head) + PSIZE * sizeof (sig_audio);
	out = malloc (size);
	out->type = SIG_AUDIO;
	out->size = size;

	s_out = (void *) (out + 1);
	if (in[0]->type == SIG_UI)
		s_in = (void *) (in[0] + 1);
	else s_in = empty_array;

	value = ds->value;
	rate = 0;

	if (s_in[0][0] == 1)
		rate += SLIDE_RATE;
	if (s_in[0][1] == 1)
		rate -= SLIDE_RATE;

	rate = rate / SAMPLE_RATE;

	for (a = 0; a < PSIZE; a++)
	{
		s_out[a] = value;
		value += rate;
	}

	ds->value = value;

	return out;
}

//==============================================================================
// Band-limited synthesis with libpolyseg

#define BUG_AMP 0.79

typedef struct
{	osc_stream *st;
	double time;
	osc_segdef seg;
	int old_notes[128];
	int num_notes;
} osc_synth_state;

sig_head *op_bl_square_synth (sig_head *in[], void **state)
{
	sig_head *out;
	osc_synth_state *ds;
	sig_t_ui s_in;
	sig_t_audio s_out, s_offset;
	float polyseg_buffer[PSIZE];
	int size;
	int a, x, y;

	osc_sample buf[PSIZE];
	osc_clock deadline, next_time, next_period, next_slope, period, time, old_time, start;

	double p0, p1, direction;

	int note, note_change;
	double freq, t;
	double speed;
	int notes[128];

	if (! *state)
	{
		*state = malloc (sizeof (osc_synth_state));
		ds = *state;
		ds->st = osc_new_stream();
		ds->time = 0;
		ds->seg = (osc_segdef) {0, 0, 0, 0};
		//osc_update_stream (ds->seg);
		for (a=0; a<128; a++)
			ds->old_notes[a] = 0;
		ds->num_notes = 0;
	}

	s_offset = silence;
	if (in[0]->type == SIG_AUDIO)
	{
		s_offset = (void *) (in[0] + 1);
	}

	if (in[1]->type != SIG_UI)
	{
		out = sig_error();
	}
	else
	{
		ds = *state;

		size = sizeof (sig_head) + PSIZE * sizeof (sig_audio);
		out = malloc (size);
		out->type = SIG_AUDIO;
		out->size = size;

		s_out = (void *) (out + 1);
		s_in = (void *) (in[1] + 1);

		//********************
		bzero (notes, sizeof (int[128]));
		for (x=0; x<8; x++) for (y=0; y<8; y++)
		{
			if (s_in[x][y] == 1)
			{
				note = (8 - x) * 3 + (8 - y) * 4 + 46;
				notes[note] = 1;
			}
		}

		speed = exp (s_offset[0]);
		start = ds->time + ((double) PSIZE / SAMPLE_RATE);
		deadline = ds->time + ((double) PSIZE / SAMPLE_RATE) * 2;

		p0 = ds->seg.p0;
		note_change = 0;
		for (a=0; a<128; a++)
		{
			if (notes[a] == 1 && ds->old_notes[a] == 0)
			{
				note = a;
				freq = 440 * exp (log (2.00001) * (note - 69) / 12);

				p0 += ((start * freq * speed) - floor (start * freq * speed)) < 0.5 ? BUG_AMP : -BUG_AMP;

				ds->old_notes[a] = 1;
				ds->num_notes++;
				note_change = 1;
			}
			else if (notes[a] == 0 && ds->old_notes[a] == 1)
			{
				note = a;
				freq = 440 * exp (log (2.00001) * (note - 69) / 12);

				p0 -= ((start * freq * speed) - floor (start * freq * speed)) < 0.5 ? BUG_AMP : -BUG_AMP;

				ds->old_notes[a] = 0;
				ds->num_notes--;
				note_change = 1;
			}
		}

		if (note_change)
		{
			// printf ("p0: %f, p1: %f\n", p0, p1);
			ds->seg.p0 = p0;
			ds->seg.time = start;

			if (ds->num_notes == 0 && abs (p0) > 0.0000001)
			{
				puts ("Drift detected, correcting...");
				ds->seg.p0 = 0;
			}

			osc_update_stream (ds->st, ds->seg);
		}

		// FIXME: Two events can be exactly simultaneous!
		next_time = INFINITY;
		for (a=0; a<128; a++)
		{
			if (notes[a] == 1)
			{
				note = a;
				freq = speed * 440 * exp (log (2.00001) * (note - 69) / 12);
				period = 0.5 * 1.0 / freq;
				time = ceil (start / period) * period;
				if (time <= start)
					time = start + period;
				if (time < next_time)
				{
					next_time = time;
					if ((long) ((time / period) + 0.01) % 2 == 0)
						direction = 1;
					else
						direction = -1;
				}
			}
		}

		while (next_time < deadline)
		{
			//printf ("%f %f %f\n", ds->seg.p0, ds->seg.p1, next_slope);
			ds->seg.p0 += 2.0 * BUG_AMP * direction;
			ds->seg.time = next_time;
			start = next_time;

			osc_update_stream (ds->st, ds->seg);
			//printf ("%f %f %f\n", ds->time, ds->seg.p0, ds->seg.p1);

			next_time = INFINITY;
			for (a=0; a<128; a++)
			{
				if (notes[a] == 1)
				{
					note = a;
					freq = speed * 440 * exp (log (2.00001) * (note - 69) / 12);
					period = 0.5 * 1.0 / freq;
					time = ceil (start / period) * period;
					if (time < start + 0.0000000001)
						time = start + period;
					if (time < next_time)
					{
						next_time = time;
						if ((long) ((time / period) + 0.01) % 2 == 0)
							direction = 1;
						else
							direction = -1;
					}
				}
			}
		}

		osc_render_stream (ds->st, PSIZE, polyseg_buffer);
		for (a = 0; a < PSIZE; a++)
		{
			s_out[a] = (sig_audio) polyseg_buffer[a];
		}

		//********************

		//for (a = 0; a < PSIZE; a++)
		//	ds->time += (s_offset[a] + 1.0) / SAMPLE_RATE;
		ds->time += (double) PSIZE / SAMPLE_RATE;
	}

	return out;
}

sig_head *op_bl_sawtooth_synth (sig_head *in[], void **state)
{
	sig_head *out;
	osc_synth_state *ds;
	sig_t_ui s_in;
	sig_t_audio s_out, s_offset;
	float polyseg_buffer[PSIZE];
	int size;
	int a, x, y;

	osc_sample buf[PSIZE];
	osc_clock deadline, next_time, next_period, next_slope, period, time, old_time, start;

	double p0, p1;

	int note, note_change;
	double freq, t;
	double speed;
	int notes[128];

	if (! *state)
	{
		*state = malloc (sizeof (osc_synth_state));
		ds = *state;
		ds->st = osc_new_stream();
		ds->time = 0;
		ds->seg = (osc_segdef) {0, 0, 0, 0};
		//osc_update_stream (ds->seg);
		for (a=0; a<128; a++)
			ds->old_notes[a] = 0;
		ds->num_notes = 0;
	}

	s_offset = silence;
	if (in[0]->type == SIG_AUDIO)
	{
		s_offset = (void *) (in[0] + 1);
	}

	if (in[1]->type != SIG_UI)
	{
		out = sig_error();
	}
	else
	{
		ds = *state;

		size = sizeof (sig_head) + PSIZE * sizeof (sig_audio);
		out = malloc (size);
		out->type = SIG_AUDIO;
		out->size = size;

		s_out = (void *) (out + 1);
		s_in = (void *) (in[1] + 1);

		//********************
		bzero (notes, sizeof (int[128]));
		for (x=0; x<8; x++) for (y=0; y<8; y++)
		{
			if (s_in[x][y] == 1)
			{
				note = (8 - x) * 3 + (8 - y) * 4 + 46;
				notes[note] = 1;
			}
		}

		speed = exp (s_offset[0]);
		start = ds->time + ((double) PSIZE / SAMPLE_RATE);
		deadline = ds->time + ((double) PSIZE / SAMPLE_RATE) * 2;

		p0 = ds->seg.p0 + ds->seg.p1 * (start - ds->seg.time);
		p1 = ds->seg.p1;
		note_change = 0;
		for (a=0; a<128; a++)
		{
			if (notes[a] == 1 && ds->old_notes[a] == 0)
			{
				note = a;
				freq = 440 * exp (log (2.00001) * (note - 69) / 12);

				p0 += BUG_AMP * (((start * freq * speed) - floor (start * freq * speed)) * 2 - 1);
				p1 += BUG_AMP * 2.0 * freq * speed;

				ds->old_notes[a] = 1;
				ds->num_notes++;
				note_change = 1;
			}
			else if (notes[a] == 0 && ds->old_notes[a] == 1)
			{
				note = a;
				freq = 440 * exp (log (2.00001) * (note - 69) / 12);

				p0 -= BUG_AMP * (((start * freq * speed) - floor (start * freq * speed)) * 2 - 1);
				p1 -= BUG_AMP * 2.0 * freq * speed;

				ds->old_notes[a] = 0;
				ds->num_notes--;
				note_change = 1;
			}
		}

		if (note_change)
		{
			// printf ("p0: %f, p1: %f\n", p0, p1);
			ds->seg.p0 = p0;
			ds->seg.p1 = p1;
			ds->seg.time = start;

			if (ds->num_notes == 0 && abs (p0) > 0.0000001)
			{
				puts ("Drift detected, correcting...");
				ds->seg.p0 = 0;
				ds->seg.p1 = 0;
			}

			osc_update_stream (ds->st, ds->seg);
		}

		// FIXME: Two events can be exactly simultaneous!
		next_time = INFINITY;
		for (a=0; a<128; a++)
		{
			if (notes[a] == 1)
			{
				note = a;
				freq = speed * 440 * exp (log (2.00001) * (note - 69) / 12);
				period = 1.0 / freq;
				time = ceil (start / period) * period;
				if (time <= start)
					time = start + period;
				if (time < next_time)
				{
					next_time = time;
				}
			}
		}

		while (next_time < deadline)
		{
			//printf ("%f %f %f\n", ds->seg.p0, ds->seg.p1, next_slope);
			ds->seg.p0 = ds->seg.p0 + ds->seg.p1 * (next_time - ds->seg.time);
			ds->seg.p0 += BUG_AMP * (-2.0);
			ds->seg.time = next_time;

			osc_update_stream (ds->st, ds->seg);
			//printf ("%f %f %f\n", ds->time, ds->seg.p0, ds->seg.p1);

			next_time = INFINITY;
			for (a=0; a<128; a++)
			{
				if (notes[a] == 1)
				{
					note = a;
					freq = speed * 440 * exp (log (2.00001) * (note - 69) / 12);
					period = 1.0 / freq;
					time = ceil (ds->seg.time / period) * period;
					if (time < ds->seg.time + 0.0000000001)
						time = ds->seg.time + period;
					if (time < next_time)
					{
						next_time = time;
					}
				}
			}
		}

		osc_render_stream (ds->st, PSIZE, polyseg_buffer);
		for (a = 0; a < PSIZE; a++)
		{
			s_out[a] = (sig_audio) polyseg_buffer[a];
		}

		//********************

		//for (a = 0; a < PSIZE; a++)
		//	ds->time += (s_offset[a] + 1.0) / SAMPLE_RATE;
		ds->time += (double) PSIZE / SAMPLE_RATE;
	}

	return out;
}

//==============================================================================
// Utility functions

void save_state (void)
{
	int x, y, a;
	instance i;
	FILE *f;

	puts ("save");

	mkdir ("Data", 0777);
	f = fopen ("Data/stacy.save", "w");
	fprintf (f, "Stacy v%s save file\n", VERSION);
	for (y=0; y<64; y++) for (x=0; x<8; x++)
	{
		i = inst_table[x][y];
		if (! i.empty)
		{
			fprintf (f, "(%d %d) 1 ", y, x);
			fprintf (f, "(%d %d) [ ", i.c.p.y, i.c.p.x);
			for (a=0; a<MAX_COMP_ARGS; a++)
			{
				if (a < i.c.num_inputs)
					fprintf (f, "(%d %d) ", i.inputs[a].y, i.inputs[a].x);
				else
					fprintf (f, "(0 0) ");
			}
			fprintf (f, "] \n");
		}
		else
		{
			fprintf (f, "(%d %d) 0 \n", y, x);
		}
	}
	fclose (f);
}

void load_state (void)
{
	FILE *f;
	int x, y, a;
	int px, py, pv;
	instance *i;
	char buf[256];

	puts ("load");

	f = fopen ("Data/stacy.save", "r");
	assert (f);
	fgets (buf, 256, f);
	if (strcmp ("Stacy v0.1.4b save file\n", buf) != 0)
	{
		puts ("Error: wrong save version");
		return;
	}

	bzero (inst_table, sizeof (inst_table));

	for (y=0; y<64; y++) for (x=0; x<8; x++)
	{
		i = &(inst_table[x][y]);
		i->empty = 1;
		i->state = NULL;
	}

	for (y=0; y<64; y++) for (x=0; x<8; x++)
	{
		fscanf (f, "(%d %d) %d ", &py, &px, &pv);
		assert (px == x && py == y);

		if (pv)
		{
			i = &(inst_table[x][y]);
			i->empty = 0;

			fscanf (f, "(%d %d) [ ", &py, &px);
			i->c = comp_table[px][py];

			for (a=0; a<MAX_COMP_ARGS; a++)
			{
				fscanf (f, "(%d %d) ", &py, &px);
				i->inputs[a].x = px;
				i->inputs[a].y = py;
			}
			fscanf (f, "] \n");
		}
	}
	fclose (f);
}

//==============================================================================
// Main function

#define coord_equal(a,b) ((a).x == (b).x && (a).y == (b).y)

#define from_comp(c) ((coord) {(c).x + 1, (c).y + 1})
#define to_comp(c)   ((coord) {(c).x - 1, (c).y - 1})

#define from_inst(c) ((coord) {(c).x + 10, ((c).y - inst_page * 8) + 1})
#define to_inst(c)   ((coord) {(c).x - 10, ((c).y - 1) + inst_page * 8})

int main (int argc, char *argv[])
{
	int x, y;

	printf ("Stacy %s started...\n", VERSION);

	playback_init();
	user_init();
	osc_init (SAMPLE_RATE);

	for (x=0; x<19; x++) for (y=0; y<9; y++)
	{
		input[x][y] = 0;
		output[x][y] = 0;
	}

	for (x=0; x<8; x++) for (y=0; y<8; y++)
	{
		comp_table[x][y].empty = 1;
		comp_table[x][y].p.x = x;
		comp_table[x][y].p.y = y;
	}

	// Line 1: Inputs
	// Playback
	comp_table[0][0].empty = 0;
	comp_table[0][0].num_inputs = 0;
	comp_table[0][0].op = op_playback;

	// Array #1
	comp_table[1][0].empty = 0;
	comp_table[1][0].num_inputs = 0;
	comp_table[1][0].op = op_array_1;

	// Array #2
	comp_table[2][0].empty = 0;
	comp_table[2][0].num_inputs = 0;
	comp_table[2][0].op = op_array_2;

	// Control 1
	comp_table[4][0].empty = 0;
	comp_table[4][0].num_inputs = 0;
	comp_table[4][0].op = op_ctrl1;

	// Control 2
	comp_table[5][0].empty = 0;
	comp_table[5][0].num_inputs = 0;
	comp_table[5][0].op = op_ctrl2;

	// Control 3
	comp_table[6][0].empty = 0;
	comp_table[6][0].num_inputs = 0;
	comp_table[6][0].op = op_ctrl3;

	// Control 4
	comp_table[7][0].empty = 0;
	comp_table[7][0].num_inputs = 0;
	comp_table[7][0].op = op_ctrl4;

	// Line 2: Generic components
	// Identity
	comp_table[0][1].empty = 0;
	comp_table[0][1].num_inputs = 1;
	comp_table[0][1].op = op_identity;

	// Delay
	comp_table[1][1].empty = 0;
	comp_table[1][1].num_inputs = 1;
	comp_table[1][1].op = op_delay;

	// Synchronous delay
	comp_table[2][1].empty = 0;
	comp_table[2][1].num_inputs = 1;
	comp_table[2][1].op = op_delay_sync;

	// Line 3: Cartesian product
	// Pair deconstruction
	comp_table[0][2].empty = 0;
	comp_table[0][2].num_inputs = 1;
	comp_table[0][2].op = op_elem1;

	// Pair deconstruction
	comp_table[1][2].empty = 0;
	comp_table[1][2].num_inputs = 1;
	comp_table[1][2].op = op_elem2;

	// Pair construction
	comp_table[3][2].empty = 0;
	comp_table[3][2].num_inputs = 2;
	comp_table[3][2].op = op_pair;

	// Line 4: Audio components
	// Attenuation
	comp_table[0][3].empty = 0;
	comp_table[0][3].num_inputs = 1;
	comp_table[0][3].op = op_attenuate;

	// Inversion
	comp_table[1][3].empty = 0;
	comp_table[1][3].num_inputs = 1;
	comp_table[1][3].op = op_inverse;

	// Addition
	comp_table[3][3].empty = 0;
	comp_table[3][3].num_inputs = 2;
	comp_table[3][3].op = op_add;

	// Multiplication
	comp_table[4][3].empty = 0;
	comp_table[4][3].num_inputs = 2;
	comp_table[4][3].op = op_mult;

	// Saturation
	comp_table[6][3].empty = 0;
	comp_table[6][3].num_inputs = 1;
	comp_table[6][3].op = op_saturate;

	// Equalizer
	comp_table[7][3].empty = 0;
	comp_table[7][3].num_inputs = 1;
	comp_table[7][3].op = op_equalizer;

	// Line 5: UI components
	// mirror
	comp_table[0][4].empty = 0;
	comp_table[0][4].num_inputs = 1;
	comp_table[0][4].op = op_mirror;

	// toggle
	comp_table[1][4].empty = 0;
	comp_table[1][4].num_inputs = 1;
	comp_table[1][4].op = op_toggle;

	// logic OR
	comp_table[3][4].empty = 0;
	comp_table[3][4].num_inputs = 2;
	comp_table[3][4].op = op_logic_or;

	// Note wrap
	comp_table[5][4].empty = 0;
	comp_table[5][4].num_inputs = 1;
	comp_table[5][4].op = op_note_wrap;

	// Game of Life
	comp_table[7][4].empty = 0;
	comp_table[7][4].num_inputs = 1;
	comp_table[7][4].op = op_game_of_life;

	// Line 6: Synthesizers
	comp_table[0][5].empty = 0;
	comp_table[0][5].num_inputs = 2;
	comp_table[0][5].op = op_sine_synth;

	comp_table[1][5].empty = 0;
	comp_table[1][5].num_inputs = 2;
	comp_table[1][5].op = op_square_synth;

	comp_table[2][5].empty = 0;
	comp_table[2][5].num_inputs = 2;
	comp_table[2][5].op = op_sawtooth_synth;

	comp_table[4][5].empty = 0;
	comp_table[4][5].num_inputs = 2;
	comp_table[4][5].op = op_bl_square_synth;

	comp_table[5][5].empty = 0;
	comp_table[5][5].num_inputs = 2;
	comp_table[5][5].op = op_bl_sawtooth_synth;

	// Line 7: Controlers
	comp_table[0][6].empty = 0;
	comp_table[0][6].num_inputs = 1;
	comp_table[0][6].op = op_slider;

	comp_table[1][6].empty = 0;
	comp_table[1][6].num_inputs = 1;
	comp_table[1][6].op = op_bb_slider;

	// Line 8: Bytebeat
	comp_table[0][7].empty = 0;
	comp_table[0][7].num_inputs = 0;
	comp_table[0][7].op = op_bb_time;

	comp_table[1][7].empty = 0;
	comp_table[1][7].num_inputs = 1;
	comp_table[1][7].op = op_bb_rshift;

	comp_table[2][7].empty = 0;
	comp_table[2][7].num_inputs = 1;
	comp_table[2][7].op = op_bb_not;

	comp_table[3][7].empty = 0;
	comp_table[3][7].num_inputs = 2;
	comp_table[3][7].op = op_bb_or;

	comp_table[4][7].empty = 0;
	comp_table[4][7].num_inputs = 2;
	comp_table[4][7].op = op_bb_and;

	comp_table[5][7].empty = 0;
	comp_table[5][7].num_inputs = 2;
	comp_table[5][7].op = op_bb_xor;

	comp_table[6][7].empty = 0;
	comp_table[6][7].num_inputs = 0;
	comp_table[6][7].op = op_bb_onetwentyeight;

	comp_table[7][7].empty = 0;
	comp_table[7][7].num_inputs = 1;
	comp_table[7][7].op = op_bb_audio;

	for (x=0; x<8; x++) for (y=0; y<64; y++)
	{
		inst_table[x][y].empty = 1;
	}

	display_editor();

	sig_error_c.type = SIG_ERROR;
	sig_error_c.size = sizeof (sig_head);

	for (x=0; x<8; x++) for (y=0; y<64; y++)
	{
		sig_table[x][y] = sig_error();
	}

	inst_page = 0;

	int a;
	button_evt *evx;
	int ex, ey, ev;
	coord ec;
	int state = S_DEFAULT;
	component comp;
	instance inst;
	int current_input;
	sig_t_bytebeat px;
	int bbv;

	for (;;)
	{
		session_timer++;
		dump_timer++;

		user_process_audio();	// Blocking

		compute_signals();

		evx = get_input();

		// Controlers
		if (evx && in_zone (evx->p, z_rup))
		{
			input[evx->p.x][evx->p.y] = evx->v;
		}

		// Timeline
		if (sig_table[0][0] != NULL && sig_table[0][0]->type == SIG_BYTEBEAT)
		{
			px = (void *) (sig_table[0][0] + 1);
			bbv = px[0];

			bbv = bbv >> 12;
			for (a = 0; a < 15; a++)
			{
				output[17-(a<8?a:(a+1))][0] = bbv&1?C_RED:C_BLACK;
				bbv = bbv >> 1;
			}
		}
		else
		{
			for (a = 0; a < 15; a++)
			{
				output[17-a][0] = C_BLACK;
			}
		}

		if (state == S_USER)
		{
			// User mode

			user_display_arrays();

			while (evx)
			{
				if (evx->p.x == 1 && evx->p.y == 0)
				{
					if (evx->v == 1)
					{
						// Switch to editor mode
						state = S_DEFAULT;
						output[1][0] = C_BLACK;
						display_editor();
					}
				}
				else
				{
					input[evx->p.x][evx->p.y] = evx->v;
				}

				free (evx);
				evx = get_input();
			}
		}

		else if (evx)
		{
			// Editor mode

			ex = evx->p.x;
			ey = evx->p.y;
			ev = evx->v;
			ec = evx->p;

			// Utility mode (safety)
			if (in_zone (ec, z_lup) && ex == 8)
			{
					if (ev == 1)
						state = S_UTIL;
					else
						state = S_DEFAULT;
			}

			// Save, Load and dumps
			if (state == S_UTIL && in_zone (ec, z_lup) && ev == 1)
			{
				if (ex == 2)
				{
					save_state();
				}

				if (ex == 3)
				{
					load_state();
					state = S_DEFAULT;
					inst_page = 0;
					display_editor();
				}
			}

			if (in_zone (ec, z_rside))
			{
				if (ev == 1)
				{
					inst_page = ey - 1;
					display_editor();
				}
			}

			switch (state)
			{
				case S_DEFAULT:
					if (in_zone (ec, z_left) && ev == 1)
					{
						comp = comp_table[ex-1][ey-1];
						if (! comp.empty)
						{
							if (comp.num_inputs > 0)
							{
								put_color (ec, C_ORANGE);
								inst.c = comp;
								inst.empty = 0;
								inst.state = NULL;
								current_input = 0;
								state = S_INSTANTIATE;
							}
							else
							{
								put_color (ec, C_RED);
								inst.c = comp;
								inst.empty = 0;
								inst.state = NULL;
								state = S_INSTANTIATE2;
							}
						}
					}
					else if (in_zone (ec, z_right))
					{
						// Show component inputs
						// FIXME: non re-entrant
						if (! inst_table[ex-10][ey-1+inst_page*8].empty)
						{
							inst = inst_table[ex-10][ey-1+inst_page*8];
							if (ev == 1)
							{
								put_color (from_comp(inst.c.p), C_ORANGE);
								for (a = 0; a < inst.c.num_inputs; a++)
								{
									if (from_inst(inst.inputs[a]).y >= 1 && from_inst(inst.inputs[a]).y <= 8)
										put_color (from_inst(inst.inputs[a]), C_ORANGE);
								}
							}
							else
							{
								put_color (from_comp(inst.c.p), C_GREEN);
								display_editor();
							}
						}
					}
					else if (ex == 0 && ey == 1 && ev == 1)
					{
						//assert (ev == 1);
						output[0][1] = C_RED;
						state = S_DELETE;
					}
					else if (ex == 1 && ey == 0 && ev == 1)
					{
						// Switch to user mode
						output[1][0] = C_YELLOW;
						state = S_USER;
					}
					break;

				case S_INSTANTIATE:
					if (in_zone (ec, z_right))
					{
						if (ev == 1)
						{
							inst.inputs[current_input++] = to_inst(ec);
							put_color (ec, C_ORANGE);
							if (current_input == comp.num_inputs)
							{
								put_color (from_comp(comp.p), C_RED);
								state = S_INSTANTIATE2;
							}
						}
					}
					else if (in_zone (ec, z_left) && coord_equal (ec, from_comp(inst.c.p)) && ev == 0)
					{
						display_editor();
						state = S_DEFAULT;
					}
					break;

				case S_INSTANTIATE2:
					if (in_zone (ec, z_right))
					{
						if (ev == 1)
						{
							inst_table[ex-10][ey-1+inst_page*8] = inst;
							// FIXME: need to free old resources
							put_color (ec, C_RED);
							if (comp.num_inputs > 0)
							{
								put_color (from_comp(inst.c.p), C_ORANGE);
								inst.c = comp;
								inst.empty = 0;
								inst.state = NULL;
								current_input = 0;
								state = S_INSTANTIATE;
							}
							else
							{
								put_color (from_comp(inst.c.p), C_RED);
								inst.c = comp;
								inst.empty = 0;
								inst.state = NULL;
								state = S_INSTANTIATE2;
							}
						}
					}
					else if (in_zone (ec, z_left) && coord_equal (ec, from_comp(inst.c.p)) && ev == 0)
					{
						display_editor();
						state = S_DEFAULT;
					}
					break;

				case S_DELETE:
					if (in_zone (ec, z_right))
					{
						if (ev == 1 && ! inst_table[ex-10][ey-1+inst_page*8].empty)
						{
							put_color (ec, C_RED);
							inst_table[ex-10][ey-1+inst_page*8].empty = 1;
							if (inst_table[ex-10][ey-1+inst_page*8].state)
							{
								free (inst_table[ex-10][ey-1+inst_page*8].state);
								inst_table[ex-10][ey-1+inst_page*8].state = NULL;
							}
						}
						if (ev == 0)
						{
							put_color (ec, C_BLACK);
						}
					}
					else if (ex == 0 && ey == 1)
					{
						assert (ev == 0);
						output[0][1] = C_BLACK;
						state = S_DEFAULT;
						display_editor();
					}
					break;
			}

			free (evx);
		}

		update_output();
	}
}

//==============================================================================
// EOF


