/*
    Copyright (C) 2010-2011 wpitchoune@gmail.com

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
    02110-1301 USA
*/

#include <stdlib.h>
#include <string.h>

#include <sensors/sensors.h>
#include <sensors/error.h>

#include "hdd.h"
#include "psensor.h"
#include "lmsensor.h"

struct psensor *psensor_create(char *id, char *name,
			       unsigned int type, int values_max_length)
{
	struct psensor *psensor
	    = (struct psensor *)malloc(sizeof(struct psensor));

	psensor->id = id;
	psensor->name = name;
	psensor->enabled = 1;
	psensor->min = UNKNOWN_VALUE;
	psensor->max = UNKNOWN_VALUE;

	psensor->type = type;

	psensor->values_max_length = values_max_length;
	psensor->measures = measures_create(values_max_length);

	psensor->alarm_limit = 0;

	psensor->cb_alarm_raised = NULL;
	psensor->cb_alarm_raised_data = NULL;
	psensor->alarm_raised = 0;

	psensor->alarm_enabled = 0;

	psensor->url = NULL;

	psensor->color = NULL;

	return psensor;
}

void psensor_values_resize(struct psensor *s, int new_size)
{
	struct measure *new_ms, *cur_ms;
	int cur_size;

	cur_size = s->values_max_length;
	cur_ms = s->measures;
	new_ms = measures_create(new_size);

	if (cur_ms) {
		int i;
		for (i = 0; i < new_size - 1 && i < cur_size - 1; i++)
			measure_copy(&cur_ms[cur_size - i - 1],
				     &new_ms[new_size - i - 1]);

		measures_free(s->measures);
	}

	s->values_max_length = new_size;
	s->measures = new_ms;
}

void psensor_free(struct psensor *sensor)
{
	if (sensor) {
		free(sensor->name);
		free(sensor->id);

		if (sensor->color)
			free(sensor->color);

		measures_free(sensor->measures);

		free(sensor->url);

		free(sensor);
	}
}

void psensor_list_free(struct psensor **sensors)
{
	struct psensor **sensor_cur;

	if (sensors) {
		sensor_cur = sensors;

		while (*sensor_cur) {
			psensor_free(*sensor_cur);

			sensor_cur++;
		}

		free(sensors);

		sensors = NULL;
	}
}

int psensor_list_size(struct psensor **sensors)
{
	int size;
	struct psensor **sensor_cur;

	if (!sensors)
		return 0;

	size = 0;
	sensor_cur = sensors;

	while (*sensor_cur) {
		size++;
		sensor_cur++;
	}
	return size;
}

int psensor_list_contains_type(struct psensor **sensors, unsigned int type)
{
	struct psensor **s;

	if (!sensors)
		return 0;

	s = sensors;
	while (*s) {
		if ((*s)->type == type)
			return 1;
		s++;
	}

	return 0;
}

struct psensor **psensor_list_add(struct psensor **sensors,
				  struct psensor *sensor)
{
	int size = psensor_list_size(sensors);

	struct psensor **result
	    = malloc((size + 1 + 1) * sizeof(struct psensor *));

	if (sensors)
		memcpy(result, sensors, size * sizeof(struct psensor *));

	result[size] = sensor;
	result[size + 1] = NULL;

	return result;
}

struct psensor *psensor_list_get_by_id(struct psensor **sensors, const char *id)
{
	struct psensor **sensors_cur = sensors;

	while (*sensors_cur) {
		if (!strcmp((*sensors_cur)->id, id))
			return *sensors_cur;

		sensors_cur++;
	}

	return NULL;
}

int is_temp_type(unsigned int type)
{
	return type & SENSOR_TYPE_TEMP;
}

int is_fan_type(unsigned int type)
{
	return type & SENSOR_TYPE_FAN;
}

char *psensor_value_to_string(unsigned int type, double value)
{
	/* should not be possible to exceed 20 characters with temp or
	   rpm values the .x part is never displayed */
	char *str = malloc(20);

	char *unit;

	if (is_temp_type(type))
		unit = "C";
	else
		unit = "";

	sprintf(str, "%.0f%s", value, unit);

	return str;
}

void psensor_set_current_value(struct psensor *sensor, double value)
{
	struct timeval tv;

	if (gettimeofday(&tv, NULL) != 0)
		timerclear(&tv);

	psensor_set_current_measure(sensor, value, tv);
}

