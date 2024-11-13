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
static uint8_t GROUP = 1;

pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
char g_oscbuffer[1024];  // Used to send OSC messages
char g_oscpath[64];      //!@todo Ensure path length is sufficient for all paths, e.g. /mixer/channels/xx/fader
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
    uint8_t normalise;     // 1 if channel normalised to main output (when output not routed)
    uint8_t group;         // 1 if strip is a group
    uint8_t inRouted;      // 1 if source routed to channel
    uint8_t outRouted;     // 1 if output routed
    uint8_t enable_dpm;    // 1 to enable calculation of peak meter
};

struct fx_send {
    jack_port_t* outPortA; // Jack output port A
    jack_port_t* outPortB; // Jack output port B
    float level;           // Current fader level 0..1
    jack_default_audio_sample_t* bufferA; // Holds audio samples
    jack_default_audio_sample_t* bufferB; // Holds audio samples
};

jack_client_t* g_chanJackClient;
jack_client_t* g_grpJackClient;
struct channel_strip* g_channelStrips[MAX_CHANNELS];
struct fx_send* g_fxSends[MAX_CHANNELS];
unsigned int g_nDampingCount  = 0;
unsigned int g_nDampingPeriod = 10; // Quantity of cycles between applying DPM damping decay
unsigned int g_nHoldCount     = 0;
float g_fDpmDecay             = 0.9;             // Factor to scale for DPM decay - defines resolution of DPM decay
struct sockaddr_in g_oscClient[MAX_OSC_CLIENTS]; // Array of registered OSC clients
char g_oscdpm[20];
jack_nframes_t g_samplerate                     = 48000; // Jack samplerate used to calculate damping factor
jack_nframes_t g_buffersize                     = 1024;  // Jack buffer size used to calculate damping factor
jack_default_audio_sample_t* pNormalisedBufferA = NULL;  // Pointer to buffer for normalised audio
jack_default_audio_sample_t* pNormalisedBufferB = NULL;  // Pointer to buffer for normalised audio

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
                if (g_channelStrips[chan] == NULL)
                    continue;
                if ((int)(100000 * g_channelStrips[chan]->dpmAlast) != (int)(100000 * g_channelStrips[chan]->dpmA)) {
                    sprintf(g_oscdpm, "/mixer/channels/%d/dpma", chan);
                    sendOscFloat(g_oscdpm, convertToDBFS(g_channelStrips[chan]->dpmA));
                    g_channelStrips[chan]->dpmAlast = g_channelStrips[chan]->dpmA;
                }
                if ((int)(100000 * g_channelStrips[chan]->dpmBlast) != (int)(100000 * g_channelStrips[chan]->dpmB)) {
                    sprintf(g_oscdpm, "/mixer/channels/%d/dpmb", chan);
                    sendOscFloat(g_oscdpm, convertToDBFS(g_channelStrips[chan]->dpmB));
                    g_channelStrips[chan]->dpmBlast = g_channelStrips[chan]->dpmB;
                }
                if ((int)(100000 * g_channelStrips[chan]->holdAlast) != (int)(100000 * g_channelStrips[chan]->holdA)) {
                    sprintf(g_oscdpm, "/mixer/channels/%d/holda", chan);
                    sendOscFloat(g_oscdpm, convertToDBFS(g_channelStrips[chan]->holdA));
                    g_channelStrips[chan]->holdAlast = g_channelStrips[chan]->holdA;
                }
                if ((int)(100000 * g_channelStrips[chan]->holdBlast) != (int)(100000 * g_channelStrips[chan]->holdB)) {
                    sprintf(g_oscdpm, "/mixer/channels/%d/holdb", chan);
                    sendOscFloat(g_oscdpm, convertToDBFS(g_channelStrips[chan]->holdB));
                    g_channelStrips[chan]->holdBlast = g_channelStrips[chan]->holdB;
                }
            }
        }
        usleep(10000);
    }
}

