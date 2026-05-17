/*
 * This file is part of mod-host.
 *
 * mod-host is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * mod-host is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with mod-host.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

/*
************************************************************************************************************************
*           INCLUDE FILES
************************************************************************************************************************
*/

#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <jack/jack.h>

#include "monitor-client.h"
#include "../mod-semaphore.h"
#include "../utils.h"

#ifndef STANDALONE_MONITOR_CLIENT
#include "../effects.h"
#endif

/*
************************************************************************************************************************
*           LOCAL DEFINES
************************************************************************************************************************
*/

// used for local stack variables
#define MAX_CHAR_BUF_SIZE 255

/*
************************************************************************************************************************
*           LOCAL CONSTANTS
************************************************************************************************************************
*/

/*
************************************************************************************************************************
*           LOCAL DATA TYPES
************************************************************************************************************************
*/

typedef struct MONITOR_CLIENT_T {
    jack_client_t *client;
    jack_port_t **in_ports;
    jack_port_t **out_ports;
    sem_t wait_proc_sem;
    sem_t wait_volume_sem;
    uint64_t connected;
    uint32_t numports;
    float volume, smooth_volume, step_volume;
    bool apply_volume, apply_smoothing;
    bool muted;
    atomic_bool wait_proc, wait_volume;
} monitor_client_t;

/*
************************************************************************************************************************
*           LOCAL MACROS
************************************************************************************************************************
*/

#define UNUSED_PARAM(var)           do { (void)(var); } while (0)

/*
************************************************************************************************************************
*           LOCAL GLOBAL VARIABLES
************************************************************************************************************************
*/

static bool g_active = false;
static monitor_client_t* g_monitor_handle = NULL;

/*
************************************************************************************************************************
*           LOCAL FUNCTION PROTOTYPES
************************************************************************************************************************
*/

/*
************************************************************************************************************************
*           LOCAL CONFIGURATION ERRORS
************************************************************************************************************************
*/

/*
************************************************************************************************************************
*           LOCAL FUNCTIONS
************************************************************************************************************************
*/

/*
************************************************************************************************************************
*           GLOBAL FUNCTIONS
************************************************************************************************************************
*/

static inline float db2lin(const float db)
{
    return powf(10.0f, 0.05f * db);
}

static int ProcessMonitor(jack_nframes_t nframes, void *arg)
{
    monitor_client_t *const mon = arg;

   #ifndef STANDALONE_MONITOR_CLIENT
    effect_sync_scheduled_params(true);
   #endif

    if (mon->muted)
    {
        for (uint32_t i=0; i < mon->numports; ++i)
            memset(jack_port_get_buffer(mon->out_ports[i], nframes), 0, sizeof(float)*nframes);

        if (atomic_exchange(&mon->wait_proc, false))
            sem_post(&mon->wait_proc_sem);

        if (atomic_exchange(&mon->wait_volume, false))
            sem_post(&mon->wait_volume_sem);

        return 0;
    }

    const float step_volume = mon->step_volume;
    const float volume = mon->volume;
    float smooth_volume = mon->smooth_volume;

    if (floats_differ_enough(volume, smooth_volume))
    {
        mon->apply_volume = true;
        mon->apply_smoothing = true;
    }

    const float* bufIn[mon->numports];
    /* */ float* bufOut[mon->numports];

    const bool apply_smoothing = mon->apply_smoothing;
    const bool apply_volume = mon->apply_volume;
    const uint64_t connected = mon->connected;

    for (uint32_t i=0; i < mon->numports; ++i)
    {
        bufIn[i] = jack_port_get_buffer(mon->in_ports[i], nframes);
        bufOut[i] = jack_port_get_buffer(mon->out_ports[i], nframes);

        if (connected & (1 << i))
        {
            if (bufOut[i] != bufIn[i])
                memcpy(bufOut[i], bufIn[i], sizeof(float)*nframes);
        }
        else
        {
            memset(bufOut[i], 0, sizeof(float)*nframes);
        }
    }

    if (apply_volume)
    {
        float dy;

        for (jack_nframes_t i=0; i<nframes; ++i)
        {
            if (apply_smoothing)
            {
                dy = volume - smooth_volume;
                smooth_volume += copysignf(fminf(fabsf(dy), step_volume), dy);
            }

            for (uint32_t j=0; j<mon->numports; ++j)
                bufOut[j][i] *= smooth_volume;
        }
    }

    // we use `floats_differ_enough` here as a safe `!=` float value comparison
    mon->apply_volume = floats_differ_enough(smooth_volume, 1.0f);
    mon->smooth_volume = smooth_volume;
    mon->muted = smooth_volume <= db2lin(MOD_MONITOR_VOLUME_MUTE) ||
               ! floats_differ_enough(smooth_volume, db2lin(MOD_MONITOR_VOLUME_MUTE));

    if (atomic_exchange(&mon->wait_proc, false))
        sem_post(&mon->wait_proc_sem);

    // now we manually specify a very low value instead of using `floats_differ_enough`
    // due to smooth filtering the final value is never reached
    if (atomic_load(&mon->wait_volume) && fabsf(volume - smooth_volume) < 0.000001f)
    {
        atomic_store(&mon->wait_volume, false);
        sem_post(&mon->wait_volume_sem);
    }

    return 0;
}

