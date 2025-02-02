/*
 * rtl_433, turns your Realtek RTL2832 based DVB dongle into a 433.92MHz generic data receiver
 * Copyright (C) 2012 by Benjamin Larsson <benjamin@southpole.se>
 *
 * Based on rtl_sdr
 *
 * Copyright (C) 2012 by Steve Markgraf <steve@steve-m.de>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdbool.h>
#include <sys/time.h>


//#include "rtl-sdr.h"
#include "rtl_433.h"
#include "baseband.h"
#include "pulse_detect.h"
#include "pulse_demod.h"
#include "data.h"

#include <hackrf.h>

#define FREQ_ONE_MHZ (1000000ull)

static int do_exit = 0;
static int do_exit_async = 0, frequencies = 0, events = 0;
uint32_t frequency[MAX_PROTOCOLS];
time_t rawtime_old;
int flag;
uint32_t samp_rate = DEFAULT_SAMPLE_RATE;
float sample_file_pos = -1;
static uint32_t bytes_to_read = 0;
//static rtlsdr_dev_t *dev = NULL;
hackrf_device* dev = NULL;

static int override_short = 0;
static int override_long = 0;
int debug_output = 0;
int quiet_mode = 0;
int overwrite_mode = 0;

int num_r_devices = 0;

volatile uint32_t call_count = 0;
volatile uint32_t byte_count = 0;
struct timeval time_start;
struct timeval t_start;
uint32_t baseband_filter_bw_hz = 0;

static float TimevalDiff(const struct timeval *a, const struct timeval *b) {
	return (a->tv_sec - b->tv_sec) + 1e-6f * (a->tv_usec - b->tv_usec);
}


struct dm_state {
	FILE *out_file;
	int32_t level_limit;
	int16_t am_buf[MAXIMAL_BUF_LENGTH];	// AM demodulated signal (for OOK decoding)
	union {
		// These buffers aren't used at the same time, so let's use a union to save some memory
		int16_t fm_buf[MAXIMAL_BUF_LENGTH];	// FM demodulated signal (for FSK decoding)
		uint16_t temp_buf[MAXIMAL_BUF_LENGTH];	// Temporary buffer (to be optimized out..)
	};
	FilterState lowpass_filter_state;
	DemodFM_State demod_FM_state;
	int enable_FM_demod;
	int analyze;
	int analyze_pulses;
	int debug_mode;

	/* Signal grabber variables */
	int signal_grabber;
	int8_t* sg_buf;
	int sg_index;
	int sg_len;


	/* Protocol states */
	int r_dev_num;
	struct protocol_state *r_devs[MAX_PROTOCOLS];

	pulse_data_t	pulse_data;
	pulse_data_t	fsk_pulse_data;
};

void usage(r_device *devices) {
	int i;

	fprintf(stderr,
			"rtl_433, an ISM band generic data receiver for RTL2832 based DVB-T receivers\n\n"
			"Usage:\t= Tuner options =\n"
			"\t[-d <device index>] (default: 0)\n"
			"\t[-g <gain>] (default: 0 for auto)\n"
			"\t[-f <frequency>] [-f...] Receive frequency(s) (default: %i Hz)\n"
			"\t[-p <ppm_error] Correct rtl-sdr tuner frequency offset error (default: 0)\n"
			"\t[-s <sample rate>] Set sample rate (default: %i Hz)\n"
			"\t[-S] Force sync output (default: async)\n"
			"\t= Demodulator options =\n"
			"\t[-R <device>] Listen only for the specified remote device (can be used multiple times)\n"
			"\t[-l <level>] Change detection level used to determine pulses [0-32767] (0 = auto) (default: %i)\n"
			"\t[-z <value>] Override short value in data decoder\n"
			"\t[-x <value>] Override long value in data decoder\n"
			"\t= Analyze/Debug options =\n"
			"\t[-a] Analyze mode. Print a textual description of the signal. Disables decoding\n"
			"\t[-A] Pulse Analyzer. Enable pulse analyzis and decode attempt\n"
			"\t[-D] Print debug info on event (repeat for more info)\n"
			"\t[-q] Quiet mode, suppress non-data messages\n"
			"\t[-W] Overwrite mode, disable checks to prevent files from being overwritten\n"
			"\t= File I/O options =\n"
			"\t[-t] Test signal auto save. Use it together with analyze mode (-a -t). Creates one file per signal\n"
			"\t\t Note: Saves raw I/Q samples (uint8 pcm, 2 channel). Preferred mode for generating test files\n"
			"\t[-r <filename>] Read data from input file instead of a receiver\n"
			"\t[-m <mode>] Data file mode for input / output file (default: 0)\n"
			"\t\t 0 = Raw I/Q samples (uint8, 2 channel)\n"
			"\t\t 1 = AM demodulated samples (int16 pcm, 1 channel)\n"
			"\t\t 2 = FM demodulated samples (int16) (experimental)\n"
			"\t\t 3 = Raw I/Q samples (cf32, 2 channel)\n"
			"\t\t Note: If output file is specified, input will always be I/Q\n"
			"\t[-F] kv|json|csv Produce decoded output in given format. Not yet supported by all drivers.\n"
			"\t[<filename>] Save data stream to output file (a '-' dumps samples to stdout)\n\n", 
		DEFAULT_FREQUENCY, DEFAULT_SAMPLE_RATE, DEFAULT_LEVEL_LIMIT);

	fprintf(stderr, "Supported devices:\n");
	for (i = 0; i < num_r_devices; i++) {
		fprintf(stderr, "\t[%02d] %s\n", i + 1, devices[i].name);
	}
	fprintf(stderr, "\n");

	exit(1);
}

#ifdef _WIN32
BOOL WINAPI
sighandler(int signum) {
	if (CTRL_C_EVENT == signum) {
		fprintf(stderr, "Signal caught, exiting!\n");
		do_exit = 1;
		//rtlsdr_cancel_async(dev);
		hackrf_stop_rx(dev);
		return TRUE;
	}
	return FALSE;
}
#else
static void sighandler(int signum) {
	if (signum == SIGPIPE) {
		signal(SIGPIPE,SIG_IGN);
	} else {
		fprintf(stderr, "Signal caught, exiting!\n");
	}
	do_exit = 1;
	//rtlsdr_cancel_async(dev);
	hackrf_stop_rx(dev);
}
#endif


