/*
 * Copyright (C) 2014 BlueKitchen GmbH
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the copyright holders nor the names of
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 * 4. Any redistribution, use, or modification is done solely for
 *    personal benefit and not for any commercial purpose or for
 *    monetary gain.
 *
 * THIS SOFTWARE IS PROVIDED BY BLUEKITCHEN GMBH AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL MATTHIAS
 * RINGWALD OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
 * THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * Please inquire about commercial licensing options at 
 * contact@bluekitchen-gmbh.com
 *
 */

/*
 * Sine Playback to test and validate audio output with simple wave form
 *
 */

#include "btstack.h"

#ifdef HAVE_POSIX_FILE_IO
#include "wav_util.h"
#endif

#define TABLE_SIZE_441HZ            100

static const int16_t sine_int16[] = {
     0,    2057,    4107,    6140,    8149,   10126,   12062,   13952,   15786,   17557,
 19260,   20886,   22431,   23886,   25247,   26509,   27666,   28714,   29648,   30466,
 31163,   31738,   32187,   32509,   32702,   32767,   32702,   32509,   32187,   31738,
 31163,   30466,   29648,   28714,   27666,   26509,   25247,   23886,   22431,   20886,
 19260,   17557,   15786,   13952,   12062,   10126,    8149,    6140,    4107,    2057,
     0,   -2057,   -4107,   -6140,   -8149,  -10126,  -12062,  -13952,  -15786,  -17557,
-19260,  -20886,  -22431,  -23886,  -25247,  -26509,  -27666,  -28714,  -29648,  -30466,
-31163,  -31738,  -32187,  -32509,  -32702,  -32767,  -32702,  -32509,  -32187,  -31738,
-31163,  -30466,  -29648,  -28714,  -27666,  -26509,  -25247,  -23886,  -22431,  -20886,
-19260,  -17557,  -15786,  -13952,  -12062,  -10126,   -8149,   -6140,   -4107,   -2057,
};

#define NUM_CHANNELS 2



// linear resampling
#define BTSTACK_RESAMPLE_MAX_CHANNELS 2
typedef struct {
    uint32_t src_pos;
    uint32_t src_step;
    int16_t  last_sample[BTSTACK_RESAMPLE_MAX_CHANNELS];
    int      num_channels;
} btstack_resample_t;

static void btstack_resample_init(btstack_resample_t * context, int num_channels){
    context->src_pos = 0;
    context->src_step = 0x10000;  // default resampling 1.0
    context->last_sample[0] = 0;
    context->last_sample[1] = 0;
    context->num_channels   = num_channels;
}

static void btstack_resample_set_src_step(btstack_resample_t * context, uint32_t src_step){
    context->src_step = src_step;
}

static uint16_t btstack_resample_block(btstack_resample_t * context, const int16_t * input_buffer, uint32_t num_samples, int16_t * output_buffer){
    uint16_t dest_frames = 0;
    uint16_t dest_samples = 0;
    // samples between last sample of previous block and first sample in current block 
    while (context->src_pos >= 0xffff0000){
        const uint16_t t = context->src_pos & 0xffff;
        int i;
        for (i=0;i<context->num_channels;i++){
            int s1 = context->last_sample[i];
            int s2 = input_buffer[i];
            int os = (s1*(0x10000 - t) + s2*t) >> 16;
            output_buffer[dest_frames++] = os;
        }
        context->src_pos += context->src_step;
    }
    // process current block
    while (1){
        const uint16_t src_pos = context->src_pos >> 16;
        const uint16_t t       = context->src_pos & 0xffff;
        int index = src_pos * context->num_channels;
        int i;
        if (src_pos >= (num_samples - 1)){
            // store last sample
            for (i=0;i<context->num_channels;i++){
                context->last_sample[i] = input_buffer[index++];
            }
            // samples processed
            context->src_pos -= num_samples << 16;
            break;
        }
        for (i=0;i<context->num_channels;i++){
            int s1 = input_buffer[index++];
            int s2 = input_buffer[index++];
            int os = (s1*(0x10000 - t) + s2*t) >> 16;
            output_buffer[dest_samples++] = os;
        }
        dest_frames++;
        context->src_pos += context->src_step;
    }
    return dest_frames;
}


int btstack_main(int argc, const char * argv[]);
int btstack_main(int argc, const char * argv[]){
    (void)argc;
    (void)argv;

#ifdef HAVE_POSIX_FILE_IO
    wav_writer_open("sine_resampled.wav", NUM_CHANNELS, 44100);
#endif
    btstack_resample_t resample;
    btstack_resample_init(&resample, NUM_CHANNELS);
    btstack_resample_set_src_step(&resample, 0xff00);

    int16_t input_buffer[TABLE_SIZE_441HZ * NUM_CHANNELS];
    int16_t output_buffer[200*NUM_CHANNELS];   // double the input size

    // generate multi channel sine
    int i, j;
    int src_pos = 0;
    for (i=0;i<TABLE_SIZE_441HZ;i++){
        for (j=0;j<NUM_CHANNELS;j++){
            input_buffer[src_pos++] = sine_int16[i];
        }
    }
    
    for (i=0;i<440*5;i++){
        uint16_t resampled_samples = btstack_resample_block(&resample, input_buffer, TABLE_SIZE_441HZ, output_buffer);
#ifdef HAVE_POSIX_FILE_IO
        wav_writer_write_int16(resampled_samples * NUM_CHANNELS, output_buffer);
#endif
    }

#ifdef HAVE_POSIX_FILE_IO                  
    wav_writer_close();
#endif

    printf("Done\n");

    return 0;
}
