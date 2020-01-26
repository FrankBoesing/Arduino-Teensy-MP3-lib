/*
   Arduino Audiocodecs

   Copyright (c) 2014-2020 Frank Bösing

   This library is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   This library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this library.  If not, see <http://www.gnu.org/licenses/>.

   The helix decoder itself as a different license, look at the subdirectories for more info.

   Diese Bibliothek ist freie Software: Sie können es unter den Bedingungen
   der GNU General Public License, wie von der Free Software Foundation,
   Version 3 der Lizenz oder (nach Ihrer Wahl) jeder neueren
   veröffentlichten Version, weiterverbreiten und/oder modifizieren.

   Diese Bibliothek wird in der Hoffnung, dass es nützlich sein wird, aber
   OHNE JEDE GEWÄHRLEISTUNG, bereitgestellt; sogar ohne die implizite
   Gewährleistung der MARKTFÄHIGKEIT oder EIGNUNG FÜR EINEN BESTIMMTEN ZWECK.
   Siehe die GNU General Public License für weitere Details.

   Sie sollten eine Kopie der GNU General Public License zusammen mit diesem
   Programm erhalten haben. Wenn nicht, siehe <http://www.gnu.org/licenses/>.

   Der Helixdecoder selbst hat eine eigene Lizenz, bitte für mehr Informationen
   in den Unterverzeichnissen nachsehen.

 */

/* The Helix-Library is modified for Teensy 3.1 */
// Total RAM Usage: ~35 KB


#include "play_sd_mp3.h"

#define MP3_SD_BUF_SIZE 2048                //Enough space for a complete stereo frame
#define MP3_BUF_SIZE  (MAX_NCHAN * MAX_NGRAN * MAX_NSAMP) //MP3 output buffer
#define DECODE_NUM_STATES 2                 //How many steps in decode() ?

#define MIN_FREE_RAM (35 + 1) * 1024 // 1KB Reserve

//#define CODEC_DEBUG

static AudioPlaySdMp3 * mp3objptr[NUM_IRQS];
void decodeMp3(AudioPlaySdMp3 *o);

void AudioPlaySdMp3::stop(void)
{
	if (irq_audiocodec) {
		NVIC_DISABLE_IRQ(irq_audiocodec);
		irq_audiocodec = 0;
	}
	playing = codec_stopped;

	if (buf[1]) {free(buf[1]); buf[1] = NULL;}
	if (buf[0]) {free(buf[0]); buf[0] = NULL;}
	freeBuffer();
	if (hMP3Decoder) {MP3FreeDecoder(hMP3Decoder); hMP3Decoder = NULL;};

	fclose();
}
/*
   float AudioPlaySdMp3::processorUsageMaxDecoder(void){
   //this is somewhat incorrect, it does not take the interruptions of update() into account -
   //therefore the returned number is too high.
   //Todo: better solution
   return (decode_cycles_max / (0.026*F_CPU)) * 100;
   };

   float AudioPlaySdMp3::processorUsageMaxSD(void){
   //this is somewhat incorrect, it does not take the interruptions of update() into account -
   //therefore the returned number is too high.
   //Todo: better solution
   return (decode_cycles_max_sd / (0.026*F_CPU)) * 100;
   };
 */

_FAST void intDecode0(void) {
	AudioPlaySdMp3 *o = mp3objptr[0];
	decodeMp3(o);
}

_FAST void intDecode1(void) {
	AudioPlaySdMp3 *o = mp3objptr[1];
	decodeMp3(o);
}

_FAST void intDecode2(void) {
	AudioPlaySdMp3 *o = mp3objptr[2];
	decodeMp3(o);
}

_FAST void intDecode3(void) {
	AudioPlaySdMp3 *o = mp3objptr[3];
	decodeMp3(o);
}

_FAST void intDecode4(void) {
	AudioPlaySdMp3 *o = mp3objptr[4];
	decodeMp3(o);
}

_FAST void intDecode5(void) {
	AudioPlaySdMp3 *o = mp3objptr[5];
	decodeMp3(o);
}