static void register_protocol(struct dm_state *demod, r_device *t_dev) {
	struct protocol_state *p = calloc(1, sizeof (struct protocol_state));
	p->short_limit = (float) t_dev->short_limit / ((float) DEFAULT_SAMPLE_RATE / (float) samp_rate);
	p->long_limit = (float) t_dev->long_limit / ((float) DEFAULT_SAMPLE_RATE / (float) samp_rate);
	p->reset_limit = (float) t_dev->reset_limit / ((float) DEFAULT_SAMPLE_RATE / (float) samp_rate);
	p->modulation = t_dev->modulation;
	p->callback = t_dev->json_callback;
	p->name = t_dev->name;
	p->demod_arg = t_dev->demod_arg;
	bitbuffer_clear(&p->bits);

	demod->r_devs[demod->r_dev_num] = p;
	demod->r_dev_num++;

	if (!quiet_mode) {
		fprintf(stderr, "Registering protocol \"%s\"\n", t_dev->name);
	}

	if (demod->r_dev_num > MAX_PROTOCOLS)
		fprintf(stderr, "Max number of protocols reached %d\n", MAX_PROTOCOLS);
}


static unsigned int counter = 0;
static unsigned int print = 1;
static unsigned int print2 = 0;
static unsigned int pulses_found = 0;
static unsigned int prev_pulse_start = 0;
static unsigned int pulse_start = 0;
static unsigned int pulse_end = 0;
static unsigned int pulse_avg = 0;
static unsigned int signal_start = 0;
static unsigned int signal_end = 0;
static unsigned int signal_pulse_data[4000][3] = {
	{0}};
static unsigned int signal_pulse_counter = 0;
typedef enum  {
	OUTPUT_KV,
	OUTPUT_JSON,
	OUTPUT_CSV
} output_format_t;
static output_format_t output_format;
void *csv_aux_data;

/* handles incoming structured data by dumping it */
void data_acquired_handler(data_t *data)
{
	switch (output_format) {
		case OUTPUT_KV: {
					data_print(data, stdout,  &data_kv_printer, NULL);
				} break;
		case OUTPUT_JSON: {
					  data_print(data, stdout,  &data_json_printer, NULL);
				  } break;
		case OUTPUT_CSV: {
					 data_print(data, stdout,  &data_csv_printer, csv_aux_data);
				 } break;
	}
	fflush(stdout);
	data_free(data);
}

