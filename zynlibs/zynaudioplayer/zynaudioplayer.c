/*  Audio file player library for Zynthian
    Copyright (C) 2021 Brian Walton <brian@riban.co.uk>
    License: LGPL V3
*/

#include "zynaudioplayer.h"

#include <stdio.h> //provides printf
#include <string.h> //provides strcmp, memset
#include <jack/jack.h> //provides interface to JACK
#include <jack/midiport.h> //provides JACK MIDI interface
#include <sndfile.h> //provides sound file manipulation
#include <samplerate.h> //provides samplerate conversion
#include <pthread.h> //provides multithreading
#include <unistd.h> //provides usleep
#include <stdlib.h> //provides exit

#define DPRINTF(fmt, args...) if(g_bDebug) printf(fmt, ## args)

enum playState {
	STOPPED		= 0,
	STARTING	= 1,
	PLAYING		= 2,
	STOPPING	= 3
};

enum seekState {
    IDLE        = 0, // Not seeking
    SEEKING     = 1, // Seeking within file
    LOADING     = 2  // Seek complete, loading data from file
};

#define AUDIO_BUFFER_SIZE 50000 // 50000 is approx. 1s of audio
#define RING_BUFFER_SIZE AUDIO_BUFFER_SIZE * 2

jack_client_t* g_pJackClient = NULL;
jack_port_t* g_pJackOutA = NULL;
jack_port_t* g_pJackOutB = NULL;
jack_port_t * g_pJackMidiIn = NULL;

uint8_t g_bDebug = 0;
uint8_t g_bFileOpen = 0; // 1 whilst file is open - used to flag thread to close file
uint8_t g_bMore = 0; // 1 if there is more data to read from file, i.e. not at end of file or looping
uint8_t g_nSeek = IDLE; //!@todo Can we conbine seek state with play state?// Seek state
uint8_t g_nPlayState = STOPPED; // Current playback state (STOPPED|STARTING|PLAYING|STOPPING)
uint8_t g_bLoop = 0; // 1 to loop at end of song
jack_nframes_t g_nSamplerate = 44100; // Playback samplerate set by jackd
struct SF_INFO  g_sf_info; // Structure containing currently loaded file info
pthread_t g_threadFile; // ID of file reader thread
struct RING_BUFFER g_ringBuffer; // Used to pass data from file reader to jack process
//!@todo Replace g_nChannelB with tracks mix down feature
size_t g_nChannelB = 0; // Offset of samples for channel B (0 for mono source or 1 for multi-channel)
jack_nframes_t g_nPlaybackPosFrames = 0; // Current playback position in frames since start of audio
size_t g_nLastFrame = -1; // Position within ring buffer of last frame or -1 if not playing last buffer iteration
unsigned int g_nSrcQuality = SRC_SINC_FASTEST;
char g_sFilename[128];
float g_fLevel = 1.0; // Audio level (volume) 0..1

struct RING_BUFFER {
    size_t front; // Offset within buffer for next read
    size_t back; // Offset within buffer for next write
    size_t size; // Quantity of elements in buffer
    float dataA[RING_BUFFER_SIZE * 2];
    float dataB[RING_BUFFER_SIZE * 2];
};

/*  @brief  Initialise ring buffer
*   @param  buffer Pointer to ring buffer
*/
void ringBufferInit(struct RING_BUFFER * buffer) {
    buffer->front = 0;
    buffer->back = 0;
    buffer->size = RING_BUFFER_SIZE;
    memset(buffer->dataA, 0, RING_BUFFER_SIZE * sizeof(buffer->dataA[0]));
    memset(buffer->dataB, 0, RING_BUFFER_SIZE * sizeof(buffer->dataB[0]));
}

