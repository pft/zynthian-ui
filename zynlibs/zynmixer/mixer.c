/*
 * ******************************************************************
 * ZYNTHIAN PROJECT: Audio Mixer Library
 *
 * Library providing stereo audio summing mixer
 *
 * Copyright (C) 2019-2024 Brian Walton <brian@riban.co.uk>
 *
 * ******************************************************************
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * For a full copy of the GNU General Public License see the LICENSE.txt file.
 *
 * ******************************************************************
 */

#include <math.h>    //provides fabs isinf
#include <pthread.h> //provides multithreading
#include <stdio.h>   //provides printf
#include <stdlib.h>  //provides exit
#include <string.h>  // provides memset
#include <unistd.h>  // provides sleep

#include "mixer.h"

#include "tinyosc.h"
#include <arpa/inet.h> // provides inet_pton

// #define DEBUG

#define MAX_CHANNELS 32
#define MAX_OSC_CLIENTS 5

static uint8_t CHANNEL = 0;
static uint8_t MIXBUS = 1;

pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
char g_oscbuffer[1024];  // Used to send OSC messages
char g_oscpath[64];      //!@todo Ensure path length is sufficient for all paths, e.g. /mixer/channel/xx/fader
int g_oscfd = -1;        // File descriptor for OSC socket
int g_bOsc  = 0;         // True if OSC client subscribed
pthread_t g_eventThread; // ID of low priority event thread
int g_sendEvents = 1;    // Set to 0 to exit event thread
int g_solo       = 0;    // True if any channel solo enabled

// Structure describing a channel strip
struct channel_strip {
    jack_port_t* inPortA;  // Jack input port A
    jack_port_t* inPortB;  // Jack input port B
    jack_port_t* outPortA; // Jack output port A
    jack_port_t* outPortB; // Jack output port B
    float level;           // Current fader level 0..1
    float reqlevel;        // Requested fader level 0..1
    float balance;         // Current balance -1..+1
    float reqbalance;      // Requested balance -1..+1
    float send[MAX_CHANNELS]; // Current fx send levels
    float dpmA;            // Current peak programme A-leg
    float dpmB;            // Current peak programme B-leg
    float holdA;           // Current peak hold level A-leg
    float holdB;           // Current peak hold level B-leg
    float dpmAlast;        // Last peak programme A-leg
    float dpmBlast;        // Last peak programme B-leg
    float holdAlast;       // Last peak hold level A-leg
    float holdBlast;       // Last peak hold level B-leg
    uint8_t mute;          // 1 if muted
    uint8_t solo;          // 1 if solo
    uint8_t mono;          // 1 if mono
    uint8_t ms;            // 1 if MS decoding
    uint8_t phase;         // 1 if channel B phase reversed
    uint8_t normalise;     // 1 if channel normalised to main output
    uint8_t inRouted;      // 1 if source routed to channel
    uint8_t outRouted;     // 1 if output routed
    uint8_t enable_dpm;    // 1 to enable calculation of peak meter
};

struct fx_send {
    jack_port_t* outPortA; // Jack output port A
    jack_port_t* outPortB; // Jack output port B
    jack_default_audio_sample_t* bufferA; // Holds audio samples
    jack_default_audio_sample_t* bufferB; // Holds audio samples
    float level;           // Current fader level 0..1
};

jack_client_t* g_chanJackClient;
jack_client_t* g_mixbusJackClient;
struct channel_strip* g_channelStrips[MAX_CHANNELS];
struct channel_strip* g_mixbusStrips[MAX_CHANNELS];
struct fx_send* g_fxSends[MAX_CHANNELS];
unsigned int g_nDampingCount  = 0;
unsigned int g_nDampingPeriod = 10; // Quantity of cycles between applying DPM damping decay
unsigned int g_nHoldCount     = 0;
float g_fDpmDecay             = 0.9;             // Factor to scale for DPM decay - defines resolution of DPM decay
struct sockaddr_in g_oscClient[MAX_OSC_CLIENTS]; // Array of registered OSC clients
char g_oscdpm[20];
jack_nframes_t g_samplerate                     = 48000; // Jack samplerate used to calculate damping factor
jack_nframes_t g_buffersize                     = 1024;  // Jack buffer size used to calculate damping factor
jack_default_audio_sample_t* g_mainNoramliseBufferA    = NULL;  // Ponter to main output normalised buffer used for normalising effects sends to main mixbus
jack_default_audio_sample_t* g_mainNoramliseBufferB    = NULL;  // Ponter to main output normalised buffer used for normalising effects sends to main mixbus

static float convertToDBFS(float raw) {
    if (raw <= 0)
        return -200;
    float fValue = 20 * log10f(raw);

    if (fValue < -200)
        fValue = -200;
    return fValue;
}

void sendOscFloat(const char* path, float value) {
    if (g_oscfd == -1)
        return;
    for (int i = 0; i < MAX_OSC_CLIENTS; ++i) {
        if (g_oscClient[i].sin_addr.s_addr == 0)
            continue;
        int len = tosc_writeMessage(g_oscbuffer, sizeof(g_oscbuffer), path, "f", value);
        sendto(g_oscfd, g_oscbuffer, len, MSG_CONFIRM | MSG_DONTWAIT, (const struct sockaddr*)&g_oscClient[i], sizeof(g_oscClient[i]));
    }
}

