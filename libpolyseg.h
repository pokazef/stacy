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


/* Types */

typedef double osc_clock;
typedef double osc_funcparm;
typedef float osc_sample;

typedef struct
{
	osc_clock time;   /* Absolute time of segment lower bound    */
	
	osc_funcparm p0;  /* Position     \                          */
	osc_funcparm p1;  /* Speed         | -> Quadratic polynomial */
	osc_funcparm p2;  /* Acceleration /                          */
}
osc_segdef;

#define QSIZE 256
#define QMASK 0xff

typedef struct
{
	osc_clock stime;
	osc_segdef state;
	osc_segdef queue[QSIZE];
	int qin, qout;
}
osc_stream;

/* Functions */

void osc_init (int sample_rate);
osc_stream *osc_new_stream (void);
osc_clock osc_time_dependency (osc_stream *s, int num_samples);
void osc_update_stream (osc_stream *s, osc_segdef segment);
void osc_render_stream (osc_stream *s, int num_samples, osc_sample *buffer);