/*  @brief  Push data to back of ring buffer
*   @param  buffer Pointer to ring buffer
*   @param  dataA Pointer to start of A data to push
*   @param  dataB Pointer to start of B data to push
*   @param  size Quantity of data to push (size of data buffer)
*   @retval size_t Quantity of data actually added to queue
*/
size_t ringBufferPush(struct RING_BUFFER * buffer, float* dataA, float* dataB, size_t size) {
    size_t count = 0;
    if(buffer->back < buffer->front) {
        // Can populate from back to front
        for(; buffer->back < buffer->front; ++buffer->back) {
            if(count >= size)
                break;
            buffer->dataA[buffer->back] = *(dataA + count);
            buffer->dataB[buffer->back] = *(dataB + count++);
        }
    } else {
        // Populate to end of buffer then wrap and populate to front
        for(; buffer->back < buffer->size; ++buffer->back) {
            if(count >= size)
                break;
            buffer->dataA[buffer->back] = *(dataA + count);
            buffer->dataB[buffer->back] = *(dataB + count++);
        }
        if(count < size) {
            for(buffer->back = 0; buffer->back < buffer->front; ++buffer->back) {
                if(count >= size)
                    break;
                buffer->dataA[buffer->back] = *(dataA + count);
                buffer->dataB[buffer->back] = *(dataB + count++);
            }
        }
    }
    //DPRINTF("ringBufferPush size=%u count=%u front=%u back=%u\n", size, count, buffer->front, buffer->back);
    return count;
}

/*  @brief  Pop data from front of ring buffer
*   @param  buffer Pointer to ring buffer
*   @param  dataA Pointer to A data buffer to receive popped data
*   @param  dataB Pointer to B data buffer to receive popped data
*   @param  size Quantity of data to pop (size of data buffer)
*   @retval size_t Quantity of data actually removed from queue
*/
size_t ringBufferPop(struct RING_BUFFER * buffer, float* dataA, float* dataB, size_t size) {
    //DPRINTF("ringBuffPop size=%u, front=%u, back=%u\n", size, buffer->front, buffer->back);
    if(buffer->back == buffer->front)
        return 0;
    size_t count = 0;
    if(buffer->back > buffer->front) {
        for(; buffer->back > buffer->front; ++buffer->front) {
            if(count >= size)
                break;
            *(dataA + count) = buffer->dataA[buffer->front];
            *(dataB + count++) = buffer->dataB[buffer->front];
        }
    } else {
        // Pop to end of buffer then wrap and pop to back
        for(; buffer->front < buffer->size; ++buffer->front) {
            if(count >= size)
                break;
            *(dataA + count) = buffer->dataA[buffer->front];
            *(dataB + count++) = buffer->dataB[buffer->front];
        }
        if (count < size) {
            for(buffer->front = 0; buffer->front <= buffer->back; ++buffer->front) {
                if(count >= size)
                    break;
                *(dataA + count) = buffer->dataA[buffer->back];
                *(dataB + count++) = buffer->dataB[buffer->back];
            }
        }
    }
    return count;
}

/*  @brief  Get available space within ring buffer
*   @param  buffer Pointer to ring buffer
*   @retval size_t Quantity of free elements in ring buffer
*/
size_t ringBufferGetFree(struct RING_BUFFER * buffer) {
    if(buffer->front > buffer->back)
        return buffer->front - buffer->back;
    return buffer->size - buffer->back + buffer->front;
} 

/*  @brief  Get quantity of elements used within ring buffer
*   @param  buffer Pointer to ring buffer
*   @retval size_t Quantity of used elements in ring buffer
*/
size_t ringBufferGetUsed(struct RING_BUFFER * buffer) {
    if(buffer->back >= buffer->front)
        return buffer->back - buffer->front;
    return buffer->size - buffer->front + buffer->back;
} 

/*** Public functions exposed as external C functions in header ***/

void enableDebug(uint8_t bEnable) {
    printf("libaudioplayer setting debug mode %s\n", bEnable?"on":"off");
    g_bDebug = bEnable;
}

uint8_t open(const char* filename) {
    closeFile();
    strcpy(g_sFilename, filename);
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);

    if(pthread_create(&g_threadFile, &attr, fileThread, NULL)) {
        fprintf(stderr, "Failed to create file reading thread\n");
        closeFile();
        return 0;
    }
    return 1;
}

