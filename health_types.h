#pragma once

enum MeasureStatus { M_OK, M_NO_FINGER, M_BAD_SIGNAL, M_SENSOR_ERROR };

struct Vitals {
    MeasureStatus status;
    int hr;
    int spo2;
};