static void classify_signal() {
	unsigned int i, k, max = 0, min = 1000000, t;
	unsigned int delta, count_min, count_max, min_new, max_new, p_limit;
	unsigned int a[3], b[2], a_cnt[3], a_new[3], b_new[2];
	unsigned int signal_distance_data[4000] = {0};
	struct protocol_state p = {0};
	unsigned int signal_type;

	if (!signal_pulse_data[0][0])
		return;

	for (i = 0; i < 1000; i++) {
		if (signal_pulse_data[i][0] > 0) {
			//fprintf(stderr, "[%03d] s: %d\t  e:\t %d\t l:%d\n",
			//i, signal_pulse_data[i][0], signal_pulse_data[i][1],
			//signal_pulse_data[i][2]);
			if (signal_pulse_data[i][2] > max)
				max = signal_pulse_data[i][2];
			if (signal_pulse_data[i][2] <= min)
				min = signal_pulse_data[i][2];
		}
	}
	t = (max + min) / 2;
	//fprintf(stderr, "\n\nMax: %d, Min: %d  t:%d\n", max, min, t);

	delta = (max - min)*(max - min);

	//TODO use Lloyd-Max quantizer instead
	k = 1;
	while ((k < 10) && (delta > 0)) {
		min_new = 0;
		count_min = 0;
		max_new = 0;
		count_max = 0;

		for (i = 0; i < 1000; i++) {
			if (signal_pulse_data[i][0] > 0) {
				if (signal_pulse_data[i][2] < t) {
					min_new = min_new + signal_pulse_data[i][2];
					count_min++;
				} else {
					max_new = max_new + signal_pulse_data[i][2];
					count_max++;
				}
			}
		}
		if (count_min != 0 && count_max != 0) {
			min_new = min_new / count_min;
			max_new = max_new / count_max;
		}

		delta = (min - min_new)*(min - min_new) + (max - max_new)*(max - max_new);
		min = min_new;
		max = max_new;
		t = (min + max) / 2;

		fprintf(stderr, "Iteration %d. t: %d    min: %d (%d)    max: %d (%d)    delta %d\n", k, t, min, count_min, max, count_max, delta);
		k++;
	}

	for (i = 0; i < 1000; i++) {
		if (signal_pulse_data[i][0] > 0) {
			//fprintf(stderr, "%d\n", signal_pulse_data[i][1]);
		}
	}
	/* 50% decision limit */
	if (min != 0 && max / min > 1) {
		fprintf(stderr, "Pulse coding: Short pulse length %d - Long pulse length %d\n", min, max);
		signal_type = 2;
	} else {
		fprintf(stderr, "Distance coding: Pulse length %d\n", (min + max) / 2);
		signal_type = 1;
	}
	p_limit = (max + min) / 2;

	/* Initial guesses */
	a[0] = 1000000;
	a[2] = 0;
	for (i = 1; i < 1000; i++) {
		if (signal_pulse_data[i][0] > 0) {
			//               fprintf(stderr, "[%03d] s: %d\t  e:\t %d\t l:%d\t  d:%d\n",
			//               i, signal_pulse_data[i][0], signal_pulse_data[i][1],
			//               signal_pulse_data[i][2], signal_pulse_data[i][0]-signal_pulse_data[i-1][1]);
			signal_distance_data[i - 1] = signal_pulse_data[i][0] - signal_pulse_data[i - 1][1];
			if (signal_distance_data[i - 1] > a[2])
				a[2] = signal_distance_data[i - 1];
			if (signal_distance_data[i - 1] <= a[0])
				a[0] = signal_distance_data[i - 1];
		}
	}
	min = a[0];
	max = a[2];
	a[1] = (a[0] + a[2]) / 2;
	//    for (i=0 ; i<1 ; i++) {
	//        b[i] = (a[i]+a[i+1])/2;
	//    }
	b[0] = (a[0] + a[1]) / 2;
	b[1] = (a[1] + a[2]) / 2;
	//     fprintf(stderr, "a[0]: %d\t a[1]: %d\t a[2]: %d\t\n",a[0],a[1],a[2]);
	//     fprintf(stderr, "b[0]: %d\t b[1]: %d\n",b[0],b[1]);

	k = 1;
	delta = 10000000;
	while ((k < 10) && (delta > 0)) {
		for (i = 0; i < 3; i++) {
			a_new[i] = 0;
			a_cnt[i] = 0;
		}

		for (i = 0; i < 1000; i++) {
			if (signal_distance_data[i] > 0) {
				if (signal_distance_data[i] < b[0]) {
					a_new[0] += signal_distance_data[i];
					a_cnt[0]++;
				} else if (signal_distance_data[i] < b[1] && signal_distance_data[i] >= b[0]) {
					a_new[1] += signal_distance_data[i];
					a_cnt[1]++;
				} else if (signal_distance_data[i] >= b[1]) {
					a_new[2] += signal_distance_data[i];
					a_cnt[2]++;
				}
			}
		}

		//         fprintf(stderr, "Iteration %d.", k);
		delta = 0;
		for (i = 0; i < 3; i++) {
			if (a_cnt[i])
				a_new[i] /= a_cnt[i];
			delta += (a[i] - a_new[i])*(a[i] - a_new[i]);
			//             fprintf(stderr, "\ta[%d]: %d (%d)", i, a_new[i], a[i]);
			a[i] = a_new[i];
		}
		//         fprintf(stderr, " delta %d\n", delta);

		if (a[0] < min) {
			a[0] = min;
			//             fprintf(stderr, "Fixing a[0] = %d\n", min);
		}
		if (a[2] > max) {
			a[0] = max;
			//             fprintf(stderr, "Fixing a[2] = %d\n", max);
		}
		//         if (a[1] == 0) {
		//             a[1] = (a[2]+a[0])/2;
		//             fprintf(stderr, "Fixing a[1] = %d\n", a[1]);
		//         }

		//         fprintf(stderr, "Iteration %d.", k);
		for (i = 0; i < 2; i++) {
			//             fprintf(stderr, "\tb[%d]: (%d) ", i, b[i]);
			b[i] = (a[i] + a[i + 1]) / 2;
			//             fprintf(stderr, "%d  ", b[i]);
		}
		//         fprintf(stderr, "\n");
		k++;
	}

	if (override_short) {
		p_limit = override_short;
		a[0] = override_short;
	}

	if (override_long) {
		a[1] = override_long;
	}

	fprintf(stderr, "\nShort distance: %d, long distance: %d, packet distance: %d\n", a[0], a[1], a[2]);
	fprintf(stderr, "\np_limit: %d\n", p_limit);

	bitbuffer_clear(&p.bits);
	if (signal_type == 1) {
		for (i = 0; i < 1000; i++) {
			if (signal_distance_data[i] > 0) {
				if (signal_distance_data[i] < (a[0] + a[1]) / 2) {
					//                     fprintf(stderr, "0 [%d] %d < %d\n",i, signal_distance_data[i], (a[0]+a[1])/2);
					bitbuffer_add_bit(&p.bits, 0);
				} else if ((signal_distance_data[i] > (a[0] + a[1]) / 2) && (signal_distance_data[i] < (a[1] + a[2]) / 2)) {
					//                     fprintf(stderr, "0 [%d] %d > %d\n",i, signal_distance_data[i], (a[0]+a[1])/2);
					bitbuffer_add_bit(&p.bits, 1);
				} else if (signal_distance_data[i] > (a[1] + a[2]) / 2) {
					//                     fprintf(stderr, "0 [%d] %d > %d\n",i, signal_distance_data[i], (a[1]+a[2])/2);
					bitbuffer_add_row(&p.bits);
				}

			}

		}
		bitbuffer_print(&p.bits);
	}
	if (signal_type == 2) {
		for (i = 0; i < 1000; i++) {
			if (signal_pulse_data[i][2] > 0) {
				if (signal_pulse_data[i][2] < p_limit) {
					//                     fprintf(stderr, "0 [%d] %d < %d\n",i, signal_pulse_data[i][2], p_limit);
					bitbuffer_add_bit(&p.bits, 0);
				} else {
					//                     fprintf(stderr, "1 [%d] %d > %d\n",i, signal_pulse_data[i][2], p_limit);
					bitbuffer_add_bit(&p.bits, 1);
				}
				if ((signal_distance_data[i] >= (a[1] + a[2]) / 2)) {
					//                     fprintf(stderr, "\\n [%d] %d > %d\n",i, signal_distance_data[i], (a[1]+a[2])/2);
					bitbuffer_add_row(&p.bits);
				}


			}
		}
		bitbuffer_print(&p.bits);
	}

	for (i = 0; i < 1000; i++) {
		signal_pulse_data[i][0] = 0;
		signal_pulse_data[i][1] = 0;
		signal_pulse_data[i][2] = 0;
		signal_distance_data[i] = 0;
	}

};