void sendOscInt(const char* path, int value) {
    if (g_oscfd == -1)
        return;
    for (int i = 0; i < MAX_OSC_CLIENTS; ++i) {
        if (g_oscClient[i].sin_addr.s_addr == 0)
            continue;
        int len = tosc_writeMessage(g_oscbuffer, sizeof(g_oscbuffer), path, "i", value);
        sendto(g_oscfd, g_oscbuffer, len, MSG_CONFIRM | MSG_DONTWAIT, (const struct sockaddr*)&g_oscClient[i], sizeof(g_oscClient[i]));
    }
}

void* eventThreadFn(void* param) {
    while (g_sendEvents) {
        if (g_bOsc) {
            for (unsigned int chan = 0; chan < MAX_CHANNELS; ++chan) {
                if (g_channelStrips[chan]) {
                    if ((int)(100000 * g_channelStrips[chan]->dpmAlast) != (int)(100000 * g_channelStrips[chan]->dpmA)) {
                        sprintf(g_oscdpm, "/mixer/channel/%d/dpma", chan);
                        sendOscFloat(g_oscdpm, convertToDBFS(g_channelStrips[chan]->dpmA));
                        g_channelStrips[chan]->dpmAlast = g_channelStrips[chan]->dpmA;
                    }
                    if ((int)(100000 * g_channelStrips[chan]->dpmBlast) != (int)(100000 * g_channelStrips[chan]->dpmB)) {
                        sprintf(g_oscdpm, "/mixer/channel/%d/dpmb", chan);
                        sendOscFloat(g_oscdpm, convertToDBFS(g_channelStrips[chan]->dpmB));
                        g_channelStrips[chan]->dpmBlast = g_channelStrips[chan]->dpmB;
                    }
                    if ((int)(100000 * g_channelStrips[chan]->holdAlast) != (int)(100000 * g_channelStrips[chan]->holdA)) {
                        sprintf(g_oscdpm, "/mixer/channel/%d/holda", chan);
                        sendOscFloat(g_oscdpm, convertToDBFS(g_channelStrips[chan]->holdA));
                        g_channelStrips[chan]->holdAlast = g_channelStrips[chan]->holdA;
                    }
                    if ((int)(100000 * g_channelStrips[chan]->holdBlast) != (int)(100000 * g_channelStrips[chan]->holdB)) {
                        sprintf(g_oscdpm, "/mixer/channel/%d/holdb", chan);
                        sendOscFloat(g_oscdpm, convertToDBFS(g_channelStrips[chan]->holdB));
                        g_channelStrips[chan]->holdBlast = g_channelStrips[chan]->holdB;
                    }
                }
                if (g_mixbusStrips[chan]) {
                    if ((int)(100000 * g_mixbusStrips[chan]->dpmAlast) != (int)(100000 * g_mixbusStrips[chan]->dpmA)) {
                        sprintf(g_oscdpm, "/mixer/buses/%d/dpma", chan);
                        sendOscFloat(g_oscdpm, convertToDBFS(g_mixbusStrips[chan]->dpmA));
                        g_mixbusStrips[chan]->dpmAlast = g_mixbusStrips[chan]->dpmA;
                    }
                    if ((int)(100000 * g_mixbusStrips[chan]->dpmBlast) != (int)(100000 * g_mixbusStrips[chan]->dpmB)) {
                        sprintf(g_oscdpm, "/mixer/buses/%d/dpmb", chan);
                        sendOscFloat(g_oscdpm, convertToDBFS(g_mixbusStrips[chan]->dpmB));
                        g_mixbusStrips[chan]->dpmBlast = g_mixbusStrips[chan]->dpmB;
                    }
                    if ((int)(100000 * g_mixbusStrips[chan]->holdAlast) != (int)(100000 * g_mixbusStrips[chan]->holdA)) {
                        sprintf(g_oscdpm, "/mixer/buses/%d/holda", chan);
                        sendOscFloat(g_oscdpm, convertToDBFS(g_mixbusStrips[chan]->holdA));
                        g_mixbusStrips[chan]->holdAlast = g_mixbusStrips[chan]->holdA;
                    }
                    if ((int)(100000 * g_mixbusStrips[chan]->holdBlast) != (int)(100000 * g_mixbusStrips[chan]->holdB)) {
                        sprintf(g_oscdpm, "/mixer/buses/%d/holdb", chan);
                        sendOscFloat(g_oscdpm, convertToDBFS(g_mixbusStrips[chan]->holdB));
                        g_mixbusStrips[chan]->holdBlast = g_mixbusStrips[chan]->holdB;
                    }
                }
            }
        }
        usleep(10000);
    }
}

