#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <math.h>
#include <assert.h>
#include "pt2_dump.h"
#include "pt2_header.h"
#include "pt2_paula.h"
#include "pt2_structs.h"

// Paula voice internals - must match the layout in pt2_paula.c
typedef struct voice_t
{
	volatile bool DMA_active;

	int period;
	int volume;

	// internal registers
	bool DMATriggerFlag, nextSampleStage;
	int8_t AUD_DAT[2]; // DMA data buffer
	const int8_t *location; // current location
	uint16_t lengthCounter; // current length
	int32_t sampleCounter; // how many bytes left in AUD_DAT
	double dSample; // currently held sample point (multiplied by volume)
	double dDelta, dPhase;

	// for BLEP synthesis
	double dLastDelta, dLastPhase, dBlepOffset;

	// registers modified by Paula functions
	const int8_t *AUD_LC; // location (data pointer)
	uint16_t AUD_LEN;
	double AUD_PER_delta;
	double AUD_VOL;
} paulaVoice_t;

extern paulaVoice_t paula[PAULA_VOICES];

static bool jsonNeedsClosing = false;
static bool firstTickElement = true;

static void closeJsonOnExit(void)
{
	if (jsonNeedsClosing)
	{
		printf("\n    ]\n}\n");
		fflush(stdout);
		jsonNeedsClosing = false;
	}
}

static void signalHandler(int sig)
{
	closeJsonOnExit();
	exit(sig);
}

// Register atexit/signal handlers for clean JSON closure
static void dumpRegisterCleanupHandlers(void)
{
	static bool handlersRegistered = false;
	if (!handlersRegistered)
	{
		atexit(closeJsonOnExit);
		signal(SIGINT, signalHandler);
		signal(SIGTERM, signalHandler);
#ifdef SIGQUIT
		signal(SIGQUIT, signalHandler);
#endif
		handlersRegistered = true;
	}
}

// Close the JSON output (prints closing brackets and brace)
void dumpClose(void)
{
	if (jsonNeedsClosing)
	{
		printf("\n    ]\n}\n");
		fflush(stdout);
		jsonNeedsClosing = false;
	}
}

// Look up a note index (0-36) from a period value
static int dumpFindNoteByPeriod(int period)
{
	static const int periods[] =
	{
		// finetune +0
		856, 808, 762, 720, 678, 640, 604, 570, 538, 508, 480, 453, // C-1 to B-1
		428, 404, 381, 360, 339, 320, 302, 285, 269, 254, 240, 226, // C-2 to B-2
		214, 202, 190, 180, 170, 160, 151, 143, 135, 127, 120, 113, // C-3 to B-3
		0
	};

	int begin = 0;
	int end = 3 * 12 + 1;
	int note = begin;
	while (note < end && periods[note] > period)
	{
		++note;
	}

	if (note < 0)
	{
		note = 0;
	}

	int minimum_note = 0;
	int maximum_note = 35;
	assert(note >= minimum_note && note <= maximum_note);
	(void)minimum_note;
	(void)maximum_note;
	return note;
}

// Print a comment line for the current row (pattern, position, row)
void dumpTickComment(int pattern, int position, int row)
{
	if (!jsonNeedsClosing)
		return;
	if (!firstTickElement)
		printf(",\n");
	firstTickElement = false;
	printf("            {\"comment\": \"pattern=%d, position=%d, row=%d\"}",
		pattern, position, row
	);
}

// Print tick data for all 4 channels (period, sample, volume, offset)
void dumpTickData(void)
{
	if (!jsonNeedsClosing)
		return;

	if (!firstTickElement)
		printf(",\n");
	firstTickElement = false;

	// Cast the extern paula array to our local struct layout
	paulaVoice_t *p = (paulaVoice_t *)paula;

	printf("            ");
	for (int i = 0; i < PAULA_VOICES; ++i)
	{
		int period = 0;
		int sample = 0;
		int volume = 0;
		int offset = 0;
		if (p[i].DMA_active && p[i].location != NULL && p[i].AUD_LC != NULL && p[i].lengthCounter >= 1)
		{
			// Ignore non-looping samples looping over the two zero samples
			// at the start of the sample.  This is effectively not playing
			// and Amp expects this to be logged as an empty array, i.e.
			// period, sample, volume, and offset are all 0.
			if (p[i].lengthCounter > 2 || p[i].AUD_LEN > 2)
			{
				period = p[i].period;
				volume = p[i].volume;

				// Search through all samples to find which one contains p[i].location
				for (int j = 0; j < MOD_SAMPLES; j++)
				{
					moduleSample_t *s = &song->samples[j];
					if (s->length > 0)
					{
						const int8_t *start = &song->sampleData[s->offset];
						const int8_t *end = start + s->length;
						const int8_t *location = p[i].location - 2 + (1 - p[i].sampleCounter) + (p[i].nextSampleStage ? 1 : 0);
						const int8_t *aud_lc = p[i].AUD_LC;
						if (location >= start && location <= end)
						{
							// If the loop start is outside of the sample then this
							// channel has encountered the special case of a finished
							// sample changing to a different sample with a portamento
							// to tone effect.  This delays the start of the new sample
							// until the next sample is fetched from memory and starts
							// playing that sample from the loop start point.
							if (aud_lc < start || aud_lc > end)
							{
								period = 0;
								sample = 0;
								volume = 0;
								offset = 0;
								break;
							}

							sample = j;
							offset = (int)floorf((double)(location - start) + p[i].dPhase);
							break;
						}
					}
				}
			}
		}
		if (i > 0)
			printf(", ");
		if (period != 0 || volume != 0 || offset != 0)
		{
			printf("[%d, %d, %d, %d]", period, sample + 1, volume, offset);
		}
		else
		{
			printf("[]");
		}
	}
}

