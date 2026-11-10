#include "analog_agc.h"

#include <math.h>

#include "webrtc_apm.h"

// Chrome reads the volume of the capture device about once a second; between reads the last level counts.
#define FRAMES_PER_VOLUME_READ 100

static int level_of(double volume) {
    long level = lround(volume * WEBRTC_APM_ANALOG_LEVEL_MAXIMUM);
    return level < 0 ? 0 : level > WEBRTC_APM_ANALOG_LEVEL_MAXIMUM ? WEBRTC_APM_ANALOG_LEVEL_MAXIMUM : (int)level;
}

bool analog_agc_start(struct analog_agc *agc, struct source_volume volume) {
    double current;
    pthread_mutex_lock(&agc->lock);
    agc->volume = volume;
    agc->active = volume.save(volume.data) && volume.get(volume.data, &current);
    if (agc->active) {
        agc->level = level_of(current);
        agc->frames_to_read = FRAMES_PER_VOLUME_READ;
    }
    pthread_mutex_unlock(&agc->lock);
    return agc->active;
}

int analog_agc_level(struct analog_agc *agc) {
    pthread_mutex_lock(&agc->lock);
    int level = -1;
    if (agc->active) {
        double current;
        if (--agc->frames_to_read == 0) {
            agc->frames_to_read = FRAMES_PER_VOLUME_READ;
            if (agc->volume.get(agc->volume.data, &current)) agc->level = level_of(current);
        }
        level = agc->level;
    }
    pthread_mutex_unlock(&agc->lock);
    return level;
}

void analog_agc_apply(struct analog_agc *agc, int recommended) {
    pthread_mutex_lock(&agc->lock);
    if (agc->active && recommended != agc->level &&
        agc->volume.set(agc->volume.data, (double)recommended / WEBRTC_APM_ANALOG_LEVEL_MAXIMUM))
        agc->level = recommended;
    pthread_mutex_unlock(&agc->lock);
}

void analog_agc_stop(struct analog_agc *agc) {
    pthread_mutex_lock(&agc->lock);
    if (agc->active) agc->volume.restore(agc->volume.data);
    agc->active = false;
    pthread_mutex_unlock(&agc->lock);
}