static void pwm_analyze(struct dm_state *demod, int16_t *buf, uint32_t len) {
	unsigned int i;
	int32_t threshold = (demod->level_limit ? demod->level_limit : DEFAULT_LEVEL_LIMIT);	// Fix for auto level

	for (i = 0; i < len; i++) {
		if (buf[i] > threshold) {
			if (!signal_start)
				signal_start = counter;
			if (print) {
				pulses_found++;
				pulse_start = counter;
				signal_pulse_data[signal_pulse_counter][0] = counter;
				signal_pulse_data[signal_pulse_counter][1] = -1;
				signal_pulse_data[signal_pulse_counter][2] = -1;
				if (debug_output) fprintf(stderr, "pulse_distance %d\n", counter - pulse_end);
				if (debug_output) fprintf(stderr, "pulse_start distance %d\n", pulse_start - prev_pulse_start);
				if (debug_output) fprintf(stderr, "pulse_start[%d] found at sample %d, value = %d\n", pulses_found, counter, buf[i]);
				prev_pulse_start = pulse_start;
				print = 0;
				print2 = 1;
			}
		}
		counter++;
		if (buf[i] < threshold) {
			if (print2) {
				pulse_avg += counter - pulse_start;
				if (debug_output) fprintf(stderr, "pulse_end  [%d] found at sample %d, pulse length = %d, pulse avg length = %d\n",
						pulses_found, counter, counter - pulse_start, pulse_avg / pulses_found);
				pulse_end = counter;
				print2 = 0;
				signal_pulse_data[signal_pulse_counter][1] = counter;
				signal_pulse_data[signal_pulse_counter][2] = counter - pulse_start;
				signal_pulse_counter++;
				if (signal_pulse_counter >= 4000) {
					signal_pulse_counter = 0;
					goto err;
				}
			}
			print = 1;
			if (signal_start && (pulse_end + 50000 < counter)) {
				signal_end = counter - 40000;
				fprintf(stderr, "*** signal_start = %d, signal_end = %d\n", signal_start - 10000, signal_end);
				fprintf(stderr, "signal_len = %d,  pulses = %d\n", signal_end - (signal_start - 10000), pulses_found);
				pulses_found = 0;
				classify_signal();

				signal_pulse_counter = 0;
				if (demod->sg_buf) {
					int start_pos, signal_bszie, wlen, wrest = 0, sg_idx, idx;
					char sgf_name[256] = {0};
					FILE *sgfp;

					while (1) {
						sprintf(sgf_name, "gfile%03d.data", demod->signal_grabber);
						demod->signal_grabber++;
						if (access(sgf_name, F_OK) == -1 || overwrite_mode) {
							break;
						}
					}

					signal_bszie = 2 * (signal_end - (signal_start - 10000));
					signal_bszie = (131072 - (signal_bszie % 131072)) + signal_bszie;
					sg_idx = demod->sg_index - demod->sg_len;
					if (sg_idx < 0)
						sg_idx = SIGNAL_GRABBER_BUFFER - demod->sg_len;
					idx = (i - 40000)*2;
					start_pos = sg_idx + idx - signal_bszie;
					fprintf(stderr, "signal_bszie = %d  -      sg_index = %d\n", signal_bszie, demod->sg_index);
					fprintf(stderr, "start_pos    = %d  -   buffer_size = %d\n", start_pos, SIGNAL_GRABBER_BUFFER);
					if (signal_bszie > SIGNAL_GRABBER_BUFFER)
						fprintf(stderr, "Signal bigger then buffer, signal = %d > buffer %d !!\n", signal_bszie, SIGNAL_GRABBER_BUFFER);

					if (start_pos < 0) {
						start_pos = SIGNAL_GRABBER_BUFFER + start_pos;
						fprintf(stderr, "restart_pos = %d\n", start_pos);
					}

					fprintf(stderr, "*** Saving signal to file %s\n", sgf_name);
					sgfp = fopen(sgf_name, "wb");
					if (!sgfp) {
						fprintf(stderr, "Failed to open %s\n", sgf_name);
					}
					wlen = signal_bszie;
					if (start_pos + signal_bszie > SIGNAL_GRABBER_BUFFER) {
						wlen = SIGNAL_GRABBER_BUFFER - start_pos;
						wrest = signal_bszie - wlen;
					}
					fprintf(stderr, "*** Writing data from %d, len %d\n", start_pos, wlen);
					fwrite(&demod->sg_buf[start_pos], 1, wlen, sgfp);

					if (wrest) {
						fprintf(stderr, "*** Writing data from %d, len %d\n", 0, wrest);
						fwrite(&demod->sg_buf[0], 1, wrest, sgfp);
					}

					fclose(sgfp);
				}
				signal_start = 0;
			}
		}


	}
	return;

err:
	fprintf(stderr, "To many pulses detected, probably bad input data or input parameters\n");
	return;
}

/*
typedef struct {
        hackrf_device* device;
        uint8_t* buffer;
        int buffer_length;
        int valid_length;
        void* rx_ctx;
        void* tx_ctx;
} hackrf_transfer;
*/