_FAST void intDecode6(void) {
	AudioPlaySdMp3 *o = mp3objptr[6];
	decodeMp3(o);
}

_FAST void intDecode7(void) {
	AudioPlaySdMp3 *o = mp3objptr[7];
	decodeMp3(o);
}

_FLASH void (*const intvects[8])(void) = {
	&intDecode0, &intDecode1, &intDecode2, &intDecode3,
	&intDecode4, &intDecode5, &intDecode6, &intDecode7
};

int AudioPlaySdMp3::play(void)
{
	lastError = ERR_CODEC_NONE;

	//find an unused interrupt:
	irq_audiocodec = idx_audiocodec = 0;
	for (unsigned irqn = 0; irqn < NUM_IRQS; irqn++) {
		if (!(NVIC_IS_ENABLED(irq_list[irqn])) ) {
			irq_audiocodec = irq_list[irqn];
			idx_audiocodec = irqn;
			break;
		}
	}

	if (!irq_audiocodec) {
		lastError = ERR_CODEC_NOINTERRUPT;
		return lastError;
	}

	initVars();

	sd_buf = allocBuffer(MP3_SD_BUF_SIZE);
	if (!sd_buf) {
		lastError = ERR_CODEC_OUT_OF_MEMORY;
		return lastError;
	}

	buf[0] = (short *) malloc(MP3_BUF_SIZE * sizeof(int16_t));
	buf[1] = (short *) malloc(MP3_BUF_SIZE * sizeof(int16_t));

	if (!buf[0] || !buf[1])
	{
		lastError = ERR_CODEC_OUT_OF_MEMORY;
		stop();
		return lastError;
	}

	hMP3Decoder = MP3InitDecoder();
	if (!hMP3Decoder)
	{
		lastError = ERR_CODEC_OUT_OF_MEMORY;
		stop();
		return lastError;
	}

	//Read-ahead 10 Bytes to detect ID3
	sd_left =  fread(sd_buf, 10);

	//Skip ID3, if existent
	int skip = skipID3(sd_buf);
	if (skip) {
		size_id3 = skip;
		int b = skip & 0xfffffe00;
		fseek(b);
		sd_left = 0;
//		Serial.print("skip");
//		Serial.print(fposition());
	} else size_id3 = 0;

	//Fill buffer from the beginning with fresh data
	sd_left = fillReadBuffer(file, sd_buf, sd_buf, sd_left, MP3_SD_BUF_SIZE);

	if (!sd_left) {
		lastError = ERR_CODEC_FILE_NOT_FOUND;
		stop();
		return lastError;
	}

	decoded_length[0] = 0;
	decoded_length[1] = 0;
	decoding_block = 0;
	decoding_state = 0;

	play_pos = 0;

	sd_p = sd_buf;

	mp3objptr[idx_audiocodec] = this;
	//for (size_t i = 0; i < DECODE_NUM_STATES; i++) decodeMp3();
	for (size_t i = 0; i < DECODE_NUM_STATES; i++) intvects[idx_audiocodec]();

	if ((mp3FrameInfo.samprate != AUDIOCODECS_SAMPLE_RATE ) || (mp3FrameInfo.bitsPerSample != 16) || (mp3FrameInfo.nChans > 2)) {
		//Serial.println("incompatible MP3 file.");
		lastError = ERR_CODEC_FORMAT;
		stop();
		return lastError;
	}
	decoding_block = 1;

	//_VectorsRam[irq_audiocodec + 16] = &decodeMp3;
	_VectorsRam[irq_audiocodec + 16] = intvects[idx_audiocodec];
	playing = codec_playing;
	initSwi(irq_audiocodec);

	return lastError;
}