static int onJackProcess(jack_nframes_t frames, void* args) {
    jack_default_audio_sample_t *pInA, *pInB, *pOutA, *pOutB, *pChanOutA, *pChanOutB, *pMainOutA, *pMainOutB;
    unsigned int frame;
    float curLevelA, curLevelB, reqLevelA, reqLevelB, fDeltaA, fDeltaB, fSampleA, fSampleB, fSampleM, fSendA[MAX_CHANNELS], fSendB[MAX_CHANNELS];
    uint8_t mixbus = *((uint8_t*)args); 

    pthread_mutex_lock(&mutex);

    if (mixbus) {
        // Clear the main mixbus output buffers to allow them to be directly populated with effects return normalisd frames.
        memset(g_mainNoramliseBufferA, 0.0, frames * sizeof(jack_default_audio_sample_t));
        memset(g_mainNoramliseBufferB, 0.0, frames * sizeof(jack_default_audio_sample_t));
    } else {
        // Clear send buffers.
        for (uint8_t send = 0; send < MAX_CHANNELS; ++send) {
            if (g_fxSends[send]) {
                memset(g_fxSends[send]->bufferA, 0.0, frames * sizeof(jack_default_audio_sample_t));
                memset(g_fxSends[send]->bufferB, 0.0, frames * sizeof(jack_default_audio_sample_t));
            }
        }
    }

    // Process each channel in reverse order (so that main mixbus is last)
    uint8_t chan = MAX_CHANNELS;
    while (chan--) {
        struct channel_strip* strip = mixbus?g_mixbusStrips[chan]:g_channelStrips[chan];
        if (strip == NULL)
            continue;

        // Only process connected inputs and mixbuses
        if (strip->inRouted || mixbus) {
            // Calculate current (last set) balance
            if (strip->balance > 0.0)
                curLevelA = strip->level * (1 - strip->balance);
            else
                curLevelA = strip->level;
            if (strip->balance < 0.0)
                curLevelB = strip->level * (1 + strip->balance);
            else
                curLevelB = strip->level;

            // Calculate mute and target level and balance (that we will fade to over this cycle period to avoid abrupt change clicks)
            //!@todo Crossfade send levels
            if (strip->mute || g_solo && strip->solo == 0 && (!mixbus || chan)) {
                // Do not mute aux if solo enabled
                strip->level = 0; // We can set this here because we have the data and will iterate towards 0 over this frame
                reqLevelA             = 0.0;
                reqLevelB             = 0.0;
            } else {
                if (strip->reqbalance > 0.0)
                    reqLevelA = strip->reqlevel * (1 - strip->reqbalance);
                else
                    reqLevelA = strip->reqlevel;
                if (strip->reqbalance < 0.0)
                    reqLevelB = strip->reqlevel * (1 + strip->reqbalance);
                else
                    reqLevelB = strip->reqlevel;
                strip->level   = strip->reqlevel;
                strip->balance = strip->reqbalance;
            }

            // Calculate the step change for each leg to apply on each sample in buffer for fade between last and this period's level
            fDeltaA = (reqLevelA - curLevelA) / frames;
            fDeltaB = (reqLevelB - curLevelB) / frames;

            // **Apply processing to audio samples**
            pInA = jack_port_get_buffer(strip->inPortA, frames);
            pInB = jack_port_get_buffer(strip->inPortB, frames);

            if (strip->outRouted) {
                // Direct output so create audio buffers
                pChanOutA = jack_port_get_buffer(strip->outPortA, frames);
                pChanOutB = jack_port_get_buffer(strip->outPortB, frames);
                memset(pChanOutA, 0.0, frames * sizeof(jack_default_audio_sample_t));
                memset(pChanOutB, 0.0, frames * sizeof(jack_default_audio_sample_t));
            } else {
                pChanOutA = pChanOutB = NULL;
            }
            // Iterate samples, scaling each and adding to output and set DPM if any samples louder than current DPM
            for (frame = 0; frame < frames; ++frame) {
                if (mixbus && chan == 0) {
                    fSampleA = pInA[frame] + g_mainNoramliseBufferA[frame];
                    fSampleB = pInB[frame] + g_mainNoramliseBufferB[frame];
                } else {
                    fSampleA = pInA[frame];
                    fSampleB = pInB[frame];
                }
                // Handle channel phase reverse
                if (strip->phase)
                    fSampleB = -fSampleB;

                // Decode M+S
                if (strip->ms) {
                    fSampleM = fSampleA + fSampleB;
                    fSampleB = fSampleA - fSampleB;
                    fSampleA = fSampleM;
                }

                // Handle mono
                if (strip->mono) {
                    fSampleA = (fSampleA + fSampleB) / 2.0;
                    fSampleB = fSampleA;
                }

                // Apply level adjustment
                fSampleA *= curLevelA;
                fSampleB *= curLevelB;

                // Check for error
                if (isinf(fSampleA))
                    fSampleA = 1.0;
                if (isinf(fSampleB))
                    fSampleB = 1.0;

                // Write sample to output buffer
                if (pChanOutA) {
                    pChanOutA[frame] += fSampleA;
                    pChanOutB[frame] += fSampleB;
                }
                if (!mixbus) {
                    // Add fx send output frames only for input channels
                    for (uint8_t send = 1; send < MAX_CHANNELS; ++send) {
                        if (g_fxSends[send]) {
                            g_fxSends[send]->bufferA[frame] += fSampleA * strip->send[send];
                            g_fxSends[send]->bufferB[frame] += fSampleB * strip->send[send];
                            if(isinf(g_fxSends[send]->bufferA[frame]))
                                g_fxSends[send]->bufferA[frame] = 1.0;
                            if(isinf(g_fxSends[send]->bufferB[frame]))
                                g_fxSends[send]->bufferB[frame] = 1.0;
                        }
                    }
                }

                // Add frames to main mixbus normalise buffer
                if (strip->normalise) {
                    g_mainNoramliseBufferA[frame] += fSampleA;
                    g_mainNoramliseBufferB[frame] += fSampleB;
                }

                curLevelA += fDeltaA;
                curLevelB += fDeltaB;

                // Process DPM
                if (strip->enable_dpm) {
                    fSampleA = fabs(fSampleA);
                    if (fSampleA > strip->dpmA)
                        strip->dpmA = fSampleA;
                    fSampleB = fabs(fSampleB);
                    if (fSampleB > strip->dpmB)
                        strip->dpmB = fSampleB;

                    // Update peak hold and scale DPM for damped release
                    if (strip->dpmA > strip->holdA)
                        strip->holdA = strip->dpmA;
                    if (strip->dpmB > strip->holdB)
                        strip->holdB = strip->dpmB;
                }
            }
            if (g_nHoldCount == 0) {
                // Only update peak hold each g_nHoldCount cycles
                strip->holdA = strip->dpmA;
                strip->holdB = strip->dpmB;
            }
            if (g_nDampingCount == 0) {
                // Only update damping release each g_nDampingCount cycles
                strip->dpmA *= g_fDpmDecay;
                strip->dpmB *= g_fDpmDecay;
            }
        } else if (strip->enable_dpm) {
            strip->dpmA  = -200.0;
            strip->dpmB  = -200.0;
            strip->holdA = -200.0;
            strip->holdB = -200.0;
        }
    }

    if (g_nDampingCount == 0)
        g_nDampingCount = g_nDampingPeriod;
    else
        --g_nDampingCount;
    if (g_nHoldCount == 0)
        g_nHoldCount = g_nDampingPeriod * 20;
    else
        --g_nHoldCount;

    pthread_mutex_unlock(&mutex);
    return 0;
}