void
psensor_set_current_measure(struct psensor *s,
			    double v, struct timeval tv)
{
	memmove(s->measures,
		&s->measures[1],
		(s->values_max_length - 1) * sizeof(struct measure));

	s->measures[s->values_max_length - 1].value = v;
	s->measures[s->values_max_length - 1].time = tv;

	if (s->min == UNKNOWN_VALUE || v < s->min)
		s->min = v;

	if (s->max == UNKNOWN_VALUE || v > s->max)
		s->max = v;

	if (s->alarm_limit && s->alarm_enabled) {
		if (v > s->alarm_limit) {
			if (!s->alarm_raised && s->cb_alarm_raised)
				s->cb_alarm_raised(s,
						   s->cb_alarm_raised_data);

			s->alarm_raised = 1;
		} else {
			s->alarm_raised = 0;
		}
	}
}

double psensor_get_current_value(struct psensor *sensor)
{
	return sensor->measures[sensor->values_max_length - 1].value;
}

struct measure *psensor_get_current_measure(struct psensor *sensor)
{
	return &sensor->measures[sensor->values_max_length - 1];
}

/*
  Returns the minimal value of a given 'type' (SENSOR_TYPE_TEMP or
  SENSOR_TYPE_FAN)
 */
double get_min_value(struct psensor **sensors, int type)
{
	double m = UNKNOWN_VALUE;
	struct psensor **s = sensors;

	while (*s) {
		struct psensor *sensor = *s;

		if (sensor->enabled && (sensor->type & type)) {
			int i;
			double t;

			for (i = 0; i < sensor->values_max_length; i++) {
				t = sensor->measures[i].value;

				if (t == UNKNOWN_VALUE)
					continue;

				if (m == UNKNOWN_VALUE || t < m)
					m = t;
			}
		}
		s++;
	}

	return m;
}

/*
  Returns the maximal value of a given 'type' (SENSOR_TYPE_TEMP or
  SENSOR_TYPE_FAN)
 */
static double get_max_value(struct psensor **sensors, int type)
{
	double m = UNKNOWN_VALUE;
	struct psensor **s = sensors;

	while (*s) {
		struct psensor *sensor = *s;

		if (sensor->enabled && (sensor->type & type)) {
			int i;
			double t;
			for (i = 0; i < sensor->values_max_length; i++) {
				t = sensor->measures[i].value;

				if (t == UNKNOWN_VALUE)
					continue;

				if (m == UNKNOWN_VALUE || t > m)
					m = t;
			}
		}
		s++;
	}

	return m;
}

double get_min_temp(struct psensor **sensors)
{
	return get_min_value(sensors, SENSOR_TYPE_TEMP);
}

double get_min_rpm(struct psensor **sensors)
{
	return get_min_value(sensors, SENSOR_TYPE_FAN);
}

double get_max_rpm(struct psensor **sensors)
{
	return get_max_value(sensors, SENSOR_TYPE_FAN);
}

double get_max_temp(struct psensor **sensors)
{
	return get_max_value(sensors, SENSOR_TYPE_TEMP);
}

struct psensor **get_all_sensors(int values_max_length)
{
	struct psensor **psensors = NULL;
	int count = 0;
	const sensors_chip_name *chip;
	int chip_nr = 0;
	struct psensor **tmp_psensors;
	const sensors_feature *feature;
	struct psensor *psensor;
	int i;

	while ((chip = sensors_get_detected_chips(NULL, &chip_nr))) {
		i = 0;
		while ((feature = sensors_get_features(chip, &i))) {

			if (feature->type == SENSORS_FEATURE_TEMP
			    || feature->type == SENSORS_FEATURE_FAN) {

				psensor = lmsensor_psensor_create
					(chip, feature, values_max_length);

				if (psensor) {
					tmp_psensors
						= psensor_list_add(psensors,
								   psensor);

					free(psensors);

					psensors = tmp_psensors;

					count++;
				}
			}
		}
	}

	tmp_psensors = hdd_psensor_list_add(psensors, values_max_length);

	if (tmp_psensors != psensors) {
		free(psensors);
		psensors = tmp_psensors;
	}

	if (!psensors) {	/* there is no detected sensors */
		psensors = malloc(sizeof(struct psensor *));
		*psensors = NULL;
	}

	return psensors;
}

const char *psensor_type_to_str(unsigned int type)
{
	if (type & SENSOR_TYPE_REMOTE)
		return "Remote";

	if (type & SENSOR_TYPE_LMSENSOR_TEMP)
		return "Temperature";

	if (type & SENSOR_TYPE_LMSENSOR_FAN)
		return "Fan";

	if (type & SENSOR_TYPE_NVIDIA)
		return "NVidia GPU Temperature";

	if (type & SENSOR_TYPE_HDD_TEMP)
		return "HDD Temperature";

	return "N/A";		/* should not be possible */
}

void psensor_list_update_measures(struct psensor **sensors)
{
	lmsensor_psensor_list_update(sensors);
	hdd_psensor_list_update(sensors);
}
