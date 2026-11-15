#ifndef PRONINX_SENSORS_H
#define PRONINX_SENSORS_H

#include "inc/types.h"

// SENSORS

enum sensor_type {
  SENSOR_TEMP,       // micro-kelvin
  SENSOR_FANRPM,     // revolutions a minute
  SENSOR_VOLTS_DC,   // micro-volts
  SENSOR_VOLTS_AC,
  SENSOR_OHMS,       // micro-ohms
  SENSOR_WATTS,      // micro-watts
  SENSOR_AMPS,       // micro-amperes
  SENSOR_WATTHOUR,   // micro-watt-hours
  SENSOR_AMPHOUR,    // micro-ampere-hours
  SENSOR_INDICATOR,  // 0 or 1
  SENSOR_INTEGER,
  SENSOR_PERCENT,    // thousandths of a percent
  SENSOR_LUX,        // micro-lux
  SENSOR_TIMEDELTA,  // micro-seconds
  SENSOR_MAX_TYPES
};

enum sensor_status {
  SENSOR_S_UNSPEC, // no opinion
  SENSOR_S_OK,
  SENSOR_S_WARN,
  SENSOR_S_CRIT,
  SENSOR_S_UNKNOWN,
};

#define SENSOR_FINVALID 0x0001 // no reading
#define SENSOR_FUNKNOWN 0x0002 // reading is not known

#define SENSOR_NAME 56

struct sensor {
  char desc[32];
  enum sensor_type type;
  enum sensor_status status;
  int flags;
  int64_t value;
  int numt;
  char node[SENSOR_NAME]; // its name in sysctl
  struct sensor *next;
};

struct sensordev {
  char xname[16];
  struct sensor *sensors;
  int numt[SENSOR_MAX_TYPES];
  int installed;
};

int sensor_attach(struct sensordev *sd, struct sensor *s);
int sensordev_install(struct sensordev *sd);

#endif