static int onJackProcess(jack_nframes_t nFrames, void* args) {
    jack_default_audio_sample_t *pInA, *pInB, *pOutA, *pOutB, *pChanOutA, *pChanOutB;
    unsigned int frame, chan;
    float curLevelA, curLevelB, reqLevelA, reqLevelB, fDeltaA, fDeltaB, fSampleA, fSampleB, fSampleM, fSendA[MAX_CHANNELS], fSendB[MAX_CHANNELS];
    uint8_t grp = *((uint8_t*)args); 

    pthread_mutex_lock(&mutex);

    // Clear the normalisation buffer. This will be populated by each channel then used in final channel iteration
    memset(pNormalisedBufferA, 0.0, nFrames * sizeof(jack_default_audio_sample_t));
    memset(pNormalisedBufferB, 0.0, nFrames * sizeof(jack_default_audio_sample_t));

    // Clear send buffers.
    for (uint8_t send = 0; send < MAX_CHANNELS; ++send) {
        if (g_fxSends[send]) {
            memset(g_fxSends[send]->bufferA, 0.0, nFrames * sizeof(jack_default_audio_sample_t));
            memset(g_fxSends[send]->bufferB, 0.0, nFrames * sizeof(jack_default_audio_sample_t));
        }
    }

    // Process each channel
    for (chan = 0; chan < MAX_CHANNELS; chan++) {
        if (g_channelStrips[chan] == NULL)
            continue;

        if (g_channelStrips[chan]->inRouted || chan == 0) {
            // Calculate processing levels

            // Calculate current (last set) balance
            if (g_channelStrips[chan]->balance > 0.0)
                curLevelA = g_channelStrips[chan]->level * (1 - g_channelStrips[chan]->balance);
            else
                curLevelA = g_channelStrips[chan]->level;
            if (g_channelStrips[chan]->balance < 0.0)
                curLevelB = g_channelStrips[chan]->level * (1 + g_channelStrips[chan]->balance);
            else
                curLevelB = g_channelStrips[chan]->level;

            // Calculate mute and target level and balance (that we will fade to over this cycle period to avoid abrupt change clicks)
            if (g_channelStrips[chan]->mute || g_solo && (chan) && g_channelStrips[chan]->solo != 1) {
                // Do not mute aux if solo enabled
                g_channelStrips[chan]->level = 0; // We can set this here because we have the data and will iterate towards 0 over this frame
                reqLevelA             = 0.0;
                reqLevelB             = 0.0;
            } else {
                if (g_channelStrips[chan]->reqbalance > 0.0)
                    reqLevelA = g_channelStrips[chan]->reqlevel * (1 - g_channelStrips[chan]->reqbalance);
                else
                    reqLevelA = g_channelStrips[chan]->reqlevel;
                if (g_channelStrips[chan]->reqbalance < 0.0)
                    reqLevelB = g_channelStrips[chan]->reqlevel * (1 + g_channelStrips[chan]->reqbalance);
                else
                    reqLevelB = g_channelStrips[chan]->reqlevel;
                g_channelStrips[chan]->level   = g_channelStrips[chan]->reqlevel;
                g_channelStrips[chan]->balance = g_channelStrips[chan]->reqbalance;
            }

            // Calculate the step change for each leg to apply on each sample in buffer for fade between last and this period's level
            fDeltaA = (reqLevelA - curLevelA) / nFrames;
            fDeltaB = (reqLevelB - curLevelB) / nFrames;

            // **Apply processing to audio samples**

            pInA = jack_port_get_buffer(g_channelStrips[chan]->inPortA, nFrames);
            pInB = jack_port_get_buffer(g_channelStrips[chan]->inPortB, nFrames);

            if (g_channelStrips[chan]->outRouted) {
                // Direct output so create audio buffers
                pChanOutA = jack_port_get_buffer(g_channelStrips[chan]->outPortA, nFrames);
                pChanOutB = jack_port_get_buffer(g_channelStrips[chan]->outPortB, nFrames);
                memset(pChanOutA, 0.0, nFrames * sizeof(jack_default_audio_sample_t));
                memset(pChanOutB, 0.0, nFrames * sizeof(jack_default_audio_sample_t));
            } else {
                pChanOutA = pChanOutB = NULL;
            }

            // Iterate samples, scaling each and adding to output and set DPM if any samples louder than current DPM
            for (frame = 0; frame < nFrames; frame++) {
                if (chan == 0) {
                    // Mix channel input and normalised channels mix
                    fSampleA = (pInA[frame] + pNormalisedBufferA[frame]);
                    fSampleB = (pInB[frame] + pNormalisedBufferB[frame]);
                } else {
                    fSampleA = pInA[frame];
                    fSampleB = pInB[frame];
                }
                // Handle channel phase reverse
                if (g_channelStrips[chan]->phase)
                    fSampleB = -fSampleB;

                // Decode M+S
                if (g_channelStrips[chan]->ms) {
                    fSampleM = fSampleA + fSampleB;
                    fSampleB = fSampleA - fSampleB;
                    fSampleA = fSampleM;
                }

                // Handle mono
                if (g_channelStrips[chan]->mono) {
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
                for (uint8_t send = 0; send < MAX_CHANNELS; ++send) {
                    if (g_fxSends[send]) {
                        g_fxSends[send]->bufferA[frame] += fSampleA * g_channelStrips[chan]->send[send];
                        g_fxSends[send]->bufferB[frame] += fSampleB * g_channelStrips[chan]->send[send];
                        if(isinf(g_fxSends[send]->bufferA[frame]))
                            g_fxSends[send]->bufferA[frame] = 1.0;
                        if(isinf(g_fxSends[send]->bufferB[frame]))
                            g_fxSends[send]->bufferB[frame] = 1.0;
                    }
                }

                // Write normalised samples
                if (g_channelStrips[chan]->normalise) {
                    pNormalisedBufferA[frame] += fSampleA;
                    pNormalisedBufferB[frame] += fSampleB;
                }

                curLevelA += fDeltaA;
                curLevelB += fDeltaB;

                // Process DPM
                if (g_channelStrips[chan]->enable_dpm) {
                    fSampleA = fabs(fSampleA);
                    if (fSampleA > g_channelStrips[chan]->dpmA)
                        g_channelStrips[chan]->dpmA = fSampleA;
                    fSampleB = fabs(fSampleB);
                    if (fSampleB > g_channelStrips[chan]->dpmB)
                        g_channelStrips[chan]->dpmB = fSampleB;

                    // Update peak hold and scale DPM for damped release
                    if (g_channelStrips[chan]->dpmA > g_channelStrips[chan]->holdA)
                        g_channelStrips[chan]->holdA = g_channelStrips[chan]->dpmA;
                    if (g_channelStrips[chan]->dpmB > g_channelStrips[chan]->holdB)
                        g_channelStrips[chan]->holdB = g_channelStrips[chan]->dpmB;
                }
            }
            if (g_nHoldCount == 0) {
                // Only update peak hold each g_nHoldCount cycles
                g_channelStrips[chan]->holdA = g_channelStrips[chan]->dpmA;
                g_channelStrips[chan]->holdB = g_channelStrips[chan]->dpmB;
            }
            if (g_nDampingCount == 0) {
                // Only update damping release each g_nDampingCount cycles
                g_channelStrips[chan]->dpmA *= g_fDpmDecay;
                g_channelStrips[chan]->dpmB *= g_fDpmDecay;
            }
        } else if (g_channelStrips[chan]->enable_dpm) {
            g_channelStrips[chan]->dpmA  = -200.0;
            g_channelStrips[chan]->dpmB  = -200.0;
            g_channelStrips[chan]->holdA = -200.0;
            g_channelStrips[chan]->holdB = -200.0;
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
    for (uint8_t chan = 0; chan < MAX_CHANNELS; chan++) {
        if (g_channelStrips[chan] == NULL)
            continue;
        if (jack_port_connected(g_channelStrips[chan]->inPortA) > 0 || (jack_port_connected(g_channelStrips[chan]->inPortB) > 0))
            g_channelStrips[chan]->inRouted = 1;
        else
            g_channelStrips[chan]->inRouted = 0;
        if (jack_port_connected(g_channelStrips[chan]->outPortA) > 0 || (jack_port_connected(g_channelStrips[chan]->outPortB) > 0))
            g_channelStrips[chan]->outRouted = 1;
        else
            g_channelStrips[chan]->outRouted = 0;
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
    free(pNormalisedBufferA);
    free(pNormalisedBufferB);
    pNormalisedBufferA = malloc(g_buffersize * sizeof(jack_default_audio_sample_t));
    pNormalisedBufferB = malloc(g_buffersize * sizeof(jack_default_audio_sample_t));
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

    if ((g_grpJackClient = jack_client_open("zynmixer_buses", nOptions, &nStatus, sServerName)) == 0) {
        fprintf(stderr, "libzynmixer: Failed to start group jack client: %d\n", nStatus);
        exit(1);
    }
#ifdef DEBUG
    fprintf(stderr, "libzynmixer: Registering as '%s'.\n", jack_get_client_name(g_pJackClient));
#endif

    // Create main mixbus channel strip
    addStrip(1);

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

    jack_set_process_callback(g_grpJackClient, onJackProcess, &GROUP);
    jack_set_port_connect_callback(g_grpJackClient, onJackConnect, &GROUP);
    jack_set_sample_rate_callback(g_grpJackClient, onJackSamplerate, &GROUP);
    jack_set_buffer_size_callback(g_grpJackClient, onJackBuffersize, &GROUP);

    if (jack_activate(g_chanJackClient)) {
        fprintf(stderr, "libzynmixer: Cannot activate client\n");
        exit(1);
    }
    if (jack_activate(g_grpJackClient)) {
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
    setLevel(0, 0.0);
    usleep(100000);

    // Close links with jack server
    if (g_grpJackClient) {
        jack_deactivate(g_grpJackClient);
        jack_client_close(g_grpJackClient);
    }
    if (g_chanJackClient) {
        jack_deactivate(g_chanJackClient);
        jack_client_close(g_chanJackClient);
    }

    // Release dynamically created resources
    free(pNormalisedBufferA);
    free(pNormalisedBufferB);
    for (uint8_t chan = 0; chan < MAX_CHANNELS; ++chan) {
        free(g_channelStrips[chan]);
        free(g_fxSends[chan]);
    }
    fprintf(stderr, "zynmixer ended\n");
}

void setLevel(uint8_t channel, float level) {
    if (channel >= MAX_CHANNELS || g_channelStrips[channel] == NULL)
        return;
    g_channelStrips[channel]->reqlevel = level;
    sprintf(g_oscpath, "/mixer/channels/%d/fader", channel);
    sendOscFloat(g_oscpath, level);
}

float getLevel(uint8_t channel) {
    if (channel >= MAX_CHANNELS || g_channelStrips[channel] == NULL)
        return 0.0f;
    return g_channelStrips[channel]->reqlevel;
}

void setBalance(uint8_t channel, float balance) {
    if (channel >= MAX_CHANNELS || g_channelStrips[channel] == NULL)
        return;
    if (fabs(balance) > 1)
        return;
    g_channelStrips[channel]->reqbalance = balance;
    sprintf(g_oscpath, "/mixer/channels/%d/balance", channel);
    sendOscFloat(g_oscpath, balance);
}

float getBalance(uint8_t channel) {
    if (channel >= MAX_CHANNELS || g_channelStrips[channel] == NULL)
        return 0.0f;
    return g_channelStrips[channel]->reqbalance;
}

void setMute(uint8_t channel, uint8_t mute) {
    if (channel >= MAX_CHANNELS || g_channelStrips[channel] == NULL)
        return;
    g_channelStrips[channel]->mute = mute;
    sprintf(g_oscpath, "/mixer/channels/%d/mute", channel);
    sendOscInt(g_oscpath, mute);
}

uint8_t getMute(uint8_t channel) {
    if (channel >= MAX_CHANNELS || g_channelStrips[channel] == NULL)
        return 0;
    return g_channelStrips[channel]->mute;
}

void setPhase(uint8_t channel, uint8_t phase) {
    if (channel >= MAX_CHANNELS || g_channelStrips[channel] == NULL)
        return;
    g_channelStrips[channel]->phase = phase;
    sprintf(g_oscpath, "/mixer/channels/%d/phase", channel);
    sendOscInt(g_oscpath, phase);
}

uint8_t getPhase(uint8_t channel) {
    if (channel >= MAX_CHANNELS || g_channelStrips[channel] == NULL)
        return 0;
    return g_channelStrips[channel]->phase;
}

void setSend(uint8_t channel, uint8_t send, float level) {
    if (channel >= MAX_CHANNELS || g_channelStrips[channel] == NULL || send >= MAX_CHANNELS)
        return;
    g_channelStrips[channel]->send[send] = level;
    sprintf(g_oscpath, "/mixer/channels/%d/send_%d", channel, send);
    sendOscFloat(g_oscpath, level);
}

float getSend(uint8_t channel, uint8_t send) {
    if (channel >= MAX_CHANNELS || g_channelStrips[channel] == NULL || send >= MAX_CHANNELS)
        return 0.0f;
    return g_channelStrips[channel]->send[send];
}

void setNormalise(uint8_t channel, uint8_t enable) {
    if (channel == 0 || channel >= MAX_CHANNELS || g_channelStrips[channel] == NULL)
        return;
    g_channelStrips[channel]->normalise = enable;
    sprintf(g_oscpath, "/mixer/channels/%d/normalise", channel);
    sendOscInt(g_oscpath, enable);
}

uint8_t getNormalise(uint8_t channel, uint8_t enable) {
    if (channel >= MAX_CHANNELS || g_channelStrips[channel] == NULL)
        return 0;
    return g_channelStrips[channel]->normalise;
}

void setSolo(uint8_t channel, uint8_t solo) {
    if (channel >= MAX_CHANNELS || g_channelStrips[channel] == NULL)
        return;
    if (channel == 0) {
        // Setting main mixbus solo will disable all channel solos
        for (uint8_t chan = 1; chan < MAX_CHANNELS; ++chan) {
            if (g_channelStrips[chan]) {
                g_channelStrips[chan]->solo = 0;
                sprintf(g_oscpath, "/mixer/channels/%d/solo", chan);
                sendOscInt(g_oscpath, 0);
            }
        }
    } else {
        g_channelStrips[channel]->solo = solo;
        sprintf(g_oscpath, "/mixer/channels/%d/solo", channel);
        sendOscInt(g_oscpath, solo);
    }
    // Set the global solo flag if any channel solo is enabled
    g_solo = 0;
    for (uint8_t chan = 1; chan < MAX_CHANNELS; ++chan)
        if (g_channelStrips[chan])
            g_solo |= g_channelStrips[chan]->solo;
    sprintf(g_oscpath, "/mixer/channels/%d/solo", 0);
    sendOscInt(g_oscpath, g_solo);
}

uint8_t getSolo(uint8_t channel) {
    if (channel >= MAX_CHANNELS || g_channelStrips[channel] == NULL)
        return 0;
    return g_channelStrips[channel]->solo;
}

void toggleMute(uint8_t channel) {
    if (channel >= MAX_CHANNELS || g_channelStrips[channel] == NULL)
        return;
    uint8_t mute;
    mute = g_channelStrips[channel]->mute;
    if (mute)
        setMute(channel, 0);
    else
        setMute(channel, 1);
}

void togglePhase(uint8_t channel) {
    if (channel >= MAX_CHANNELS || g_channelStrips[channel] == NULL)
        return;
    uint8_t phase;
    phase = g_channelStrips[channel]->phase;
    if (phase)
        setPhase(channel, 0);
    else
        setPhase(channel, 1);
}

void setMono(uint8_t channel, uint8_t mono) {
    if (channel >= MAX_CHANNELS || g_channelStrips[channel] == NULL)
        return;
    g_channelStrips[channel]->mono = (mono != 0);
    sprintf(g_oscpath, "/mixer/channels/%d/mono", channel);
    sendOscInt(g_oscpath, mono);
}

uint8_t getMono(uint8_t channel) {
    if (channel >= MAX_CHANNELS || g_channelStrips[channel] == NULL)
        return 0;
    return g_channelStrips[channel]->mono;
}

void setMS(uint8_t channel, uint8_t enable) {
    if (channel >= MAX_CHANNELS || g_channelStrips[channel] == NULL)
        return;
    g_channelStrips[channel]->ms = enable != 0;
}

uint8_t getMS(uint8_t channel) {
    if (channel >= MAX_CHANNELS || g_channelStrips[channel] == NULL)
        return 0;
    return g_channelStrips[channel]->ms;
}

void reset(uint8_t channel) {
    if (channel >= MAX_CHANNELS || g_channelStrips[channel] == NULL)
        return;
    setLevel(channel, 0.8);
    setBalance(channel, 0.0);
    setMute(channel, 0);
    setMono(channel, 0);
    setPhase(channel, 0);
    setSolo(channel, 0);
    for (uint8_t send = 0; send < MAX_CHANNELS; ++send)
        setSend(channel, send, 0);
}

float getDpm(uint8_t channel, uint8_t leg) {
    if (channel >= MAX_CHANNELS || g_channelStrips[channel] == NULL)
        return 0.0f;
    if (leg)
        return convertToDBFS(g_channelStrips[channel]->dpmB);
    return convertToDBFS(g_channelStrips[channel]->dpmA);
}

float getDpmHold(uint8_t channel, uint8_t leg) {
    if (channel >= MAX_CHANNELS || g_channelStrips[channel] == NULL)
        return 0.0f;
    if (leg)
        return convertToDBFS(g_channelStrips[channel]->holdB);
    return convertToDBFS(g_channelStrips[channel]->holdA);
}

void getDpmStates(uint8_t start, uint8_t end, float* values) {
    if (start > end) {
        uint8_t tmp = start;
        start       = end;
        end         = tmp;
    }
    uint8_t count = end - start + 1;
    while (count--) {
        *(values++) = getDpm(start, 0);
        *(values++) = getDpm(start, 1);
        *(values++) = getDpmHold(start, 0);
        *(values++) = getDpmHold(start, 1);
        *(values++) = getMono(start);
        ++start;
    }
}

void enableDpm(uint8_t start, uint8_t end, uint8_t enable) {
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
    for (uint8_t chan = start; chan <= end; ++chan) {
        if (g_channelStrips[chan] == NULL)
            continue;
        g_channelStrips[chan]->enable_dpm = enable;
        if (g_channelStrips[chan] == 0) {
            g_channelStrips[chan]->dpmA  = 0;
            g_channelStrips[chan]->dpmB  = 0;
            g_channelStrips[chan]->holdA = 0;
            g_channelStrips[chan]->holdB = 0;
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
        for (int chan = 0; chan < MAX_CHANNELS; ++chan) {
            if (g_channelStrips[chan] == NULL)
                continue;
            setBalance(chan, getBalance(chan));
            setLevel(chan, getLevel(chan));
            setMono(chan, getMono(chan));
            setMute(chan, getMute(chan));
            setPhase(chan, getPhase(chan));
            setSolo(chan, getSolo(chan));
            g_channelStrips[chan]->dpmAlast  = 100.0;
            g_channelStrips[chan]->dpmBlast  = 100.0;
            g_channelStrips[chan]->holdAlast = 100.0;
            g_channelStrips[chan]->holdBlast = 100.0;
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

int8_t addStrip(uint8_t grp) {
    uint8_t chan;
    for (chan = 0; chan < MAX_CHANNELS; ++chan) {
        if (g_channelStrips[chan])
            continue;
        g_channelStrips[chan] = malloc(sizeof(struct channel_strip));
        g_channelStrips[chan]->level      = 0.0;
        g_channelStrips[chan]->reqlevel   = 0.8;
        g_channelStrips[chan]->balance    = 0.0;
        g_channelStrips[chan]->reqbalance = 0.0;
        g_channelStrips[chan]->mute       = 0;
        g_channelStrips[chan]->ms         = 0;
        g_channelStrips[chan]->phase      = 0;
        for (uint8_t send = 0; send < MAX_CHANNELS; ++send)
            g_channelStrips[chan]->send[send] = 0.0;
        g_channelStrips[chan]->enable_dpm = 1;
        g_channelStrips[chan]->group  = grp;
        g_channelStrips[chan]->normalise  = 0; //!@todo fix normaisation (chan != 0);
        jack_client_t* client = g_chanJackClient;
        if (grp)
            client = g_grpJackClient;
        char name[11];
        sprintf(name, "input_%02da", chan);
        pthread_mutex_lock(&mutex);
        if (!(g_channelStrips[chan]->inPortA = jack_port_register(client, name, JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput, 0))) {
            fprintf(stderr, "libzynmixer: Cannot register %s\n", name);
            free(g_channelStrips[chan]);
            pthread_mutex_unlock(&mutex);
            return -1;
        }
        sprintf(name, "input_%02db", chan);
        if (!(g_channelStrips[chan]->inPortB = jack_port_register(client, name, JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput, 0))) {
            fprintf(stderr, "libzynmixer: Cannot register %s\n", name);
            jack_port_unregister(client, g_channelStrips[chan]->inPortA);
            free(g_channelStrips[chan]);
            g_channelStrips[chan] = NULL;
            pthread_mutex_unlock(&mutex);
            return -1;
        }
        sprintf(name, "output_%02da", chan);
        if (!(g_channelStrips[chan]->outPortA = jack_port_register(client, name, JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0))) {
            fprintf(stderr, "libzynmixer: Cannot register %s\n", name);
            jack_port_unregister(client, g_channelStrips[chan]->inPortA);
            jack_port_unregister(client, g_channelStrips[chan]->inPortB);
            free(g_channelStrips[chan]);
            g_channelStrips[chan] = NULL;
            pthread_mutex_unlock(&mutex);
            return -1;
        }
        sprintf(name, "output_%02db", chan);
        if (!(g_channelStrips[chan]->outPortB = jack_port_register(client, name, JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0))) {
            fprintf(stderr, "libzynmixer: Cannot register %s\n", name);
            jack_port_unregister(client, g_channelStrips[chan]->inPortA);
            jack_port_unregister(client, g_channelStrips[chan]->inPortB);
            jack_port_unregister(client, g_channelStrips[chan]->outPortA);
            free(g_channelStrips[chan]);
            g_channelStrips[chan] = NULL;
            pthread_mutex_unlock(&mutex);
            return -1;
        }
        if (chan && grp) {
            g_fxSends[chan] = malloc(sizeof(struct fx_send));
            sprintf(name, "send_%02da", chan);
            g_fxSends[chan]->outPortA = jack_port_register(g_chanJackClient, name, JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);
            sprintf(name, "send_%02db", chan);
            g_fxSends[chan]->outPortB = jack_port_register(g_chanJackClient, name, JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);
            g_fxSends[chan]->bufferA = jack_port_get_buffer(g_fxSends[chan]->outPortA, g_buffersize);
            g_fxSends[chan]->bufferB = jack_port_get_buffer(g_fxSends[chan]->outPortB, g_buffersize);
            g_fxSends[chan]->level = 1.0;
        }
        g_channelStrips[chan]->dpmAlast  = 100.0;
        g_channelStrips[chan]->dpmBlast  = 100.0;
        g_channelStrips[chan]->holdAlast = 100.0;
        g_channelStrips[chan]->holdBlast = 100.0;
        pthread_mutex_unlock(&mutex);
        return chan;
    }
    return -1;
}

int8_t removeStrip(uint8_t chan) {
    if (chan == 0 || chan >= MAX_CHANNELS || g_channelStrips[chan] == NULL)
        return -1;

    jack_client_t* client = g_chanJackClient;
    if (g_fxSends[chan]){
        jack_port_unregister(g_chanJackClient, g_fxSends[chan]->outPortA);
        jack_port_unregister(g_chanJackClient, g_fxSends[chan]->outPortB);
        free(g_fxSends[chan]);
        g_fxSends[chan] = NULL;
        client = g_grpJackClient;
    }
    jack_port_unregister(client, g_channelStrips[chan]->inPortA);
    jack_port_unregister(client, g_channelStrips[chan]->inPortB);
    jack_port_unregister(client, g_channelStrips[chan]->outPortA);
    jack_port_unregister(client, g_channelStrips[chan]->outPortB);
    free(g_channelStrips[chan]);
    g_channelStrips[chan] = NULL;

    return chan;
}

int8_t isGroup(uint8_t chan) {
    if (chan > MAX_CHANNELS || g_channelStrips[chan] == NULL)
        return -1;
    return g_channelStrips[chan]->group;
}

uint8_t getMaxChannels() { return MAX_CHANNELS; }