static int GraphOrder(void* arg)
{
    monitor_client_t *const mon = arg;

    for (uint32_t i=0; i<mon->numports; ++i)
    {
        const uint64_t flag = 1 << i;

        if (jack_port_connected(mon->in_ports[i]) > 0)
            mon->connected |= flag;
        else
            mon->connected &= ~flag;
    }

    return 0;
}

#ifdef STANDALONE_MONITOR_CLIENT
__attribute__ ((visibility("default")))
int jack_initialize(jack_client_t* client, const char* load_init);
#else
static
#endif
int jack_initialize(jack_client_t* client, const char* load_init)
{
    /* can only be run once */
    if (g_active)
    {
        fprintf(stderr, "loading 2 instances of monitor client is not allowed\n");
        return 1;
    }

    /* allocate monitor client */
    monitor_client_t *const mon = calloc(1, sizeof(monitor_client_t));

    if (!mon)
    {
        fprintf(stderr, "out of memory\n");
        return 1;
    }

    mon->client = client;
    mon->connected = 0;
    mon->apply_volume = false;
    mon->muted = false;
    mon->wait_volume = false;
    mon->volume = mon->smooth_volume = 1.0f;
    mon->step_volume = 0.f;

    /* Register jack ports */
   #if defined(_DARKGLASS_DEVICE_PABLITO)
    const uint32_t numports = 10;
   #else
    uint32_t numports = 0;

    const char** const sysports = jack_get_ports(client, "system:playback_", JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput);
    if (sysports != NULL)
    {
        while (sysports[numports])
            ++numports;
        jack_free(sysports);
    }

    if (numports == 0)
    {
        fprintf(stderr, "no jack playback ports\n");
        free(mon);
        return 1;
    }
   #endif

    mon->numports = numports;
    mon->in_ports = malloc(sizeof(jack_port_t*) * numports);
    mon->out_ports = malloc(sizeof(jack_port_t*) * numports);

    if (!mon->in_ports || !mon->out_ports)
    {
        fprintf(stderr, "out of memory\n");
        free(mon);
        return 1;
    }

    char portname[MAX_CHAR_BUF_SIZE+1];
    portname[MAX_CHAR_BUF_SIZE] = '\0';

    for (uint32_t i=0; i<numports; ++i)
    {
        snprintf(portname, MAX_CHAR_BUF_SIZE, "in_%d", i + 1);
        mon->in_ports[i] = jack_port_register(client, portname, JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput, 0);

        snprintf(portname, MAX_CHAR_BUF_SIZE, "out_%d", i + 1);
        mon->out_ports[i] = jack_port_register(client, portname, JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);

        if (!mon->in_ports[i] || !mon->out_ports[i])
        {
            fprintf(stderr, "can't register jack ports\n");
            free(mon);
            return 1;
        }

        jack_port_tie(mon->in_ports[i], mon->out_ports[i]);
    }

    atomic_init(&mon->wait_proc, false);
    atomic_init(&mon->wait_volume, false);

    sem_init(&mon->wait_proc_sem, 0, 0);
    sem_init(&mon->wait_volume_sem, 0, 0);

    /* Set jack callbacks */
    jack_set_graph_order_callback(client, GraphOrder, mon);
    jack_set_process_callback(client, ProcessMonitor, mon);

    /* Activate the jack client */
    if (jack_activate(client) != 0)
    {
        fprintf(stderr, "can't activate jack client\n");
        sem_destroy(&mon->wait_proc_sem);
        sem_destroy(&mon->wait_volume_sem);
        free(mon);
        return 1;
    }

    g_active = true;
    g_monitor_handle = mon;

    /* Connect output ports */
    char ourportname[MAX_CHAR_BUF_SIZE+1];
    ourportname[MAX_CHAR_BUF_SIZE] = '\0';

    const char* const ourclientname = jack_get_client_name(client);

   #if defined(_DARKGLASS_DEVICE_PABLITO)
    snprintf(ourportname, MAX_CHAR_BUF_SIZE, "%s:in_9", ourclientname);
    jack_connect(client, "anagram-output:out1", ourportname);

    snprintf(ourportname, MAX_CHAR_BUF_SIZE, "%s:in_10", ourclientname);
    jack_connect(client, "anagram-output:out2", ourportname);

    snprintf(ourportname, MAX_CHAR_BUF_SIZE, "%s:out_5", ourclientname);
    jack_connect(client, ourportname, "system:playback_5");

    snprintf(ourportname, MAX_CHAR_BUF_SIZE, "%s:out_6", ourclientname);
    jack_connect(client, ourportname, "system:playback_6");
   #else
    for (uint32_t i=0; i<numports; ++i)
    {
        snprintf(ourportname, MAX_CHAR_BUF_SIZE, "%s:out_%d", ourclientname, i + 1);
        snprintf(portname, MAX_CHAR_BUF_SIZE, "system:playback_%d", i + 1);
        jack_connect(client, ourportname, portname);
    }
   #endif

    return 0;

    UNUSED_PARAM(load_init);
}