void onJackConnect(jack_port_id_t source, jack_port_id_t dest, int connect, void* args) {
    pthread_mutex_lock(&mutex);
    uint8_t mixbus = *((uint8_t*)args); 
    struct channel_strip** strips = mixbus?g_mixbusStrips:g_channelStrips;
    for (uint8_t chan = 0; chan < MAX_CHANNELS; chan++) {
        if (strips[chan] == NULL)
            continue;
        if (jack_port_connected(strips[chan]->inPortA) > 0 || (jack_port_connected(strips[chan]->inPortB) > 0))
            strips[chan]->inRouted = 1;
        else
            strips[chan]->inRouted = 0;
        if (jack_port_connected(strips[chan]->outPortA) > 0 || (jack_port_connected(strips[chan]->outPortB) > 0))
            strips[chan]->outRouted = 1;
        else
            strips[chan]->outRouted = 0;
    }
    pthread_mutex_unlock(&mutex);
}

int onJackSamplerate(jack_nframes_t nSamplerate, void* arg) {
    if (nSamplerate == 0)
        return 0;
    g_samplerate     = nSamplerate;
    g_nDampingPeriod = g_fDpmDecay * nSamplerate / g_buffersize / 15;
    return 0;
}

int onJackBuffersize(jack_nframes_t nBuffersize, void* arg) {
    if (nBuffersize == 0)
        return 0;
    g_buffersize     = nBuffersize;
    g_nDampingPeriod = g_fDpmDecay * g_samplerate / g_buffersize / 15;
    pthread_mutex_lock(&mutex);
    free(g_mainNoramliseBufferA);
    free(g_mainNoramliseBufferB);
    g_mainNoramliseBufferA = malloc(sizeof(jack_nframes_t) * g_buffersize);
    g_mainNoramliseBufferB = malloc(sizeof(jack_nframes_t) * g_buffersize);

    for (uint8_t chan = 0; chan < MAX_CHANNELS; ++chan) {
        if (g_fxSends[chan]) {
            g_fxSends[chan]->bufferA = jack_port_get_buffer(g_fxSends[chan]->outPortA, g_buffersize);
            g_fxSends[chan]->bufferB = jack_port_get_buffer(g_fxSends[chan]->outPortB, g_buffersize);
        }
    }
    pthread_mutex_unlock(&mutex);
    return 0;
}

int init() {
    fprintf(stderr, "zynmixer starting\n");

    for (uint8_t chan = 0; chan < MAX_CHANNELS; ++chan) {
        g_channelStrips[chan] = NULL;
        g_mixbusStrips[chan] = NULL;
        g_fxSends[chan] = NULL;
    }

    // Initialsize OSC
    g_oscfd = socket(AF_INET, SOCK_DGRAM, 0);
    for (uint8_t i = 0; i < MAX_OSC_CLIENTS; ++i) {
        memset(g_oscClient[i].sin_zero, '\0', sizeof g_oscClient[i].sin_zero);
        g_oscClient[i].sin_family      = AF_INET;
        g_oscClient[i].sin_port        = htons(1370);
        g_oscClient[i].sin_addr.s_addr = 0;
    }

    // Register with Jack server
    char* sServerName = NULL;
    jack_status_t nStatus;
    jack_options_t nOptions = JackNoStartServer;

    if ((g_chanJackClient = jack_client_open("zynmixer_chans", nOptions, &nStatus, sServerName)) == 0) {
        fprintf(stderr, "libzynmixer: Failed to start channel jack client: %d\n", nStatus);
        exit(1);
    }
#ifdef DEBUG
    fprintf(stderr, "libzynmixer: Registering as '%s'.\n", jack_get_client_name(g_pJackClient));
#endif

    if ((g_mixbusJackClient = jack_client_open("zynmixer_buses", nOptions, &nStatus, sServerName)) == 0) {
        fprintf(stderr, "libzynmixer: Failed to start mixbus jack client: %d\n", nStatus);
        exit(1);
    }
#ifdef DEBUG
    fprintf(stderr, "libzynmixer: Registering as '%s'.\n", jack_get_client_name(g_pJackClient));
#endif

    // Create main mixbus channel strip
    addStrip(1);
    g_mainNoramliseBufferA = malloc(sizeof(jack_nframes_t) * g_buffersize);
    g_mainNoramliseBufferB = malloc(sizeof(jack_nframes_t) * g_buffersize);

    // Temporarily create a port on chanstips client to work around bug in jack
    jack_port_t* tmp_port = jack_port_register(g_chanJackClient, "tmp", JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput, 0);

#ifdef DEBUG
    fprintf(stderr, "libzynmixer: Created channel strips\n");
#endif

    // Register the cleanup function to be called when library exits
    atexit(end);

    // Register the callbacks
    jack_set_process_callback(g_chanJackClient, onJackProcess, &CHANNEL);
    jack_set_port_connect_callback(g_chanJackClient, onJackConnect, &CHANNEL);
    jack_set_sample_rate_callback(g_chanJackClient, onJackSamplerate, &CHANNEL);
    jack_set_buffer_size_callback(g_chanJackClient, onJackBuffersize, &CHANNEL);

    jack_set_process_callback(g_mixbusJackClient, onJackProcess, &MIXBUS);
    jack_set_port_connect_callback(g_mixbusJackClient, onJackConnect, &MIXBUS);
    jack_set_sample_rate_callback(g_mixbusJackClient, onJackSamplerate, &MIXBUS);
    jack_set_buffer_size_callback(g_mixbusJackClient, onJackBuffersize, &MIXBUS);

    if (jack_activate(g_chanJackClient)) {
        fprintf(stderr, "libzynmixer: Cannot activate client\n");
        exit(1);
    }
    if (jack_activate(g_mixbusJackClient)) {
        fprintf(stderr, "libzynmixer: Cannot activate client\n");
        exit(1);
    }

    // Remove tmp port
    jack_port_unregister(g_chanJackClient, tmp_port);

#ifdef DEBUG
    fprintf(stderr, "libzynmixer: Activated client\n");
#endif

    // Configure and start event thread
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);
    if (pthread_create(&g_eventThread, &attr, eventThreadFn, NULL)) {
        fprintf(stderr, "zynmixer error: failed to create event thread\n");
        return 0;
    }

    fprintf(stderr, "Started libzynmixer\n");

    return 1;
}