//static void rtlsdr_callback(unsigned char *iq_buf, uint32_t len, void *ctx) {
int rtlsdr_callback(hackrf_transfer *transfer) {
	//struct dm_state *demod = ctx;
	struct dm_state *demod = transfer->rx_ctx;
	int i;
	uint32_t len = transfer->valid_length;
	//printf("-----------> %u vs %u\n", transfer->valid_length, transfer->buffer_length);

	if (do_exit || do_exit_async)
		return 0;

	if ((bytes_to_read > 0) && (bytes_to_read < len)) {
		len = bytes_to_read;
		do_exit = 1;
		//rtlsdr_cancel_async(dev);
		hackrf_stop_rx(dev);
	}

	if (demod->signal_grabber) {
		//fprintf(stderr, "[%d] sg_index - len %d\n", demod->sg_index, len );
		//memcpy(&demod->sg_buf[demod->sg_index], iq_buf, len);
		memcpy(&demod->sg_buf[demod->sg_index], transfer->buffer, len);
		demod->sg_len = len;
		demod->sg_index += len;
		if (demod->sg_index + len > SIGNAL_GRABBER_BUFFER)
			demod->sg_index = 0;
	}

	// AM demodulation
	//envelope_detect(iq_buf, demod->temp_buf, len/2);
	envelope_detect(transfer->buffer, demod->temp_buf, len/2);
	baseband_low_pass_filter(demod->temp_buf, demod->am_buf, len/2, &demod->lowpass_filter_state);

	// FM demodulation
	if (demod->enable_FM_demod) {
		//baseband_demod_FM(iq_buf, demod->fm_buf, len/2, &demod->demod_FM_state);
		baseband_demod_FM(transfer->buffer, demod->fm_buf, len/2, &demod->demod_FM_state);
	}

	// Handle special input formats
	if(!demod->out_file) {				// If output file is specified we always assume I/Q input
		if (demod->debug_mode == 1) {	// The IQ buffer is really AM demodulated data
			//memcpy(demod->am_buf, iq_buf, len);
			memcpy(demod->am_buf, transfer->buffer, len);
		} else if (demod->debug_mode == 2) {	// The IQ buffer is really FM demodulated data
			fprintf(stderr, "Reading FM modulated data not implemented yet!\n");
		}
	}

	if (demod->analyze || (demod->out_file == stdout)) {	// We don't want to decode devices when outputting to stdout
		pwm_analyze(demod, demod->am_buf, len / 2);
	} else {
		// Detect a package and loop through demodulators with pulse data
		int package_type = 1;	// Just to get us started
		while(package_type) {
			package_type = detect_pulse_package(demod->am_buf, demod->fm_buf, len/2, demod->level_limit, samp_rate, &demod->pulse_data, &demod->fsk_pulse_data);
			if (package_type == 1) {
				if(demod->analyze_pulses) fprintf(stderr, "Detected OOK package\n");
				for (i = 0; i < demod->r_dev_num; i++) {
					switch (demod->r_devs[i]->modulation) {
						case OOK_PULSE_PCM_RZ:
							pulse_demod_pcm(&demod->pulse_data, demod->r_devs[i]);
							break;
						case OOK_PULSE_PPM_RAW:
							pulse_demod_ppm(&demod->pulse_data, demod->r_devs[i]);
							break;
						case OOK_PULSE_PWM_PRECISE:
							pulse_demod_pwm_precise(&demod->pulse_data, demod->r_devs[i]);
							break;
						case OOK_PULSE_PWM_RAW:
							pulse_demod_pwm(&demod->pulse_data, demod->r_devs[i]);
							break;
						case OOK_PULSE_PWM_TERNARY:
							pulse_demod_pwm_ternary(&demod->pulse_data, demod->r_devs[i]);
							break;
						case OOK_PULSE_MANCHESTER_ZEROBIT:
							pulse_demod_manchester_zerobit(&demod->pulse_data, demod->r_devs[i]);
							break;
						case OOK_PULSE_CLOCK_BITS:
							pulse_demod_clock_bits(&demod->pulse_data, demod->r_devs[i]);
							break;
							// FSK decoders
						case FSK_PULSE_PCM:
						case FSK_PULSE_PWM_RAW:
							break;
						default:
							fprintf(stderr, "Unknown modulation %d in protocol!\n", demod->r_devs[i]->modulation);
					}
				} // for demodulators
				if(debug_output > 1) pulse_data_print(&demod->pulse_data);
				if(demod->analyze_pulses) pulse_analyzer(&demod->pulse_data);
			} else if (package_type == 2) {
				if(demod->analyze_pulses) fprintf(stderr, "Detected FSK package\n");
				for (i = 0; i < demod->r_dev_num; i++) {
					switch (demod->r_devs[i]->modulation) {
						// OOK decoders
						case OOK_PULSE_PCM_RZ:
						case OOK_PULSE_PPM_RAW:
						case OOK_PULSE_PWM_PRECISE:
						case OOK_PULSE_PWM_RAW:
						case OOK_PULSE_PWM_TERNARY:
						case OOK_PULSE_MANCHESTER_ZEROBIT:
						case OOK_PULSE_CLOCK_BITS:
							break;
						case FSK_PULSE_PCM:
							pulse_demod_pcm(&demod->fsk_pulse_data, demod->r_devs[i]);
							break;
						case FSK_PULSE_PWM_RAW:
							pulse_demod_pwm(&demod->fsk_pulse_data, demod->r_devs[i]);
							break;
						default:
							fprintf(stderr, "Unknown modulation %d in protocol!\n", demod->r_devs[i]->modulation);
					}
				} // for demodulators
				if(debug_output > 1) pulse_data_print(&demod->fsk_pulse_data);
				if(demod->analyze_pulses) pulse_analyzer(&demod->fsk_pulse_data);
			}
		}
	}

	if (demod->out_file) {
		//uint8_t* out_buf = iq_buf;				// Default is to dump IQ samples
		uint8_t* out_buf = transfer->buffer;			// Default is to dump IQ samples
		if (demod->debug_mode == 1) {			// AM data
			out_buf = (uint8_t*)demod->am_buf;
		} else if (demod->debug_mode == 2) {	// FM data
			out_buf = (uint8_t*)demod->fm_buf;
		}
		if (fwrite(out_buf, 1, len, demod->out_file) != len) {
			fprintf(stderr, "Short write, samples lost, exiting!\n");
			//rtlsdr_cancel_async(dev);
			hackrf_stop_rx(dev);
		}
	}

	if (bytes_to_read > 0)
		bytes_to_read -= len;

	if (frequencies > 1) {
		time_t rawtime;
		time(&rawtime);
		if (difftime(rawtime, rawtime_old) > DEFAULT_HOP_TIME || events >= DEFAULT_HOP_EVENTS) {
			rawtime_old = rawtime;
			events = 0;
			do_exit_async = 1;
			//rtlsdr_cancel_async(dev);
			hackrf_stop_rx(dev);
		}
	}
	byte_count += transfer->valid_length;
	call_count++;
	return 0;
}

// find the fields output for CSV
void *determine_csv_fields(r_device* devices, int num_devices)
{
	int i, j;
	int cur_output_fields = 0;
	int num_output_fields = 0;
	void *csv_aux;
	const char **output_fields = NULL;
	for (i = 0; i < num_devices; i++)
		if (!devices[i].disabled) {
			if (devices[i].fields)
				for (int c = 0; devices[i].fields[c]; ++c)
					++num_output_fields;
			else
				fprintf(stderr, "rtl_433: warning: %d \"%s\" does not support CSV output\n",
						i, devices[i].name);
		}
	output_fields = calloc(num_output_fields + 1, sizeof(char*));
	for (i = 0; i < num_devices; i++) {
		if (!devices[i].disabled && devices[i].fields) {
			for (int c = 0; devices[i].fields[c]; ++c) {
				output_fields[cur_output_fields] = devices[i].fields[c];
				++cur_output_fields;
			}
		}
	}

	csv_aux = data_csv_init(output_fields, num_output_fields);
	free(output_fields);
	return csv_aux;
}