static void dumpSamplesJson(void)
{
	// Find last used sample (skip trailing empty samples)
	int lastSample = MOD_SAMPLES - 1;
	while (lastSample >= 0 && song->samples[lastSample].length == 0)
		lastSample--;

	printf("    \"samples\": [\n");
	for (int i = 0; i <= lastSample; i++)
	{
		moduleSample_t *s = &song->samples[i];
		printf("        {\n");
		printf("            \"name\": \"%s\",\n", s->text);
		printf("            \"length\": %d,\n", s->length);
		printf("            \"finetune\": %d,\n", s->fineTune);
		printf("            \"volume\": %d,\n", s->volume);
		printf("            \"loop_start\": %d,\n", s->loopStart);
		printf("            \"loop_length\": %d\n", s->loopLength);
		printf("        }%s\n", i < lastSample ? "," : "");
	}
	printf("    ],\n");
}

static void dumpPatternOrderTableJson(void)
{
	printf("    \"pattern_order_table\": [");
	for (int i = 0; i < song->header.songLength; i++)
	{
		if (i > 0)
			printf(", ");
		printf("%d", song->header.patternTable[i]);
	}
	printf("],\n");
}

static void dumpPatternsJson(void)
{
	static const char *notes[] =
	{
		"c-1", "c#1", "d-1", "d#1", "e-1", "f-1", "f#1", "g-1", "g#1", "a-1", "a#1", "b-1",
		"c-2", "c#2", "d-2", "d#2", "e-2", "f-2", "f#2", "g-2", "g#2", "a-2", "a#2", "b-2",
		"c-3", "c#3", "d-3", "d#3", "e-3", "f-3", "f#3", "g-3", "g#3", "a-3", "a#3", "b-3",
		""
	};

	// Find the highest pattern number referenced in the order table
	int maxPattern = 0;
	for (int i = 0; i < song->header.songLength; i++)
	{
		if (song->header.patternTable[i] > maxPattern)
			maxPattern = song->header.patternTable[i];
	}

	printf("    \"patterns\": [\n");
	for (int pat = 0; pat <= maxPattern; pat++)
	{
		if (song->patterns[pat] == NULL)
		{
			printf("        []%s\n", pat < maxPattern ? "," : "");
			continue;
		}

		printf("        [\n");
		for (int row = 0; row < MOD_ROWS; row++)
		{
			if (row % 16 == 0)
				printf("            {\"comment\": \"pattern=%d, row=%d\"},\n", pat, row);
			printf("            [");
			for (int ch = 0; ch < PAULA_VOICES; ch++)
			{
				note_t *n = &song->patterns[pat][(row * PAULA_VOICES) + ch];
				if (n->period != 0 || n->sample != 0 || n->command != 0 || n->param != 0)
				{
					int noteIndex = dumpFindNoteByPeriod(n->period);
					printf("[\"%s\", %d, \"%x%02x\"]", notes[noteIndex], n->sample, n->command & 0xf, n->param);
				}
				else
				{
					printf("[]");
				}
				if (ch < PAULA_VOICES - 1)
					printf(", ");
			}
			printf("]%s\n", row < MOD_ROWS - 1 ? "," : "");
		}
		printf("        ]%s\n", pat < maxPattern ? "," : "");
	}
	printf("    ],\n");
}

// Dump module data in JSON format (samples, pattern_order_table, patterns, ticks).
// Writes the samples, pattern_order_table, and patterns sections, then opens ticks.
// After calling this, use dumpTickComment/dumpTickData for each tick,
// then call dumpClose when done.
void dumpOpen(void)
{
	if (jsonNeedsClosing)
		return;
	printf("{\n");
	dumpSamplesJson();
	dumpPatternOrderTableJson();
	dumpPatternsJson();
	printf("    \"ticks\": [\n");
	fflush(stdout);
	jsonNeedsClosing = true;
	firstTickElement = true;
	dumpRegisterCleanupHandlers();
}