void end() {
    g_sendEvents = 0;
    void* status;
    pthread_join(g_eventThread, &status);

    //Soft mute output
    setLevel(0, 1, 0.0);
    usleep(100000);

    // Close links with jack server
    if (g_mixbusJackClient) {
        jack_deactivate(g_mixbusJackClient);
        jack_client_close(g_mixbusJackClient);
    }
    if (g_chanJackClient) {
        jack_deactivate(g_chanJackClient);
        jack_client_close(g_chanJackClient);
    }

    // Release dynamically created resources
    free(g_mainNoramliseBufferA);
    free(g_mainNoramliseBufferB);
    for (uint8_t chan = 0; chan < MAX_CHANNELS; ++chan) {
        free(g_channelStrips[chan]);
        free(g_mixbusStrips[chan]);
        free(g_fxSends[chan]);
    }
    fprintf(stderr, "zynmixer ended\n");
}

void setLevel(uint8_t channel, uint8_t mixbus, float level) {
    struct channel_strip** strips = mixbus?g_mixbusStrips:g_channelStrips;
    if (channel >= MAX_CHANNELS || strips[channel] == NULL)
        return;
    strips[channel]->reqlevel = level;
    if (mixbus)
        sprintf(g_oscpath, "/mixer/mixbus/%d/fader", channel);
    else
        sprintf(g_oscpath, "/mixer/channel/%d/fader", channel);
    sendOscFloat(g_oscpath, level);
}

float getLevel(uint8_t channel, uint8_t mixbus) {
    struct channel_strip** strips = mixbus?g_mixbusStrips:g_channelStrips;
    if (channel >= MAX_CHANNELS || strips[channel] == NULL)
        return 0.0f;
    return strips[channel]->reqlevel;
}

void setBalance(uint8_t channel, uint8_t mixbus, float balance) {
    struct channel_strip** strips = mixbus?g_mixbusStrips:g_channelStrips;
    if (channel >= MAX_CHANNELS || strips[channel] == NULL)
        return;
    if (fabs(balance) > 1)
        return;
    strips[channel]->reqbalance = balance;
    if (mixbus)
        sprintf(g_oscpath, "/mixer/mixbus/%d/balance", channel);
    else
        sprintf(g_oscpath, "/mixer/channel/%d/balance", channel);
    sendOscFloat(g_oscpath, balance);
}

float getBalance(uint8_t channel, uint8_t mixbus) {
    struct channel_strip** strips = mixbus?g_mixbusStrips:g_channelStrips;
    if (channel >= MAX_CHANNELS || strips[channel] == NULL)
        return 0.0f;
    return strips[channel]->reqbalance;
}

void setMute(uint8_t channel, uint8_t mixbus, uint8_t mute) {
    struct channel_strip** strips = mixbus?g_mixbusStrips:g_channelStrips;
    if (channel >= MAX_CHANNELS || strips[channel] == NULL)
        return;
    strips[channel]->mute = mute;
    if (mixbus)
        sprintf(g_oscpath, "/mixer/mixbus/%d/mute", channel);
    else
        sprintf(g_oscpath, "/mixer/channel/%d/mute", channel);
    sendOscInt(g_oscpath, mute);
}

uint8_t getMute(uint8_t channel, uint8_t mixbus) {
    struct channel_strip** strips = mixbus?g_mixbusStrips:g_channelStrips;
    if (channel >= MAX_CHANNELS || strips[channel] == NULL)
        return 0;
    return strips[channel]->mute;
}

void setPhase(uint8_t channel, uint8_t mixbus, uint8_t phase) {
    struct channel_strip** strips = mixbus?g_mixbusStrips:g_channelStrips;
    if (channel >= MAX_CHANNELS || strips[channel] == NULL)
        return;
    strips[channel]->phase = phase;
    if (mixbus)
        sprintf(g_oscpath, "/mixer/mixbus/%d/phase", channel);
    else
        sprintf(g_oscpath, "/mixer/channel/%d/phase", channel);
    sendOscInt(g_oscpath, phase);
}