int main(int argc, char **argv) {
#ifndef _WIN32
	struct sigaction sigact;
#endif
	char *out_filename = NULL;
	char *in_filename = NULL;
	FILE *in_file;
	int n_read;
	int r, opt;
	int i, gain = 0;
	int sync_mode = 0;
	int ppm_error = 0;
	struct dm_state* demod;
	uint32_t dev_index = 0;
	int frequency_current = 0;
	uint32_t out_block_size = DEFAULT_BUF_LENGTH;
	int device_count;
	char vendor[256], product[256], serial[256];
	int have_opt_R = 0;

	uint8_t board_id = BOARD_ID_INVALID;
	char version[255 + 1];

	setbuf(stdout, NULL);
	setbuf(stderr, NULL);

	demod = malloc(sizeof (struct dm_state));
	memset(demod, 0, sizeof (struct dm_state));

	/* initialize tables */
	baseband_init();

	r_device devices[] = {
#define DECL(name) name,
		DEVICES
#undef DECL
	};

	num_r_devices = sizeof(devices)/sizeof(*devices);

	demod->level_limit = DEFAULT_LEVEL_LIMIT;

	while ((opt = getopt(argc, argv, "x:z:p:DtaAqm:r:l:d:f:g:s:b:n:SR:F:W")) != -1) {
		switch (opt) {
			case 'd':
				dev_index = atoi(optarg);
				break;
			case 'f':
				if (frequencies < MAX_PROTOCOLS) frequency[frequencies++] = (uint32_t) atof(optarg);
				else fprintf(stderr, "Max number of frequencies reached %d\n", MAX_PROTOCOLS);
				break;
			case 'g':
				gain = (int) (atof(optarg) * 10); /* tenths of a dB */
				break;
			case 'p':
				ppm_error = atoi(optarg);
				break;
			case 's':
				samp_rate = (uint32_t) atof(optarg);
				break;
			case 'b':
				out_block_size = (uint32_t) atof(optarg);
				break;
			case 'l':
				demod->level_limit = (uint32_t) atof(optarg);
				break;
			case 'n':
				bytes_to_read = (uint32_t) atof(optarg) * 2;
				break;
			case 'a':
				demod->analyze = 1;
				break;
			case 'A':
				demod->analyze_pulses = 1;
				break;
			case 'r':
				in_filename = optarg;
				break;
			case 't':
				demod->signal_grabber = 1;
				break;
			case 'm':
				demod->debug_mode = atoi(optarg);
				break;
			case 'S':
				sync_mode = 1;
				break;
			case 'D':
				debug_output++;
				break;
			case 'z':
				override_short = atoi(optarg);
				break;
			case 'x':
				override_long = atoi(optarg);
				break;
			case 'R':
				if (!have_opt_R) {
					for (i = 0; i < num_r_devices; i++) {
						devices[i].disabled = 1;
					}
					have_opt_R = 1;
				}

				i = atoi(optarg);
				if (i > num_r_devices) {
					fprintf(stderr, "Remote device number specified larger than number of devices\n\n");
					usage(devices);
				}

				devices[i - 1].disabled = 0;
				break;
			case 'q':
				quiet_mode = 1;
				break;
			case 'F':
				if (strcmp(optarg, "json") == 0) {
					output_format = OUTPUT_JSON;
				} else if (strcmp(optarg, "csv") == 0) {
					output_format = OUTPUT_CSV;
				} else if (strcmp(optarg, "kv") == 0) {
					output_format = OUTPUT_KV;
				} else {
					fprintf(stderr, "Invalid output format %s\n", optarg);
					usage(devices);
				}
				break;
			case 'W':
				overwrite_mode = 1;
				break;

			default:
				usage(devices);
				break;
		}
	}

	if (argc <= optind - 1) {
		usage(devices);
	} else {
		out_filename = argv[optind];
	}

	if (output_format == OUTPUT_CSV) {
		csv_aux_data = determine_csv_fields(devices, num_r_devices);
		if (!csv_aux_data) {
			fprintf(stderr, "rtl_433: failed to allocate memory for CSV auxiliary data\n");
			exit(1);
		}
	}

	for (i = 0; i < num_r_devices; i++) {
		if (!devices[i].disabled) {
			register_protocol(demod, &devices[i]);
			if(devices[i].modulation >= FSK_DEMOD_MIN_VAL) {
				demod->enable_FM_demod = 1;
			}
		}
	}

	if (out_block_size < MINIMAL_BUF_LENGTH ||
			out_block_size > MAXIMAL_BUF_LENGTH) {
		fprintf(stderr,
				"Output block size wrong value, falling back to default\n");
		fprintf(stderr,
				"Minimal length: %u\n", MINIMAL_BUF_LENGTH);
		fprintf(stderr,
				"Maximal length: %u\n", MAXIMAL_BUF_LENGTH);
		out_block_size = DEFAULT_BUF_LENGTH;
	}

	if (!in_filename) {
		/*
		device_count = rtlsdr_get_device_count();
		if (!device_count) {
			fprintf(stderr, "No supported devices found.\n");
			if (!in_filename)
				exit(1);
		}

		if (!quiet_mode) {
			fprintf(stderr, "Found %d device(s):\n", device_count);
			for (i = 0; i < device_count; i++) {
				rtlsdr_get_device_usb_strings(i, vendor, product, serial);
				fprintf(stderr, "  %d:  %s, %s, SN: %s\n", i, vendor, product, serial);
			}
			fprintf(stderr, "\n");

			fprintf(stderr, "Using device %d: %s\n",
					dev_index, rtlsdr_get_device_name(dev_index));
		}
		*/

		//r = rtlsdr_open(&dev, dev_index);
		if ((r = hackrf_init()) != HACKRF_SUCCESS) {
			fprintf(stderr, "hackrf_init() failed: %s (%d)\n", hackrf_error_name(r), r);
			return EXIT_FAILURE;
		}
		r = hackrf_open(&dev);
		if (r != HACKRF_SUCCESS) {
			fprintf(stderr, "Failed to open rtlsdr device #%d.\n", dev_index);
			exit(1);
		} else {
			printf(">> HackRF open\n");
		}
		// get info
		if((r = hackrf_board_id_read(dev, &board_id)) != HACKRF_SUCCESS) {
			fprintf(stderr, "hackrf_board_id_read() failed: %s (%d)\n",
					hackrf_error_name(r), r);
			return EXIT_FAILURE;
		}
		printf("Board ID Number: %d (%s)\n", board_id, hackrf_board_id_name(board_id));

		// get firmware version
		if((r = hackrf_version_string_read(dev, &version[0], 255)) != HACKRF_SUCCESS) {
			fprintf(stderr, "hackrf_version_string_read() failed: %s (%d)\n",
					hackrf_error_name(r), r);
			return EXIT_FAILURE;
		}
		printf("Firmware Version: %s\n", version);

#ifndef _WIN32
		sigact.sa_handler = sighandler;
		sigemptyset(&sigact.sa_mask);
		sigact.sa_flags = 0;
		sigaction(SIGINT, &sigact, NULL);
		sigaction(SIGTERM, &sigact, NULL);
		sigaction(SIGQUIT, &sigact, NULL);
		sigaction(SIGPIPE, &sigact, NULL);
#else
		SetConsoleCtrlHandler((PHANDLER_ROUTINE) sighandler, TRUE);
#endif
		/* Set the sample rate */
		//r = rtlsdr_set_sample_rate(dev, samp_rate);
		r = hackrf_set_sample_rate_manual(dev, samp_rate, 1);
		if (r != HACKRF_SUCCESS)
			fprintf(stderr, "WARNING: Failed to set sample rate.\n");
		else
			//fprintf(stderr, "Sample rate set to %d.\n", rtlsdr_get_sample_rate(dev)); // Unfortunately, doesn't return real rate
			fprintf(stderr, "Sample rate set to %u Hz/%.03f MHz.\n", samp_rate, ((float)samp_rate/(float)FREQ_ONE_MHZ));
		
		baseband_filter_bw_hz = hackrf_compute_baseband_filter_bw_round_down_lt(samp_rate);

		r = hackrf_set_baseband_filter_bandwidth(dev, baseband_filter_bw_hz);
		if (r != HACKRF_SUCCESS)
			printf("hackrf_baseband_filter_bandwidth_set() failed: %s (%d)\n", hackrf_error_name(r), r);
		else
			fprintf(stderr, "Baseband filter bandwidth set to %u Hz/%.03f MHz\n", baseband_filter_bw_hz, (float)((float)baseband_filter_bw_hz/(float)FREQ_ONE_MHZ));


		fprintf(stderr, "Bit detection level set to %d.\n", demod->level_limit);

		/*
		if (0 == gain) {
			// Enable automatic gain 
			r = rtlsdr_set_tuner_gain_mode(dev, 0);
			if (r < 0)
				fprintf(stderr, "WARNING: Failed to enable automatic gain.\n");
			else
				fprintf(stderr, "Tuner gain set to Auto.\n");
		} else {
			// Enable manual gain 
			r = rtlsdr_set_tuner_gain_mode(dev, 1);
			if (r < 0)
				fprintf(stderr, "WARNING: Failed to enable manual gain.\n");

			// Set the tuner gain 
			r = rtlsdr_set_tuner_gain(dev, gain);
			if (r < 0)
				fprintf(stderr, "WARNING: Failed to set tuner gain.\n");
			else
				fprintf(stderr, "Tuner gain set to %f dB.\n", gain / 10.0);
		}
		*/

		// RX VGA (baseband) gain, 0-62dB, 2dB steps
		if((r = hackrf_set_vga_gain(dev, 2)) != HACKRF_SUCCESS) {
			fprintf(stderr, "WARNING: hackrf_set_vga_gain.\n");
		} else {
			printf(">> hackrf_set_vga_gain\n");
		}

		// RX LNA (IF) gain, 0-40dB, 8dB steps
		if((r = hackrf_set_lna_gain(dev, 40)) != HACKRF_SUCCESS) {
			fprintf(stderr, "WARNING: hackrf_set_lna_gain.\n");
		} else {
			printf(">> hackrf_set_lna_gain\n");
		}

		if((r = hackrf_set_amp_enable(dev, (uint8_t)1)) != HACKRF_SUCCESS) {
			fprintf(stderr, "WARNING: hackrf_set_amp_enable.\n");
		} else {
			printf(">> hackrf_set_amp_enable\n");
		}

		//r = rtlsdr_set_freq_correction(dev, ppm_error);

	}

	if (out_filename) {
		if (strcmp(out_filename, "-") == 0) { /* Write samples to stdout */
			demod->out_file = stdout;
#ifdef _WIN32
			_setmode(_fileno(stdin), _O_BINARY);
#endif
		} else {
			if (access(out_filename, F_OK) == 0 && !overwrite_mode) {
				fprintf(stderr, "Output file %s already exists, exiting\n", out_filename);
				goto out;
			}
			demod->out_file = fopen(out_filename, "wb");
			if (!demod->out_file) {
				fprintf(stderr, "Failed to open %s\n", out_filename);
				goto out;
			}
		}
	}

	if (demod->signal_grabber)
		demod->sg_buf = malloc(SIGNAL_GRABBER_BUFFER);

	/*
	if (in_filename) {
		int i = 0;
		unsigned char test_mode_buf[DEFAULT_BUF_LENGTH];
		float test_mode_float_buf[DEFAULT_BUF_LENGTH];
		if (strcmp(in_filename, "-") == 0) { // read samples from stdin
			in_file = stdin;
			in_filename = "<stdin>";
		} else {
			in_file = fopen(in_filename, "r");
			if (!in_file) {
				fprintf(stderr, "Opening file: %s failed!\n", in_filename);
				goto out;
			}
		}
		if (!quiet_mode) {
			fprintf(stderr, "Test mode active. Reading samples from file: %s\n", in_filename);
			fprintf(stderr, "Input format: %s\n", (demod->debug_mode == 3) ? "cf32" : "uint8");
		}
		sample_file_pos = 0.0;

		int n_read, cf32_tmp;
		do {
			if (demod->debug_mode == 3) {
				n_read = fread(test_mode_float_buf, sizeof(float), 131072, in_file);
				for(int n = 0; n < n_read; n++) {
					cf32_tmp = test_mode_float_buf[n]*127 + 127;
					if (cf32_tmp < 0)
						cf32_tmp = 0;
					else if (cf32_tmp > 255)
						cf32_tmp = 255;
					test_mode_buf[n] = (uint8_t)cf32_tmp;
				}
			} else {
				n_read = fread(test_mode_buf, 1, 131072, in_file);
			}
			if (n_read == 0) break;	// rtlsdr_callback() will Segmentation Fault with len=0
			rtlsdr_callback(test_mode_buf, n_read, demod);
			i++;
			sample_file_pos = (float)i * n_read / samp_rate;
		} while (n_read != 0);

		// Call a last time with cleared samples to ensure EOP detection
		memset(test_mode_buf, 128, DEFAULT_BUF_LENGTH);     // 128 is 0 in unsigned data
		rtlsdr_callback(test_mode_buf, 131072, demod);      // Why the magic value 131072?

		//Always classify a signal at the end of the file
		classify_signal();
		if (!quiet_mode) {
			fprintf(stderr, "Test mode file issued %d packets\n", i);
		}
		exit(0);
	}
	*/

	/* Reset endpoint before we start reading from it (mandatory) */
	//r = rtlsdr_reset_buffer(dev);
	//r = hackrf_buffer_reset();
	//if (r < 0) fprintf(stderr, "WARNING: Failed to reset buffers.\n");


	// FIXME !!!!
	sync_mode=0;
	if (sync_mode) {
		fprintf(stderr, "Reading samples in sync mode...\n");
		uint8_t *buffer = malloc(out_block_size * sizeof (uint8_t));

		while (!do_exit) {
			//r = rtlsdr_read_sync(dev, buffer, out_block_size, &n_read);
			if (r < 0) {
				fprintf(stderr, "WARNING: sync read failed.\n");
				break;
			}

			if ((bytes_to_read > 0) && (bytes_to_read < (uint32_t) n_read)) {
				n_read = bytes_to_read;
				do_exit = 1;
			}

			if (fwrite(buffer, 1, n_read, demod->out_file) != (size_t) n_read) {
				fprintf(stderr, "Short write, samples lost, exiting!\n");
				break;
			}

			if ((uint32_t) n_read < out_block_size) {
				fprintf(stderr, "Short read, samples lost, exiting!\n");
				break;
			}

			if (bytes_to_read > 0)
				bytes_to_read -= n_read;
		}

		free(buffer);
	} else {
		if (frequencies == 0) {
			frequency[0] = DEFAULT_FREQUENCY;
			frequencies = 1;
		} else {
			time(&rawtime_old);
		}
		if (!quiet_mode) {
			fprintf(stderr, "Reading samples in async mode...\n");
		}

		r = hackrf_set_freq(dev, frequency[0]);
		if (r != HACKRF_SUCCESS)
			fprintf(stderr, "WARNING: Failed to set center freq.\n");

		// GO GO GO
		if((r = hackrf_start_rx(dev, rtlsdr_callback, (void *) demod)) != HACKRF_SUCCESS) {
			fprintf(stderr, "hackrf_start_rx failed: %s (%d)\n",
					hackrf_error_name(r), r);
			return EXIT_FAILURE;
		}
		do_exit_async = 0;
		gettimeofday(&t_start, NULL);
	        gettimeofday(&time_start, NULL);

		while ((hackrf_is_streaming(dev) == HACKRF_TRUE) && !do_exit) {
			/* Set the frequency */
			//r = rtlsdr_set_center_freq(dev, frequency[frequency_current]);
			//r = hackrf_set_freq(dev, frequency[frequency_current]);
			//if (r != HACKRF_SUCCESS)
			//	fprintf(stderr, "WARNING: Failed to set center freq.\n");
			//else
				//fprintf(stderr, "Tuned to %u Hz.\n", rtlsdr_get_center_freq(dev));
			
			//r = rtlsdr_read_async(dev, rtlsdr_callback, (void *) demod, DEFAULT_ASYNC_BUF_NUMBER, out_block_size);
			//hackrf_start_rx(dev, rtlsdr_callback, (void *) demod);
			//do_exit_async = 0;
			//frequency_current++;
			//if (frequency_current > frequencies - 1) frequency_current = 0;
			uint32_t byte_count_now;
			uint32_t call_count_now;
			struct timeval time_now;
			float time_difference, rate;
			sleep(1);
			gettimeofday(&time_now, NULL);
			byte_count_now = byte_count;
			call_count_now = call_count;
			byte_count = 0;
			call_count = 0;

			time_difference = TimevalDiff(&time_now, &time_start);
			rate = (float)byte_count_now / time_difference;
			printf("%4.1f MiB / %5.3f sec = %4.1f MiB/second\n", (byte_count_now / 1e6f), time_difference, (rate / 1e6f) );
//			printf("Callback count : %u / %5.3f sec -> %u\n", call_count_now, time_difference, byte_count_now/call_count_now );

			time_start = time_now;

			if (byte_count_now == 0) {
				printf("\nCouldn't transfer any bytes for one second.\n");
				break;
			}

		}
	}

	if (do_exit)
		fprintf(stderr, "\nUser cancel, exiting...\n");
	else
		fprintf(stderr, "\nLibrary error %d, exiting...\n", r);

	if (demod->out_file && (demod->out_file != stdout))
		fclose(demod->out_file);

	for (i = 0; i < demod->r_dev_num; i++)
		free(demod->r_devs[i]);

	if (demod->signal_grabber)
		free(demod->sg_buf);

	free(demod);

	//rtlsdr_close(dev);
	hackrf_close(dev);
out:
	if (csv_aux_data) {
		data_csv_free(csv_aux_data);
	}
	return r >= 0 ? r : -r;
}
