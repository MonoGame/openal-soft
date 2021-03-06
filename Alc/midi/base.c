
#include "config.h"

#include <stdlib.h>
#include <string.h>
#include <limits.h>

#include "midi/base.h"

#include "alMidi.h"
#include "alMain.h"
#include "alError.h"
#include "alThunk.h"
#include "evtqueue.h"
#include "rwlock.h"
#include "alu.h"


/* Microsecond resolution */
#define TICKS_PER_SECOND (1000000)

/* MIDI events */
#define SYSEX_EVENT  (0xF0)


void InitEvtQueue(EvtQueue *queue)
{
    queue->events = NULL;
    queue->maxsize = 0;
    queue->size = 0;
    queue->pos = 0;
}

void ResetEvtQueue(EvtQueue *queue)
{
    ALsizei i;
    for(i = 0;i < queue->size;i++)
    {
        if(queue->events[i].event == SYSEX_EVENT)
        {
            free(queue->events[i].param.sysex.data);
            queue->events[i].param.sysex.data = NULL;
        }
    }

    free(queue->events);
    queue->events = NULL;
    queue->maxsize = 0;
    queue->size = 0;
    queue->pos = 0;
}

ALenum InsertEvtQueue(EvtQueue *queue, const MidiEvent *evt)
{
    ALsizei pos;

    if(queue->maxsize == queue->size)
    {
        if(queue->pos > 0)
        {
            /* Queue has some stale entries, remove them to make space for more
             * events. */
            for(pos = 0;pos < queue->pos;pos++)
            {
                if(queue->events[pos].event == SYSEX_EVENT)
                {
                    free(queue->events[pos].param.sysex.data);
                    queue->events[pos].param.sysex.data = NULL;
                }
            }
            memmove(&queue->events[0], &queue->events[queue->pos],
                    (queue->size-queue->pos)*sizeof(queue->events[0]));
            queue->size -= queue->pos;
            queue->pos = 0;
        }
        else
        {
            /* Queue is full, double the allocated space. */
            void *temp = NULL;
            ALsizei newsize;

            newsize = (queue->maxsize ? (queue->maxsize<<1) : 16);
            if(newsize > queue->maxsize)
                temp = realloc(queue->events, newsize * sizeof(queue->events[0]));
            if(!temp)
                return AL_OUT_OF_MEMORY;

            queue->events = temp;
            queue->maxsize = newsize;
        }
    }

    pos = queue->pos;
    if(queue->size > 0)
    {
        ALsizei high = queue->size - 1;
        while(pos < high)
        {
            ALsizei mid = pos + (high-pos)/2;
            if(queue->events[mid].time < evt->time)
                pos = mid + 1;
            else
                high = mid;
        }
        while(pos < queue->size && queue->events[pos].time <= evt->time)
            pos++;

        if(pos < queue->size)
            memmove(&queue->events[pos+1], &queue->events[pos],
                    (queue->size-pos)*sizeof(queue->events[0]));
    }

    queue->events[pos] = *evt;
    queue->size++;

    return AL_NO_ERROR;
}


void MidiSynth_Construct(MidiSynth *self, ALCdevice *device)
{
    InitEvtQueue(&self->EventQueue);

    RWLockInit(&self->Lock);

    self->Soundfonts = NULL;
    self->NumSoundfonts = 0;

    self->Gain = 1.0f;
    self->State = AL_INITIAL;

    self->LastEvtTime = 0;
    self->NextEvtTime = UINT64_MAX;
    self->SamplesSinceLast = 0.0;
    self->SamplesToNext = 0.0;

    self->SamplesPerTick = (ALdouble)device->Frequency / TICKS_PER_SECOND;
}

void MidiSynth_Destruct(MidiSynth *self)
{
    ALsizei i;

    for(i = 0;i < self->NumSoundfonts;i++)
        DecrementRef(&self->Soundfonts[i]->ref);
    free(self->Soundfonts);
    self->Soundfonts = NULL;
    self->NumSoundfonts = 0;

    ResetEvtQueue(&self->EventQueue);
}