float getFileDuration(const char* filename) {
    SF_INFO info;
    info.format = 0;
    info.samplerate = 0;
    SNDFILE* pFile = sf_open(filename, SFM_READ, &info);
    sf_close(pFile);
    if(info.samplerate)
        return (float)info.frames / info.samplerate;
    return 0.0f;
}

void closeFile() {
    stopPlayback();
    g_bFileOpen = 0;
    void* status;
    pthread_join(g_threadFile, &status);
    g_sFilename[0] = '\0';
}

uint8_t save(const char* filename) {
    //!@todo Implement save
    return 0;
}

const char* getFilename() {
    return g_sFilename;
}

float getDuration() {
    if(g_sf_info.samplerate)
        return (float)g_sf_info.frames / g_sf_info.samplerate;
    return 0.0f;
}

void setPosition(float time) {
    g_nPlaybackPosFrames = time * g_nSamplerate;
    g_nSeek = SEEKING;
}

float getPosition() {
    return (float)g_nPlaybackPosFrames / g_nSamplerate;
}

void setLoop(uint8_t bLoop) {
	g_bLoop = bLoop;
	g_bMore = 1;
}

void startPlayback() {
	if(!g_pJackClient)
		return;
	g_nPlayState = STARTING;
}

void stopPlayback() {
	if(g_nPlayState == STOPPED)
		return;
	g_nPlayState = STOPPING;
}

uint8_t getPlayState() {
	return g_nPlayState;
}

int getSamplerate() {
    return g_sf_info.samplerate;
}

int getChannels() {
    return g_sf_info.channels;
}

int getFrames() {
    return g_sf_info.frames;
}

int getFormat() {
    return g_sf_info.format;
}

size_t getQueueFront() {
    return g_ringBuffer.front;
}

size_t getQueueBack() {
    return g_ringBuffer.back;
}

/*** Private functions not exposed as external C functions (not declared in header) ***/

// Clean up before library unloads
void end() {
    closeFile();
    if(g_pJackClient)
        jack_client_close(g_pJackClient);
}

// Handle JACK process callback
static int onJackProcess(jack_nframes_t nFrames, void *notused) {
    static size_t count;
    jack_default_audio_sample_t *pOutA = (jack_default_audio_sample_t*)jack_port_get_buffer(g_pJackOutA, nFrames);
    jack_default_audio_sample_t *pOutB = (jack_default_audio_sample_t*)jack_port_get_buffer(g_pJackOutB, nFrames);
    count = 0;

    if(g_nPlayState == STARTING && g_nSeek == IDLE)
        g_nPlayState = PLAYING;

    if(g_nPlayState == PLAYING || g_nPlayState == STOPPING) {
        count = ringBufferPop(&g_ringBuffer, pOutA, pOutB, nFrames);
        if(g_nPlayState == STOPPING || g_nLastFrame == g_ringBuffer.front) {
            g_nPlayState = STOPPED;
            g_nLastFrame = -1;
            DPRINTF("onJackProcess: Stopped\n");
        }
        //if(count) DPRINTF("jack process count=%u, g_nLastFrame=%u front=%u\n", count, g_nLastFrame,  g_ringBuffer.front);
    }
    for(size_t offset = 0; offset < count; ++offset) {
        pOutA[offset] *= g_fLevel;
        pOutB[offset] *= g_fLevel;
    }
    //!@todo Soft mute during STOPPING
    // Silence remainder of frame
    memset(pOutA + count * sizeof(jack_default_audio_sample_t), 0, (nFrames - count) * sizeof(jack_default_audio_sample_t));
    memset(pOutB + count * sizeof(jack_default_audio_sample_t), 0, (nFrames - count) * sizeof(jack_default_audio_sample_t));

    // Process MIDI input
    void* pMidiBuffer = jack_port_get_buffer(g_pJackMidiIn, nFrames);
    jack_midi_event_t midiEvent;
    jack_nframes_t nCount = jack_midi_get_event_count(pMidiBuffer);
    for(jack_nframes_t i = 0; i < nCount; i++)
    {
        jack_midi_event_get(&midiEvent, pMidiBuffer, i);
        if((midiEvent.buffer[0] & 0xF0) == 0xB0)
        {
            switch(midiEvent.buffer[1])
            {
                case 1:
                    setPosition(midiEvent.buffer[2] * getDuration() / 127);
                    break;
                case 7:
                    g_fLevel = (float)midiEvent.buffer[2] / 100.0;
                    break;
                case 68:
                    if(midiEvent.buffer[2] > 63)
                        startPlayback();
                    else
                        stopPlayback();
                    break;
                case 69:
                    setLoop(midiEvent.buffer[2] > 63);
                    break;
            }
        }
    }
	return 0;
}