#ifdef STANDALONE_MONITOR_CLIENT
__attribute__ ((visibility("default")))
void jack_finish(void* arg);
#else
static
#endif
void jack_finish(void* arg)
{
    monitor_client_t *const mon = arg;

    jack_deactivate(mon->client);
    sem_destroy(&mon->wait_proc_sem);
    sem_destroy(&mon->wait_volume_sem);

    g_monitor_handle = NULL;
    g_active = false;

    for (uint32_t i=0; i<mon->numports; ++i)
    {
        jack_port_unregister(mon->client, mon->in_ports[i]);
        jack_port_unregister(mon->client, mon->out_ports[i]);
    }

    free(mon->in_ports);
    free(mon->out_ports);
    free(mon);
}

bool monitor_client_init(void)
{
    jack_client_t *const client = jack_client_open("mod-monitor", JackNoStartServer|JackUseExactName, NULL);

    if (!client)
    {
        fprintf(stderr, "failed to open mod-monitor client\n");
        return false;
    }

    if (jack_initialize(client, NULL) != 0)
    {
        jack_client_close(client);
        return false;
    }

    return true;
}

void monitor_client_stop(void)
{
    monitor_client_t *const mon = g_monitor_handle;

    if (!mon)
    {
        fprintf(stderr, "failed to close mod-monitor client\n");
        return;
    }

    jack_client_t *const client = mon->client;

    jack_finish(mon);
    jack_client_close(client);
}

bool monitor_client_setup_volume(float volume)
{
    monitor_client_t *const mon = g_monitor_handle;

    if (!mon)
    {
        fprintf(stderr, "asked to setup volume while monitor client is not active\n");
        return false;
    }

    // local variables for calculations before changing the real struct values
    const float final_volume = db2lin(volume);
    const float step_volume = fabsf(final_volume - mon->smooth_volume)
                            / (MOD_MONITOR_VOLUME_WAIT * jack_get_sample_rate(mon->client));
    const bool apply_volume = floats_differ_enough(final_volume, 1.0f);
    const bool unmute = volume > MOD_MONITOR_VOLUME_MUTE;

    mon->volume = final_volume;
    mon->step_volume = step_volume;
    mon->apply_volume = apply_volume;

    atomic_thread_fence(memory_order_seq_cst);

    if (unmute)
        mon->muted = false;

    return true;
}

bool monitor_client_flush_volume(void)
{
    monitor_client_t *const mon = g_monitor_handle;

    if (!mon)
    {
        fprintf(stderr, "asked to flush volume while monitor client is not active\n");
        return false;
    }

    mon->smooth_volume = mon->volume;
    return true;
}

bool monitor_client_wait_proc(void)
{
    monitor_client_t *const mon = g_monitor_handle;

    if (!mon)
    {
        fprintf(stderr, "asked to wait for proc while monitor client is not active\n");
        return false;
    }

    atomic_store(&mon->wait_proc, true);
    return sem_timedwait_secs(&mon->wait_proc_sem, 1) == 0;
}

bool monitor_client_wait_volume(void)
{
    monitor_client_t *const mon = g_monitor_handle;

    if (!mon)
    {
        fprintf(stderr, "asked to wait for volume while monitor client is not active\n");
        return false;
    }

    atomic_store(&mon->wait_volume, true);
    return sem_timedwait_secs(&mon->wait_volume_sem, 1) == 0;
}