ALenum MidiSynth_selectSoundfonts(MidiSynth *self, ALCcontext *context, ALsizei count, const ALuint *ids)
{
    ALCdevice *device = context->Device;
    ALsoundfont **sfonts;
    ALsizei i;

    if(self->State != AL_INITIAL && self->State != AL_STOPPED)
        return AL_INVALID_OPERATION;

    sfonts = calloc(1, count * sizeof(sfonts[0]));
    if(!sfonts) return AL_OUT_OF_MEMORY;

    for(i = 0;i < count;i++)
    {
        if(ids[i] == 0)
            sfonts[i] = ALsoundfont_getDefSoundfont(context);
        else if(!(sfonts[i]=LookupSfont(device, ids[i])))
        {
            free(sfonts);
            return AL_INVALID_VALUE;
        }
    }

    for(i = 0;i < count;i++)
        IncrementRef(&sfonts[i]->ref);
    sfonts = ExchangePtr((XchgPtr*)&self->Soundfonts, sfonts);
    count = ExchangeInt(&self->NumSoundfonts, count);

    for(i = 0;i < count;i++)
        DecrementRef(&sfonts[i]->ref);
    free(sfonts);

    return AL_NO_ERROR;
}

extern inline void MidiSynth_setGain(MidiSynth *self, ALfloat gain);
extern inline ALfloat MidiSynth_getGain(const MidiSynth *self);
extern inline void MidiSynth_setState(MidiSynth *self, ALenum state);
extern inline ALenum MidiSynth_getState(const MidiSynth *self);

void MidiSynth_stop(MidiSynth *self)
{
    ResetEvtQueue(&self->EventQueue);

    self->LastEvtTime = 0;
    self->NextEvtTime = UINT64_MAX;
    self->SamplesSinceLast = 0.0;
    self->SamplesToNext = 0.0;
}

extern inline void MidiSynth_reset(MidiSynth *self);

ALuint64 MidiSynth_getTime(const MidiSynth *self)
{
    ALuint64 time = self->LastEvtTime + (self->SamplesSinceLast/self->SamplesPerTick);
    return clampu64(time, self->LastEvtTime, self->NextEvtTime);
}

extern inline ALuint64 MidiSynth_getNextEvtTime(const MidiSynth *self);

void MidiSynth_setSampleRate(MidiSynth *self, ALdouble srate)
{
    ALdouble sampletickrate = srate / TICKS_PER_SECOND;

    self->SamplesSinceLast = self->SamplesSinceLast * sampletickrate / self->SamplesPerTick;
    self->SamplesToNext = self->SamplesToNext * sampletickrate / self->SamplesPerTick;
    self->SamplesPerTick = sampletickrate;
}

extern inline void MidiSynth_update(MidiSynth *self, ALCdevice *device);

ALenum MidiSynth_insertEvent(MidiSynth *self, ALuint64 time, ALuint event, ALsizei param1, ALsizei param2)
{
    MidiEvent entry;
    ALenum err;

    entry.time = time;
    entry.event = event;
    entry.param.val[0] = param1;
    entry.param.val[1] = param2;

    err = InsertEvtQueue(&self->EventQueue, &entry);
    if(err != AL_NO_ERROR) return err;

    if(entry.time < self->NextEvtTime)
    {
        self->NextEvtTime = entry.time;

        self->SamplesToNext  = (self->NextEvtTime - self->LastEvtTime) * self->SamplesPerTick;
        self->SamplesToNext -= self->SamplesSinceLast;
    }

    return AL_NO_ERROR;
}

ALenum MidiSynth_insertSysExEvent(MidiSynth *self, ALuint64 time, const ALbyte *data, ALsizei size)
{
    MidiEvent entry;
    ALenum err;

    entry.time = time;
    entry.event = SYSEX_EVENT;
    entry.param.sysex.size = size;
    entry.param.sysex.data = malloc(size);
    if(!entry.param.sysex.data)
        return AL_OUT_OF_MEMORY;
    memcpy(entry.param.sysex.data, data, size);

    err = InsertEvtQueue(&self->EventQueue, &entry);
    if(err != AL_NO_ERROR)
    {
        free(entry.param.sysex.data);
        return err;
    }

    if(entry.time < self->NextEvtTime)
    {
        self->NextEvtTime = entry.time;

        self->SamplesToNext  = (self->NextEvtTime - self->LastEvtTime) * self->SamplesPerTick;
        self->SamplesToNext -= self->SamplesSinceLast;
    }

    return AL_NO_ERROR;
}
