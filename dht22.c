/*
 * dht22.c
 *  DHT22 Sensor nagios plugin for Single Board Computers
 *  Copyright (c) 2017 Frostbyte <frostbytegr@gmail.com>
 *
 *  Check environment temperature and humidity with DHT22 via GPIO
 *
 *  Credits:
 *  <projects@drogon.net> - wiringPi library and rht03 code
 *  <devel@nagios-plugins.org> - plugin development guidelines
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sched.h>
#include <sys/time.h>

// wiringPi library
#include "ThirdParty/wiringPi/wiringPi.h"

// Sensor library
#include "dht22.h"

// Boolean definitions
#ifndef	TRUE
#	define	TRUE	(1==1)
#	define	FALSE	(!TRUE)
#endif

// Sensor definitions
#define QUERYRETRIES 4

// Function to set the scheduling policy with maximum priority
static void setMaximumPriority() {
	struct sched_param sched;

	// Utilize the FIFO scheduler and assign the highest possible priority so context switching is avoided
	memset(&sched, 0, sizeof(sched));
	sched.sched_priority = sched_get_priority_max(SCHED_FIFO);
	sched_setscheduler(0, SCHED_FIFO, &sched);
}

// Function to set the scheduling policy with default priority
static void setDefaultPriority() {
	struct sched_param sched;

	// Revert back to the standard round-robin scheduler
	memset(&sched, 0, sizeof(sched));
	sched.sched_priority = 0;
	sched_setscheduler(0, SCHED_OTHER, &sched);
}

// Function to enforce delay until the GPIO transitions from LOW to HIGH state
static int sensorLowHighWait(int GPIO) {
	struct timeval now, timeOut, timeUp;

	// Set a timer of 1 ms
	gettimeofday(&now, NULL);
	timerclear(&timeOut);
	timeOut.tv_usec=1000;
	timeradd(&now, &timeOut, &timeUp);

	// If the GPIO is already in a HIGH state
	// Wait until it transitions to a LOW state
	while (digitalRead(GPIO)==HIGH) {
		gettimeofday(&now, NULL);

		// If the timer runs out
		if (timercmp(&now, &timeUp, >)) {
			return FALSE;
		}
	}

	// Set another time of 1 ms
	gettimeofday(&now, NULL);
	timerclear(&timeOut);
	timeOut.tv_usec=1000;
	timeradd(&now, &timeOut, &timeUp);

	// Wait until the GPIO transitions to a HIGH state
	while (digitalRead(GPIO)==LOW) {
		gettimeofday(&now, NULL);

		// If the timer runs out
		if (timercmp(&now, &timeUp, >)) {
			return FALSE;
		}
	}

	// Upon successful transition
	return TRUE;
}

// Function to retrieve a byte of data from the sensor
static uint8_t retrieveByte(int GPIO) {
	uint8_t result=0x00;

	// For every bit of the byte
	for (int bit=0; bit<8; ++bit) {
		// If the sensor transition fails
		if (!sensorLowHighWait(GPIO)) {
			return 0;
		}

		// Data retrieval needs to be timed
		delayMicroseconds(30);

		// Insert bits into the result by shifting them to the left
		result<<=1;

		// If the GPIO is in a HIGH state
		if (digitalRead(GPIO)==HIGH) {
			// Turn the bit that was just inserted to 1
			result|=1;
		}
	}

	// Return the processed byte
	return result;
}

// Function to query the sensor for information
static int querySensor(int GPIO, uint8_t results[4]) {
	struct timeval now, then, took;
	uint8_t queryChecksum=0x00, retrievedBytes[5];

	// Set priority to maximum
	setMaximumPriority();

	// Take a timestamp before the operation begins
	gettimeofday(&then, NULL);

	// Set the GPIO into OUTPUT mode so it's state can be manipulated
	pinMode(GPIO, OUTPUT);

	// Wake up the sensor by setting the GPIO to a LOW state for 10ms
	digitalWrite(GPIO, LOW);
	delay(10);

	// And then a HIGH state for 40ìs
	digitalWrite(GPIO, HIGH);
	delayMicroseconds(40);

	// Set the GPIO into INPUT mode so data can be read from it
	pinMode(GPIO, INPUT);

	// If the sensor transition fails
	if (!sensorLowHighWait(GPIO)) {
		return FALSE;
	}

	// Retrieve 5 bytes (40 bits) of information from the sensor
	for (int byte=0; byte<5; ++byte) {
		retrievedBytes[byte]=retrieveByte(GPIO);

		// For the first 4 bytes of information
		if (byte<4) {
			// Copy them to the results
			results[byte]=retrievedBytes[byte];
			// Add them to the query checksum
			queryChecksum+=retrievedBytes[byte];
		}
	}

	// Mask the query checksum
	queryChecksum&=0xFF;

	// Take another timestamp once the operation ends
	gettimeofday(&now, NULL);

	// Find out how long the operation took
	timersub(&now, &then, &took);

	// Set priority back to default
	setDefaultPriority();

	// The time it should take to complete the operation should be:
	//	10ms + 40µs - for sensor to reset
	//	+ 80µs + 80µs - for the sensor to transition from a LOW to a HIGH state
	//	+ 40 * ( 50µs + 27µs [0] or 70µs [1] ) - for the data to be retrieved from the sensor
	//	= 15010µs in total
	// If it took more than that, there has been a scheduling
	// interruption and the reading is probably invalid
	if ((took.tv_sec!=0) || (took.tv_usec>16000)) {
		return FALSE;
	}

	// Return the checksum validation result of the query
	return queryChecksum==retrievedBytes[4];
}

// Main query function for the DHT22 sensor
struct sensorOutput parseSensorOutput(int GPIO) {
	struct sensorOutput result;
	uint8_t sensorData[4];

	// If wiringPi fails to initialize
	if (wiringPiSetup()==-1) {
		// Throw an error and exit
		fprintf(stderr, "wiringPi failed to initialize.\n");
		fflush(stderr);
		exit(EXIT_FAILURE);
	}

	// Set the sensor query retry count
	int queryRetries=QUERYRETRIES+1;

	// While there are still retries remaining
	while (queryRetries--) {
		// Clean up any retrieved sensor data
		memset(sensorData, 0, sizeof(sensorData));

		// If the sensor query was successful
		if (querySensor(GPIO, sensorData)) {
			// Parse the temperature and humidity data
			result.temperature=(sensorData[2]*256+sensorData[3])/10.0;
			result.humidity=(sensorData[0]*256+sensorData[1])/10.0;

			// Check and adjust for negative temperatures
			if ((sensorData[2]&0x80)!=0) {
				result.temperature*=-1;
			}

			// If the retrieved data are within the sensor's documented capabilities
			if (result.temperature>=SENSOR_TMP_MIN && result.temperature<=SENSOR_TMP_MAX && result.humidity>=SENSOR_HUM_MIN && result.humidity<=SENSOR_HUM_MAX) {
				// Return the processed output
				return result;
			}
		}

		// Wait for 2 seconds before retrying
		delay(2000);
	}

	// If this part is reached, no measurement was valid

	// Set the output values to N/A
	result.temperature=SENSOR_NA;
	result.humidity=SENSOR_NA;

	// Return the processed output
	return result;
}
