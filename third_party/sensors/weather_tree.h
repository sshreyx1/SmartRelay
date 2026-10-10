#ifndef WEATHER_MODEL_H
#define WEATHER_MODEL_H

// f[0] = temperature, f[1] = humidity, f[2] = light (12-bit ADC, 0-4095)
// Returns: 0 = Cloudy, 1 = Hot_Dry, 2 = Humid_Rainy
static inline int weather_predict(const float *f) {
    if (f[2] <= 2307.4012f) {
        if (f[2] <= 850.9608f) {
            if (f[1] <= 69.0876f) {
                return 0;
            }
            return 2;
        }
        if (f[0] <= 28.3f) {
            return 0;
        }
        return 1;
    }
    if (f[0] <= 27.55f) {
        return 0;
    }
    return 1;
}

static inline const char *weather_label(int idx) {
    switch (idx) {
        case 0:  return "Cloudy";
        case 1:  return "Hot_Dry";
        case 2:  return "Humid_Rainy";
        default: return "Unknown";
    }
}

#endif