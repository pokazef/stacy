/*
**    This file is part of libpolyseg, the bandlimited chip-sound renderer.
**    Copyright (C) 2011 Mikael Bouillot
**
**    Libpolyseg is free software: you can redistribute it and/or modify
**    it under the terms of the GNU General Public License as published by
**    the Free Software Foundation, either version 3 of the License, or
**    (at your option) any later version.
**
**    Libpolyseg is distributed in the hope that it will be useful,
**    but WITHOUT ANY WARRANTY; without even the implied warranty of
**    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
**    GNU General Public License for more details.
**
**    You should have received a copy of the GNU General Public License
**    along with libpolyseg.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <math.h>
#include "libpolyseg.h"


/********** Anti-aliasing filter **********/

// TODO: test integration quality
#define KUNIT 256
#define KSIZE 32
#define KBUFSIZE (KUNIT*KSIZE+1)

double k0 [KBUFSIZE];
double k1 [KBUFSIZE];
double k2 [KBUFSIZE];

void integrate (double *dst, double *src)
{
	int a;
	double fa;
	
	dst[0] = 0;
	for (a=1; a<KBUFSIZE; a++)
	{
		fa = (src[a] + src[a-1]) / 2;
		fa = fa / KUNIT;
		dst[a] = dst[a-1] + fa;
	}
}

double gk1 (int x)
{
	if (x > 0)
	{
		assert (x < KBUFSIZE);
		return k1[x];
	}
	else
	{
		assert (-x < KBUFSIZE);
		return -(k1[-x]);
	}
}

double gk2 (int x)
{
	if (x > 0)
	{
		assert (x < KBUFSIZE);
		return k2[x];
	}
	else
	{
		assert (-x < KBUFSIZE);
		return k2[-x];
	}
}

void make_kernel (void)
{
	int a;
	double fa;
	
	for (a=0; a<KBUFSIZE; a++)
	{
		fa = (double) a / KUNIT * M_PI * 0.83;
		if (fa == 0)
			k0[a] = 1;
		else
			k0[a] = sin(fa)/fa * exp(-(fa/40)*(fa/40));
	}
	
	integrate (k1, k0);
	integrate (k2, k1);
}

/******************************************/

double sample_rate;

// 2013-09-29: I don't understand why this is needed.
// But without it, triangle waveforms get flattened.
double oversampling_ratio;
#define BUG1 oversampling_ratio

void osc_init (int sr)
{
	sample_rate = sr;
	
	oversampling_ratio = 1.0 / sr;
	
	make_kernel();
}

osc_stream *osc_new_stream (void)
{
	osc_stream *s;
	
	s = malloc (sizeof (osc_stream));
	s->stime = 0;
	
	s->state.time = 0;
	s->state.p0 = 0;
	s->state.p1 = 0;
	s->state.p2 = 0;
	
	s->qin = s->qout = 0;
	
	s->queue[0].time = -((double) KSIZE / sample_rate);
	s->queue[0].p0 = 0;
	s->queue[0].p1 = 0;
	s->queue[0].p2 = 0;
	
	return s;
}

osc_clock osc_time_dependency (osc_stream *s, int num_samples)
{
	osc_clock ret;
	
	assert (sample_rate != 0);
	ret =
		s->stime
		+ (osc_clock) (num_samples + KSIZE) / sample_rate;
	return ret;
}

void osc_update_stream (osc_stream *s, osc_segdef seg)
{
	assert (seg.time >= s->queue[s->qin].time);
	
	s->qin = (s->qin + 1) & QMASK;
	s->queue[s->qin] = seg;
}

void osc_render_stream (osc_stream *s, int samples, osc_sample *buffer)
{
	int sp;
	int seg;
	osc_clock x1, x2, x1k, x2k;
	double res, fx1, fx2, fa;
	
	for (sp=0; sp<samples; sp++)
	{
		s->stime += 1.0 / sample_rate;
		x2 = s->stime + (double) KSIZE / sample_rate;
		seg = s->qin;
		while (s->queue[seg].time > x2)
		{
			seg = (seg - 1) & QMASK;
			assert (seg != s->qin);
		}
		x1 = s->queue[seg].time;
		assert (x1 <= x2);
		
		res = 0;
		
		while (x1 > s->stime - (double) KSIZE / sample_rate)
		{
			x1k = (x1 - s->stime) * sample_rate * KUNIT;
			x2k = (x2 - s->stime) * sample_rate * KUNIT;
			
			fx1 = s->queue[seg].p0;
			fx2 = s->queue[seg].p1 * (x2-x1) + s->queue[seg].p0;
			fa = BUG1*s->queue[seg].p1;
			
			res += fx2*gk1(x2k) - fx1*gk1(x1k) - fa*gk2(x2k) + fa*gk2(x1k);
			
			x2 = x1;
			seg = (seg - 1) & QMASK;
			assert (seg != s->qin);
			x1 = s->queue[seg].time;
		}
		
		x1 = s->stime - (double) KSIZE / sample_rate;
		x1k = (x1 - s->stime) * sample_rate * KUNIT;
		x2k = (x2 - s->stime) * sample_rate * KUNIT;

		fx1 = s->queue[seg].p1 * (x1 - s->queue[seg].time) + s->queue[seg].p0;
		fx2 = s->queue[seg].p1 * (x2 - s->queue[seg].time) + s->queue[seg].p0;
		fa = BUG1*s->queue[seg].p1;
		
		res += fx2*gk1(x2k) - fx1*gk1(x1k) - fa*gk2(x2k) + fa*gk2(x1k);

		buffer[sp] = res;
	}
}