uint8_t getPhase(uint8_t channel, uint8_t mixbus) {
    struct channel_strip** strips = mixbus?g_mixbusStrips:g_channelStrips;
    if (channel >= MAX_CHANNELS || strips[channel] == NULL)
        return 0;
    return strips[channel]->phase;
}

void setSend(uint8_t channel, uint8_t send, float level) {
    if (channel >= MAX_CHANNELS || g_channelStrips[channel] == NULL || send >= MAX_CHANNELS)
        return;
    g_channelStrips[channel]->send[send] = level;
    sprintf(g_oscpath, "/mixer/channel/%d/send_%d", channel, send);
    sendOscFloat(g_oscpath, level);
}

float getSend(uint8_t channel, uint8_t send) {
    if (channel >= MAX_CHANNELS || g_channelStrips[channel] == NULL || send >= MAX_CHANNELS)
        return 0.0f;
    return g_channelStrips[channel]->send[send];
}

void setNormalise(uint8_t channel, uint8_t enable) {
    if (channel == 0 || channel >= MAX_CHANNELS || g_mixbusStrips[channel] == NULL)
        return;
    g_mixbusStrips[channel]->normalise = enable;
    sprintf(g_oscpath, "/mixer/mixbus/%d/normalise", channel);
    sendOscInt(g_oscpath, enable);
}

uint8_t getNormalise(uint8_t channel) {
    if (channel >= MAX_CHANNELS || g_mixbusStrips[channel] == NULL)
        return 0;
    return g_mixbusStrips[channel]->normalise;
}

void setSolo(uint8_t channel, uint8_t mixbus, uint8_t solo) {
    struct channel_strip** strips = mixbus?g_mixbusStrips:g_channelStrips;
    if (channel >= MAX_CHANNELS || strips[channel] == NULL)
        return;
    if (mixbus && !channel) {
        // Setting main mixbus solo will disable all channel solos
        for (uint8_t chan = 0; chan < MAX_CHANNELS; ++chan) {
            if(g_channelStrips[chan]) {
                g_channelStrips[chan]->solo = 0;
                sprintf(g_oscpath, "/mixer/channel/%d/solo", chan);
                sendOscInt(g_oscpath, 0);
            }
            if(g_mixbusStrips[chan]) {
                g_mixbusStrips[chan]->solo = 0;
                sprintf(g_oscpath, "/mixer/mixbus/%d/solo", chan);
                sendOscInt(g_oscpath, 0);
            }
        }
    } else {
        strips[channel]->solo = solo;
        sprintf(g_oscpath, "/mixer/channel/%d/solo", channel);
        sendOscInt(g_oscpath, solo);
    }
    // Set the global solo flag if any channel solo is enabled
    g_solo = 0;
    for (uint8_t chan = 0; chan < MAX_CHANNELS; ++chan) {
        if (g_channelStrips[chan] && g_channelStrips[chan]->solo ||g_mixbusStrips[chan] && g_mixbusStrips[chan]->solo) {
            g_solo = 1;
            break;
        }
    }
    if (mixbus)
        sprintf(g_oscpath, "/mixer/mixbus/%d/solo", 0);
    else
        sprintf(g_oscpath, "/mixer/channel/%d/solo", 0);
    sendOscInt(g_oscpath, g_solo);
}

uint8_t getSolo(uint8_t channel, uint8_t mixbus) {
    struct channel_strip** strips = mixbus?g_mixbusStrips:g_channelStrips;
    if (channel >= MAX_CHANNELS || strips[channel] == NULL)
        return 0;
    return strips[channel]->solo;
}

void toggleMute(uint8_t channel, uint8_t mixbus) {
    struct channel_strip** strips = mixbus?g_mixbusStrips:g_channelStrips;
    if (channel >= MAX_CHANNELS || strips[channel] == NULL)
        return;
    uint8_t mute;
    mute = strips[channel]->mute;
    if (mute)
        setMute(channel, mixbus, 0);
    else
        setMute(channel, mixbus, 1);
}

void togglePhase(uint8_t channel, uint8_t mixbus) {
    struct channel_strip** strips = mixbus?g_mixbusStrips:g_channelStrips;
    if (channel >= MAX_CHANNELS || strips[channel] == NULL)
        return;
    uint8_t phase;
    phase = strips[channel]->phase;
    if (phase)
        setPhase(channel, mixbus, 0);
    else
        setPhase(channel, mixbus, 1);
}

void setMono(uint8_t channel, uint8_t mixbus, uint8_t mono) {
    struct channel_strip** strips = mixbus?g_mixbusStrips:g_channelStrips;
    if (channel >= MAX_CHANNELS || strips[channel] == NULL)
        return;
    strips[channel]->mono = (mono != 0);
    if (mixbus)
        sprintf(g_oscpath, "/mixer/mixbus/%d/mono", channel);
    else
        sprintf(g_oscpath, "/mixer/channel/%d/mono", channel);
    sendOscInt(g_oscpath, mono);
}

uint8_t getMono(uint8_t channel, uint8_t mixbus) {
    struct channel_strip** strips = mixbus?g_mixbusStrips:g_channelStrips;
    if (channel >= MAX_CHANNELS || strips[channel] == NULL)
        return 0;
    return strips[channel]->mono;
}

