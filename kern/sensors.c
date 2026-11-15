// SENSORS

#include "inc/abi.h"
#include "defs.h"
#include "sensors.h"

static const char *const typename[SENSOR_MAX_TYPES] = {
    "temp",   "fan",       "volt",    "acvolt",  "resistance",
    "power",  "current",   "watthour", "amphour", "indicator",
    "raw",    "percent",   "illuminance", "timedelta",
};

int sensor_attach(struct sensordev *sd, struct sensor *s) {
  struct sensor **pp;

  if (s->type >= SENSOR_MAX_TYPES)
    return -1;
  s->numt = sd->numt[s->type]++;
  s->next = 0;
  for (pp = &sd->sensors; *pp != 0; pp = &(*pp)->next)
    ;
  *pp = s;
  return 0;
}

static int fixed(char *out, size_t max, int64_t v) {
  int64_t a = v < 0 ? -v : v;

  return snprintf(out, max, "%s%ld.%02ld", v < 0 ? "-" : "", (long)(a / 1000000),
                  (long)(a % 1000000 / 10000));
}

static int format(void *arg, char *out, size_t max) {
  struct sensor *s = arg;
  char num[32];
  size_t n;

  if (s->flags & SENSOR_FINVALID)
    return snprintf(out, max, "invalid (%s)", s->desc);
  if (s->flags & SENSOR_FUNKNOWN)
    return snprintf(out, max, "unknown (%s)", s->desc);
  switch (s->type) {
  case SENSOR_TEMP:
    // kelvin to celsius: 273.15 off
    fixed(num, sizeof(num), s->value - 273150000);
    n = (size_t)snprintf(out, max, "%s degC", num);
    break;
  case SENSOR_FANRPM:
    n = (size_t)snprintf(out, max, "%ld RPM", (long)s->value);
    break;
  case SENSOR_VOLTS_DC:
  case SENSOR_VOLTS_AC:
    fixed(num, sizeof(num), s->value);
    n = (size_t)snprintf(out, max, "%s V %s", num,
                         s->type == SENSOR_VOLTS_DC ? "DC" : "AC");
    break;
  case SENSOR_OHMS:
    fixed(num, sizeof(num), s->value);
    n = (size_t)snprintf(out, max, "%s ohm", num);
    break;
  case SENSOR_WATTS:
    fixed(num, sizeof(num), s->value);
    n = (size_t)snprintf(out, max, "%s W", num);
    break;
  case SENSOR_AMPS:
    fixed(num, sizeof(num), s->value);
    n = (size_t)snprintf(out, max, "%s A", num);
    break;
  case SENSOR_WATTHOUR:
    fixed(num, sizeof(num), s->value);
    n = (size_t)snprintf(out, max, "%s Wh", num);
    break;
  case SENSOR_AMPHOUR:
    fixed(num, sizeof(num), s->value);
    n = (size_t)snprintf(out, max, "%s Ah", num);
    break;
  case SENSOR_INDICATOR:
    n = (size_t)snprintf(out, max, "%s", s->value ? "On" : "Off");
    break;
  case SENSOR_PERCENT:
    fixed(num, sizeof(num), s->value * 1000);
    n = (size_t)snprintf(out, max, "%s%%", num);
    break;
  case SENSOR_LUX:
    fixed(num, sizeof(num), s->value);
    n = (size_t)snprintf(out, max, "%s lx", num);
    break;
  case SENSOR_TIMEDELTA:
    fixed(num, sizeof(num), s->value);
    n = (size_t)snprintf(out, max, "%s secs", num);
    break;
  default:
    n = (size_t)snprintf(out, max, "%ld", (long)s->value);
    break;
  }
  if (n < max && s->desc[0] != 0)
    n += (size_t)snprintf(out + n, max - n, " (%s)", s->desc);
  if (n < max && s->status == SENSOR_S_WARN)
    n += (size_t)snprintf(out + n, max - n, ", WARNING");
  else if (n < max && s->status == SENSOR_S_CRIT)
    n += (size_t)snprintf(out + n, max - n, ", CRITICAL");
  return (int)n;
}

int sensordev_install(struct sensordev *sd) {
  struct sensor *s;

  if (sd->installed)
    return -1;
  for (s = sd->sensors; s != 0; s = s->next) {
    snprintf(s->node, sizeof(s->node), "hw.sensors.%s.%s%d", sd->xname,
             typename[s->type], s->numt);
    sysctl_add_string_fn(s->node, format, s, FNU_PLEDGE_VMINFO);
  }
  sd->installed = 1;
  return 0;
}