// Handle JACK process callback
int onJackSamplerate(jack_nframes_t nFrames, void *pArgs) {
    DPRINTF("zynaudioplayer: Jack sample rate: %u\n", nFrames);
    g_nSamplerate = nFrames;
    return 0;
}

void* fileThread(void* param) {
    g_sf_info.format = 0; // This triggers sf_open to populate info structure
    SNDFILE* pFile = sf_open(g_sFilename, SFM_READ, &g_sf_info);
    if(!pFile) {
        fprintf(stderr, "libaudioplayer failed to open file %s: %s\n", g_sFilename, sf_strerror(pFile));
        pthread_exit(NULL);
    }
    g_bFileOpen = 1;
    g_nChannelB = (g_sf_info.channels == 1)?0:1; // Mono or stereo based on first one or two channels

    g_bMore = 1;
    g_nSeek = SEEKING;
    g_nPlaybackPosFrames = 0;

    // Initialise samplerate converter
    SRC_DATA srcData;
    float pBufferOut[AUDIO_BUFFER_SIZE]; // Buffer used to write converted sample data to
    float pBufferIn[AUDIO_BUFFER_SIZE]; // Buffer used to read sample data from file
    srcData.data_in = pBufferIn;
    srcData.data_out = pBufferOut;
    srcData.src_ratio = (float)g_nSamplerate / g_sf_info.samplerate;
    srcData.output_frames = AUDIO_BUFFER_SIZE;
    size_t nMaxRead = AUDIO_BUFFER_SIZE;
    if(srcData.src_ratio > 1.0)
        nMaxRead = (float)AUDIO_BUFFER_SIZE / srcData.src_ratio;
    nMaxRead /= g_sf_info.channels;
    int nError;
    SRC_STATE* pSrcState = src_new(g_nSrcQuality, g_sf_info.channels, &nError);

    while(g_bFileOpen) {
        if(g_nSeek) {
            // Main thread has signalled seek within file
            ringBufferInit(&g_ringBuffer);
            size_t nNewPos = g_nPlaybackPosFrames;
            if(srcData.src_ratio)
                nNewPos = g_nPlaybackPosFrames / srcData.src_ratio;
            sf_seek(pFile, nNewPos, SEEK_SET);
            g_nSeek = LOADING;
            src_reset(pSrcState);
            srcData.end_of_input = 0;
        }
        if(g_bMore || g_nSeek == LOADING)
        {
            // Load block of data from file to SRC buffer
            int nRead;
            if(srcData.src_ratio == 1.0) {
                // No SRC required so populate SRC output buffer directly
                nRead = sf_readf_float(pFile, pBufferOut, nMaxRead);
            } else {
                // Populate SRC input buffer before SRC process
                nRead = sf_readf_float(pFile, pBufferIn, nMaxRead);
            }
            if(nRead == nMaxRead) {
                // Filled buffer from file so probably more data to read
                g_bMore = 1;
                srcData.end_of_input = 0;
            }
            else if(g_bLoop) {
                // Short read - looping so flag to seek to start
                sf_seek(pFile, 0, SEEK_SET);
                g_bMore = 1;
                srcData.end_of_input = 1;
            } else {
                // Short read - assume at end of file
                g_bMore = 0;
                srcData.end_of_input = 1;
                DPRINTF("zynaudioplayer read to end of input file\n");
            }
            if(srcData.src_ratio != 1.0) {
                // We need to perform SRC on this block of code
                srcData.input_frames = nRead;
                int rc = src_process(pSrcState, &srcData);
                nRead = srcData.output_frames_gen;
                if(rc) {
                    DPRINTF("SRC failed with error %d, %u frames generated\n", nRead, srcData.output_frames_gen);
                } else {
                    DPRINTF("SRC suceeded - %u frames generated\n", srcData.output_frames_gen);
                }
            } else {
                DPRINTF("srcData.src_ratio=%f\n", srcData.src_ratio);
            }
            while(ringBufferGetFree(&g_ringBuffer) < nRead) {
                // Wait until there is sufficient space in ring buffer to add new sample data
                usleep(1000);
                if(g_nSeek == SEEKING || g_bFileOpen == 0)
                    break;
            }
            if(g_nSeek != SEEKING && g_bFileOpen) {
                // De-interpolate samples
                for(size_t offset = 0; offset < nRead; offset += g_sf_info.channels) {
                    //!@todo Sum odd/even channels to A/B output or mono to both from single channel file
                    if(0 == ringBufferPush(&g_ringBuffer, pBufferOut + offset, pBufferOut + offset + g_nChannelB, 1))
                        break; // Shouldn't underun due to previous wait for space but just in case...
                }
            }
            if(g_bMore == 0)
                g_nLastFrame = g_ringBuffer.back;

            if(g_nSeek == LOADING)
                g_nSeek = IDLE;
        }
        usleep(10000);
    }
    if(pFile) {
        int nError = sf_close(pFile);
        if(nError != 0)
            fprintf(stderr, "libaudioplayer failed to close file with error code %d\n", nError);
    }
    ringBufferInit(&g_ringBuffer); // Don't want audio playing from closed file
    g_nPlaybackPosFrames = 0;
    g_nLastFrame = -1;
    pSrcState = src_delete(pSrcState);
    DPRINTF("File reader thread ended\n");
    pthread_exit(NULL);
}