//decoding-interrupt
_FAST void decodeMp3(AudioPlaySdMp3 *o)
{

	//AudioPlaySdMp3 *o = mp3objptr[0];

	int db = o->decoding_block;

	if ( o->decoded_length[db] > 0 ) return; //this block is playing, do NOT fill it

	uint32_t cycles = ARM_DWT_CYCCNT;
	int eof = false;

	switch (o->decoding_state) {

	case 0:
	{

		o->sd_left = o->fillReadBuffer( o->file, o->sd_buf, o->sd_p, o->sd_left, MP3_SD_BUF_SIZE);
		if (!o->sd_left) { eof = true; goto mp3end; }
		o->sd_p = o->sd_buf;

		uint32_t cycles_rd = (ARM_DWT_CYCCNT - cycles);
		if (cycles_rd > o->decode_cycles_max_read ) o->decode_cycles_max_read = cycles_rd;
		break;
	}

	case 1:
	{
		// find start of next MP3 frame - assume EOF if no sync found
		int offset = MP3FindSyncWord(o->sd_p, o->sd_left);

		if (offset < 0) {
			//Serial.println("No sync"); //no error at end of file
			eof = true;
			goto mp3end;
		}

		o->sd_p += offset;
		o->sd_left -= offset;

		int decode_res = MP3Decode(o->hMP3Decoder, &o->sd_p, (int*)&o->sd_left, o->buf[db], 0);

		switch (decode_res)
		{
		case ERR_MP3_NONE:
		{
			MP3GetLastFrameInfo(o->hMP3Decoder, &o->mp3FrameInfo);
			o->decoded_length[db] = o->mp3FrameInfo.outputSamps;
			break;
		}

		case ERR_MP3_MAINDATA_UNDERFLOW:
		{
			break;
		}

		default:
		{
			AudioPlaySdMp3::lastError = decode_res;
			eof = true;
			break;
		}
		}

		cycles = (ARM_DWT_CYCCNT - cycles);
		if (cycles > o->decode_cycles_max ) o->decode_cycles_max = cycles;
		break;
	}
	}//switch

mp3end:

	o->decoding_state++;
	if (o->decoding_state >= DECODE_NUM_STATES) o->decoding_state = 0;

	if (eof) o->stop();

}

//runs in ISR
_FAST
void AudioPlaySdMp3::update(void)
{
	audio_block_t *block_left;
	audio_block_t *block_right;
	//Serial.println(irq_audiocodec);
	//paused or stopped ?
	if (playing != codec_playing) return;

	//chain decoder-interrupt.
	//to give the user-sketch some cpu-time, only chain
	//if the swi is not active currently.
	int db = decoding_block;
	if (decoded_length[db] == 0)
		if (!NVIC_IS_ACTIVE(irq_audiocodec))
			NVIC_TRIGGER_IRQ(irq_audiocodec);

	//determine the block we're playing from
	int playing_block = 1 - db;
	if (decoded_length[playing_block] <= 0) return;

	// allocate the audio blocks to transmit
	block_left = allocate();
	if (block_left == NULL) return;

	uintptr_t pl = play_pos;

	if (mp3FrameInfo.nChans == 2) {
		// if we're playing stereo, allocate another
		// block for the right channel output
		block_right = allocate();
		if (block_right == NULL) {
			release(block_left);
			return;
		}

		memcpy_frominterleaved(&block_left->data[0], &block_right->data[0], buf[playing_block] + pl);

		pl += AUDIO_BLOCK_SAMPLES * 2;
		transmit(block_left, 0);
		transmit(block_right, 1);
		release(block_right);
		decoded_length[playing_block] -= AUDIO_BLOCK_SAMPLES * 2;

	} else
	{
		// if we're playing mono, no right-side block
		// let's do a (hopefully good optimized) simple memcpy
		memcpy(block_left->data, buf[playing_block] + pl, AUDIO_BLOCK_SAMPLES * sizeof(short));

		pl += AUDIO_BLOCK_SAMPLES;
		transmit(block_left, 0);
		transmit(block_left, 1);
		decoded_length[playing_block] -= AUDIO_BLOCK_SAMPLES;

	}

	samples_played += AUDIO_BLOCK_SAMPLES;

	release(block_left);

	//Switch to the next block if we have no data to play anymore:
	if (decoded_length[playing_block] == 0)
	{
		decoding_block = playing_block;
		play_pos = 0;
	} else
		play_pos = pl;

}