void setMS(uint8_t channel, uint8_t mixbus, uint8_t enable) {
    struct channel_strip** strips = mixbus?g_mixbusStrips:g_channelStrips;
    if (channel >= MAX_CHANNELS || strips[channel] == NULL)
        return;
    strips[channel]->ms = enable != 0;
    if (mixbus)
        sprintf(g_oscpath, "/mixer/mixbus/%d/ms", channel);
    else
        sprintf(g_oscpath, "/mixer/channel/%d/ms", channel);
}

uint8_t getMS(uint8_t channel, uint8_t mixbus) {
    struct channel_strip** strips = mixbus?g_mixbusStrips:g_channelStrips;
    if (channel >= MAX_CHANNELS || strips[channel] == NULL)
        return 0;
    return strips[channel]->ms;
}

void reset(uint8_t channel, uint8_t mixbus) {
    setLevel(channel, mixbus, 0.8);
    setBalance(channel, mixbus, 0.0);
    setMute(channel, mixbus, 0);
    setMono(channel, mixbus, 0);
    setPhase(channel, mixbus, 0);
    setSolo(channel, mixbus, 0);
    if (!mixbus)
        for (uint8_t send = 0; send < MAX_CHANNELS; ++send)
            setSend(channel, send, 0);
}

float getDpm(uint8_t channel, uint8_t mixbus, uint8_t leg) {
    struct channel_strip** strips = mixbus?g_mixbusStrips:g_channelStrips;
    if (channel >= MAX_CHANNELS || strips[channel] == NULL)
        return 0.0f;
    if (leg)
        return convertToDBFS(strips[channel]->dpmB);
    return convertToDBFS(strips[channel]->dpmA);
}

float getDpmHold(uint8_t channel, uint8_t mixbus, uint8_t leg) {
    struct channel_strip** strips = mixbus?g_mixbusStrips:g_channelStrips;
    if (channel >= MAX_CHANNELS || strips[channel] == NULL)
        return 0.0f;
    if (leg)
        return convertToDBFS(strips[channel]->holdB);
    return convertToDBFS(strips[channel]->holdA);
}

void getDpmStates(uint8_t start, uint8_t end, uint8_t mixbus, float* values) {
    if (start > end) {
        uint8_t tmp = start;
        start       = end;
        end         = tmp;
    }
    uint8_t count = end - start + 1;
    while (count--) {
        *(values++) = getDpm(start, mixbus, 0);
        *(values++) = getDpm(start, mixbus, 1);
        *(values++) = getDpmHold(start, mixbus, 0);
        *(values++) = getDpmHold(start, mixbus, 1);
        *(values++) = getMono(start, mixbus);
        ++start;
    }
}

void enableDpm(uint8_t start, uint8_t end, uint8_t mixbus, uint8_t enable) {
    fprintf(stderr, "enableDpm %d %d %d %d\n", start, end, mixbus, enable);
    struct channel_strip* pChannel;
    if (start > end) {
        uint8_t tmp = start;
        start       = end;
        end         = tmp;
    }
    if (start >= MAX_CHANNELS)
        start = MAX_CHANNELS - 1;
    if (end >= MAX_CHANNELS)
        end = MAX_CHANNELS - 1;
    struct channel_strip** strips = mixbus?g_mixbusStrips:g_channelStrips;
    for (uint8_t chan = start; chan <= end; ++chan) {
        if (strips[chan] == NULL)
            continue;
        strips[chan]->enable_dpm = enable;
        if (strips[chan] == 0) {
            strips[chan]->dpmA  = 0;
            strips[chan]->dpmB  = 0;
            strips[chan]->holdA = 0;
            strips[chan]->holdB = 0;
        }
    }
}

int addOscClient(const char* client) {
    for (uint8_t i = 0; i < MAX_OSC_CLIENTS; ++i) {
        if (g_oscClient[i].sin_addr.s_addr != 0)
            continue;
        if (inet_pton(AF_INET, client, &(g_oscClient[i].sin_addr)) != 1) {
            g_oscClient[i].sin_addr.s_addr = 0;
            fprintf(stderr, "libzynmixer: Failed to register client %s\n", client);
            return -1;
        }
        fprintf(stderr, "libzynmixer: Added OSC client %d: %s\n", i, client);
        for (uint8_t mixbus = 0; mixbus < 2; ++mixbus) {
            struct channel_strip** strips = mixbus?g_mixbusStrips:g_channelStrips;
            for (int chan = 0; chan < MAX_CHANNELS; ++chan) {
                setBalance(chan, mixbus, getBalance(chan, mixbus));
                setLevel(chan, mixbus, getLevel(chan, mixbus));
                setMono(chan, mixbus, getMono(chan, mixbus));
                setMute(chan, mixbus, getMute(chan, mixbus));
                setPhase(chan, mixbus, getPhase(chan, mixbus));
                setSolo(chan, mixbus, getSolo(chan, mixbus));
                strips[chan]->dpmAlast  = 100.0;
                strips[chan]->dpmBlast  = 100.0;
                strips[chan]->holdAlast = 100.0;
                strips[chan]->holdBlast = 100.0;
            }
        }
        g_bOsc = 1;
        return i;
    }
    fprintf(stderr, "libzynmixer: Not adding OSC client %s - Maximum client count reached [%d]\n", client, MAX_OSC_CLIENTS);
    return -1;
}