void init() {
    printf("zynaudioplayer init\n");
    ringBufferInit(&g_ringBuffer);

	// Register with Jack server
	char *sServerName = NULL;
	jack_status_t nStatus;
	jack_options_t nOptions = JackNoStartServer;

	if ((g_pJackClient = jack_client_open("zynaudioplayer", nOptions, &nStatus, sServerName)) == 0) {
		fprintf(stderr, "libaudioplayer failed to start jack client: %d\n", nStatus);
		exit(1);
	}

    g_nSamplerate = jack_get_sample_rate(g_pJackClient);

	// Create audio output ports
	if (!(g_pJackOutA = jack_port_register(g_pJackClient, "output_a", JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0))) {
		fprintf(stderr, "libaudioplayer cannot register audio output port A\n");
		exit(1);
	}
	if (!(g_pJackOutB = jack_port_register(g_pJackClient, "output_b", JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0))) {
		fprintf(stderr, "libaudioplayer cannot register audio output port B\n");
		exit(1);
	}

    // Create MIDI input port
    if(!(g_pJackMidiIn = jack_port_register(g_pJackClient, "input", JACK_DEFAULT_MIDI_TYPE, JackPortIsInput, 0)))
    {
        fprintf(stderr, "libzynaudioplayer cannot register MIDI input port\n");
        exit(1);
    }

	// Register the cleanup function to be called when program exits
	//atexit(end);

	// Register the callback to process audio and MIDI
	jack_set_process_callback(g_pJackClient, onJackProcess, 0);

	if (jack_activate(g_pJackClient)) {
		fprintf(stderr, "libaudioplayer cannot activate client\n");
		exit(1);
	}
}

const char* getFileInfo(const char* filename, int type) {
    SF_INFO info;
    info.format = 0;
    info.samplerate = 0;
    SNDFILE* pFile = sf_open(filename, SFM_READ, &info);
    const char* pValue = sf_get_string(pFile, type);
    if(pValue) {
        sf_close(pFile);
        return pValue;
    }
    sf_close(pFile);
    return "";
}

uint8_t setSrcQuality(unsigned int quality) {
    if(quality > SRC_LINEAR)
        return 0;
    g_nSrcQuality = quality;
    return 1;
}

void setVolume(float level) {
    if(level < 0 || level > 2)
        return;
    g_fLevel = level;
}

float getVolume() {
    return g_fLevel;
}