void removeOscClient(const char* client) {
    char pClient[sizeof(struct in_addr)];
    if (inet_pton(AF_INET, client, pClient) != 1)
        return;
    g_bOsc = 0;
    for (uint8_t i = 0; i < MAX_OSC_CLIENTS; ++i) {
        if (memcmp(pClient, &g_oscClient[i].sin_addr.s_addr, 4) == 0) {
            g_oscClient[i].sin_addr.s_addr = 0;
            fprintf(stderr, "libzynmixer: Removed OSC client %d: %s\n", i, client);
        }
        if (g_oscClient[i].sin_addr.s_addr != 0)
            g_bOsc = 1;
    }
}

int8_t addStrip(uint8_t mixbus) {
    uint8_t chan;
    struct channel_strip** strips = mixbus?g_mixbusStrips:g_channelStrips;
    jack_client_t* client = mixbus?g_mixbusJackClient:g_chanJackClient;
    for (chan = 0; chan < MAX_CHANNELS; ++chan) {
        if (strips[chan])
            continue;
        strips[chan] = malloc(sizeof(struct channel_strip));
        strips[chan]->level      = 0.0;
        strips[chan]->reqlevel   = 0.8;
        strips[chan]->balance    = 0.0;
        strips[chan]->reqbalance = 0.0;
        strips[chan]->mute       = 0;
        strips[chan]->solo       = 0;
        strips[chan]->mono       = 0;
        strips[chan]->ms         = 0;
        strips[chan]->phase      = 0;
        strips[chan]->normalise  = 0;
        strips[chan]->inRouted   = 0;
        strips[chan]->outRouted  = 0;
        strips[chan]->enable_dpm = 0;
        for (uint8_t send = 0; send < MAX_CHANNELS; ++send)
            strips[chan]->send[send] = 0.0;
        strips[chan]->enable_dpm = 1;
        strips[chan]->normalise  = 0;
        char name[11];
        sprintf(name, "input_%02da", chan);
        pthread_mutex_lock(&mutex);
        if (!(strips[chan]->inPortA = jack_port_register(client, name, JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput, 0))) {
            fprintf(stderr, "libzynmixer: Cannot register %s\n", name);
            free(strips[chan]);
            pthread_mutex_unlock(&mutex);
            return -1;
        }
        sprintf(name, "input_%02db", chan);
        if (!(strips[chan]->inPortB = jack_port_register(client, name, JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput, 0))) {
            fprintf(stderr, "libzynmixer: Cannot register %s\n", name);
            jack_port_unregister(client, strips[chan]->inPortA);
            free(strips[chan]);
            strips[chan] = NULL;
            pthread_mutex_unlock(&mutex);
            return -1;
        }
        sprintf(name, "output_%02da", chan);
        if (!(strips[chan]->outPortA = jack_port_register(client, name, JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0))) {
            fprintf(stderr, "libzynmixer: Cannot register %s\n", name);
            jack_port_unregister(client, strips[chan]->inPortA);
            jack_port_unregister(client, strips[chan]->inPortB);
            free(strips[chan]);
            strips[chan] = NULL;
            pthread_mutex_unlock(&mutex);
            return -1;
        }
        sprintf(name, "output_%02db", chan);
        if (!(strips[chan]->outPortB = jack_port_register(client, name, JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0))) {
            fprintf(stderr, "libzynmixer: Cannot register %s\n", name);
            jack_port_unregister(client, strips[chan]->inPortA);
            jack_port_unregister(client, strips[chan]->inPortB);
            jack_port_unregister(client, strips[chan]->outPortA);
            free(strips[chan]);
            strips[chan] = NULL;
            pthread_mutex_unlock(&mutex);
            return -1;
        }
        if (chan && mixbus) {
            g_fxSends[chan] = malloc(sizeof(struct fx_send));
            sprintf(name, "send_%02da", chan);
            g_fxSends[chan]->outPortA = jack_port_register(g_chanJackClient, name, JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);
            sprintf(name, "send_%02db", chan);
            g_fxSends[chan]->outPortB = jack_port_register(g_chanJackClient, name, JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);
            g_fxSends[chan]->bufferA = jack_port_get_buffer(g_fxSends[chan]->outPortA, g_buffersize);
            g_fxSends[chan]->bufferB = jack_port_get_buffer(g_fxSends[chan]->outPortB, g_buffersize);
            g_fxSends[chan]->level = 1.0;
        }
        strips[chan]->dpmAlast  = 100.0;
        strips[chan]->dpmBlast  = 100.0;
        strips[chan]->holdAlast = 100.0;
        strips[chan]->holdBlast = 100.0;
        pthread_mutex_unlock(&mutex);
        return chan;
    }
    return -1;
}

int8_t removeStrip(uint8_t chan, uint8_t mixbus) {
    struct channel_strip** strips = mixbus?g_mixbusStrips:g_channelStrips;
    jack_client_t* client = mixbus?g_mixbusJackClient:g_chanJackClient;

    if (mixbus && chan == 0 || chan >= MAX_CHANNELS || strips[chan] == NULL)
        return -1;

    if (g_fxSends[chan]){
        jack_port_unregister(g_chanJackClient, g_fxSends[chan]->outPortA);
        jack_port_unregister(g_chanJackClient, g_fxSends[chan]->outPortB);
        free(g_fxSends[chan]);
        g_fxSends[chan] = NULL;
    }
    jack_port_unregister(client, strips[chan]->inPortA);
    jack_port_unregister(client, strips[chan]->inPortB);
    jack_port_unregister(client, strips[chan]->outPortA);
    jack_port_unregister(client, strips[chan]->outPortB);
    free(strips[chan]);
    strips[chan] = NULL;

    return chan;
}

uint8_t getMaxChannels() { return MAX_CHANNELS; }
