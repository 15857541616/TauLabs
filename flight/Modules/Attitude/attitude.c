/**
 ******************************************************************************
 * @addtogroup OpenPilotModules OpenPilot Modules
 * @{
 * @addtogroup Attitude Copter Control Attitude Estimation
 * @brief Acquires sensor data and computes attitude estimate
 * Specifically updates the the @ref AttitudeActual "AttitudeActual" and @ref AttitudeRaw "AttitudeRaw" settings objects
 * @{
 *
 * @file       attitude.c
 * @author     The OpenPilot Team, http://www.openpilot.org Copyright (C) 2010.
 * @brief      Module to handle all comms to the AHRS on a periodic basis.
 *
 * @see        The GNU Public License (GPL) Version 3
 *
 ******************************************************************************/
/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
 * for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 */

/**
 * Input objects: None, takes sensor data via pios
 * Output objects: @ref AttitudeRaw @ref AttitudeActual
 *
 * This module computes an attitude estimate from the sensor data
 *
 * The module executes in its own thread.
 *
 * UAVObjects are automatically generated by the UAVObjectGenerator from
 * the object definition XML file.
 *
 * Modules have no API, all communication to other modules is done through UAVObjects.
 * However modules may use the API exposed by shared libraries.
 * See the OpenPilot wiki for more details.
 * http://www.openpilot.org/OpenPilot_Application_Architecture
 *
 */

#include "pios.h"
#include "attitude.h"
#include "gyros.h"
#include "accels.h"
#include "attitudeactual.h"
#include "attitudesettings.h"
#include "flightstatus.h"
#include "manualcontrolcommand.h"
#include "CoordinateConversions.h"
#include <pios_board_info.h>
 
// Private constants
#define STACK_SIZE_BYTES 540
#define TASK_PRIORITY (tskIDLE_PRIORITY+3)

#define SENSOR_PERIOD 4
#define UPDATE_RATE  25.0f
#define GYRO_NEUTRAL 1665

#define PI_MOD(x) (fmod(x + M_PI, M_PI * 2) - M_PI)
// Private types

// Private variables
static xTaskHandle taskHandle;

// Private functions
static void AttitudeTask(void *parameters);

static float gyro_correct_int[3] = {0,0,0};
static xQueueHandle gyro_queue;

static int32_t updateSensors(AccelsData *, GyrosData *);
static int32_t updateSensorsCC3D(AccelsData * accelsData, GyrosData * gyrosData);
static void updateAttitude(AccelsData *, GyrosData *);
static void settingsUpdatedCb(UAVObjEvent * objEv);

static float accelKi = 0;
static float accelKp = 0;
static float accel_alpha = 0;
static bool accel_filter_enabled = false;
static float yawBiasRate = 0;
static float gyroGain = 0.42;
static int16_t accelbias[3];
static float q[4] = {1,0,0,0};
static float R[3][3];
static int8_t rotate = 0;
static bool zero_during_arming = false;
static bool bias_correct_gyro = true;

// For running trim flights
static volatile bool trim_requested = false;
static volatile int32_t trim_accels[3];
static volatile int32_t trim_samples;
int32_t const MAX_TRIM_FLIGHT_SAMPLES = 65535;

#define GRAV         9.81f
#define ACCEL_SCALE  (GRAV * 0.004f)
/* 0.004f is gravity / LSB */

/**
 * Initialise the module, called on startup
 * \returns 0 on success or -1 if initialisation failed
 */
int32_t AttitudeStart(void)
{
	
	// Start main task
	xTaskCreate(AttitudeTask, (signed char *)"Attitude", STACK_SIZE_BYTES/4, NULL, TASK_PRIORITY, &taskHandle);
	TaskMonitorAdd(TASKINFO_RUNNING_ATTITUDE, taskHandle);
	PIOS_WDG_RegisterFlag(PIOS_WDG_ATTITUDE);
	
	return 0;
}

/**
 * Initialise the module, called on startup
 * \returns 0 on success or -1 if initialisation failed
 */
int32_t AttitudeInitialize(void)
{
	AttitudeActualInitialize();
	AttitudeSettingsInitialize();
	AccelsInitialize();
	GyrosInitialize();
	
	// Initialize quaternion
	AttitudeActualData attitude;
	AttitudeActualGet(&attitude);
	attitude.q1 = 1;
	attitude.q2 = 0;
	attitude.q3 = 0;
	attitude.q4 = 0;
	AttitudeActualSet(&attitude);
	
	// Cannot trust the values to init right above if BL runs
	gyro_correct_int[0] = 0;
	gyro_correct_int[1] = 0;
	gyro_correct_int[2] = 0;
	
	q[0] = 1;
	q[1] = 0;
	q[2] = 0;
	q[3] = 0;
	for(uint8_t i = 0; i < 3; i++)
		for(uint8_t j = 0; j < 3; j++)
			R[i][j] = 0;
	
	trim_requested = false;
	
	AttitudeSettingsConnectCallback(&settingsUpdatedCb);
	
	return 0;
}

MODULE_INITCALL(AttitudeInitialize, AttitudeStart)

/**
 * Module thread, should not return.
 */
 
int32_t accel_test;
int32_t gyro_test;
static void AttitudeTask(void *parameters)
{
	uint8_t init = 0;
	AlarmsClear(SYSTEMALARMS_ALARM_ATTITUDE);
	
	// Set critical error and wait until the accel is producing data
	while(PIOS_ADXL345_FifoElements() == 0) {
		AlarmsSet(SYSTEMALARMS_ALARM_ATTITUDE, SYSTEMALARMS_ALARM_CRITICAL);
		PIOS_WDG_UpdateFlag(PIOS_WDG_ATTITUDE);
	}
	
	const struct pios_board_info * bdinfo = &pios_board_info_blob;
	
	bool cc3d = bdinfo->board_rev == 0x02;

	if(cc3d) {
#if defined(PIOS_INCLUDE_MPU6000)
		gyro_test = PIOS_MPU6000_Test();
#endif
	} else {
#if defined(PIOS_INCLUDE_ADXL345)
		accel_test = PIOS_ADXL345_Test();
#endif

#if defined(PIOS_INCLUDE_ADC)
		// Create queue for passing gyro data, allow 2 back samples in case
		gyro_queue = xQueueCreate(1, sizeof(float) * 4);
		PIOS_Assert(gyro_queue != NULL);
		PIOS_ADC_SetQueue(gyro_queue);
		PIOS_ADC_Config((PIOS_ADC_RATE / 1000.0f) * UPDATE_RATE);
#endif

	}
	// Force settings update to make sure rotation loaded
	settingsUpdatedCb(AttitudeSettingsHandle());
	
	// Main task loop
	while (1) {
		
		FlightStatusData flightStatus;
		FlightStatusGet(&flightStatus);
		
		if((xTaskGetTickCount() < 7000) && (xTaskGetTickCount() > 1000)) {
			// For first 7 seconds use accels to get gyro bias
			accelKp = 1;
			accelKi = 0.9;
			yawBiasRate = 0.23;
			accel_filter_enabled = false;
			init = 0;
		}
		else if (zero_during_arming && (flightStatus.Armed == FLIGHTSTATUS_ARMED_ARMING)) {
			accelKp = 1;
			accelKi = 0.9;
			yawBiasRate = 0.23;
			accel_filter_enabled = false;
			init = 0;
		} else if (init == 0) {
			// Reload settings (all the rates)
			AttitudeSettingsAccelKiGet(&accelKi);
			AttitudeSettingsAccelKpGet(&accelKp);
			AttitudeSettingsYawBiasRateGet(&yawBiasRate);
			if(accel_alpha > 0.0f)
				accel_filter_enabled = true;
			init = 1;
		}
		
		PIOS_WDG_UpdateFlag(PIOS_WDG_ATTITUDE);

		AccelsData accels;
		GyrosData gyros;
		int32_t retval = 0;

		if (cc3d)
			retval = updateSensorsCC3D(&accels, &gyros);
		else
			retval = updateSensors(&accels, &gyros);

		// Only update attitude when sensor data is good
		if (retval != 0)
			AlarmsSet(SYSTEMALARMS_ALARM_ATTITUDE, SYSTEMALARMS_ALARM_ERROR);
		else {
			// Do not update attitude data in simulation mode
			if (!AttitudeActualReadOnly())
				updateAttitude(&accels, &gyros);

			AlarmsClear(SYSTEMALARMS_ALARM_ATTITUDE);
		}
	}
}

/**
 * Get an update from the sensors
 * @param[in] attitudeRaw Populate the UAVO instead of saving right here
 * @return 0 if successfull, -1 if not
 */
static int32_t updateSensors(AccelsData * accels, GyrosData * gyros)
{
	struct pios_adxl345_data accel_data;
	float gyro[4];
	
	// Only wait the time for two nominal updates before setting an alarm
	if(xQueueReceive(gyro_queue, (void * const) gyro, UPDATE_RATE * 2) == errQUEUE_EMPTY) {
		AlarmsSet(SYSTEMALARMS_ALARM_ATTITUDE, SYSTEMALARMS_ALARM_ERROR);
		return -1;
	}

	// Do not read raw sensor data in simulation mode
	if (GyrosReadOnly() || AccelsReadOnly())
		return 0;

	// No accel data available
	if(PIOS_ADXL345_FifoElements() == 0)
		return -1;
	
	// First sample is temperature
	gyros->x = -(gyro[1] - GYRO_NEUTRAL) * gyroGain;
	gyros->y = (gyro[2] - GYRO_NEUTRAL) * gyroGain;
	gyros->z = -(gyro[3] - GYRO_NEUTRAL) * gyroGain;
	
	int32_t x = 0;
	int32_t y = 0;
	int32_t z = 0;
	uint8_t i = 0;
	uint8_t samples_remaining;
	do {
		i++;
		samples_remaining = PIOS_ADXL345_Read(&accel_data);
		x +=  accel_data.x;
		y += -accel_data.y;
		z += -accel_data.z;
	} while ( (i < 32) && (samples_remaining > 0) );
	gyros->temperature = samples_remaining;

	float accel[3] = {(float) x / i, (float) y / i, (float) z / i};
	
	if(rotate) {
		// TODO: rotate sensors too so stabilization is well behaved
		float vec_out[3];
		rot_mult(R, accel, vec_out,false);
		accels->x = vec_out[0];
		accels->y = vec_out[1];
		accels->z = vec_out[2];
		rot_mult(R, &gyros->x, vec_out,false);
		gyros->x = vec_out[0];
		gyros->y = vec_out[1];
		gyros->z = vec_out[2];
	} else {
		accels->x = accel[0];
		accels->y = accel[1];
		accels->z = accel[2];
	}
	
	if (trim_requested) {
		if (trim_samples >= MAX_TRIM_FLIGHT_SAMPLES) {
			trim_requested = false;
		} else {
			uint8_t armed;
			float throttle;
			FlightStatusArmedGet(&armed);
			ManualControlCommandThrottleGet(&throttle);  // Until flight status indicates airborne
			if ((armed == FLIGHTSTATUS_ARMED_ARMED) && (throttle > 0)) {
				trim_samples++;
				// Store the digitally scaled version since that is what we use for bias
				trim_accels[0] += accels->x;
				trim_accels[1] += accels->y;
				trim_accels[2] += accels->z;
			}
		}
	}
	
	// Scale accels and correct bias
	accels->x = (accels->x - accelbias[0]) * ACCEL_SCALE;
	accels->y = (accels->y - accelbias[1]) * ACCEL_SCALE;
	accels->z = (accels->z - accelbias[2]) * ACCEL_SCALE;
	
	if(bias_correct_gyro) {
		// Applying integral component here so it can be seen on the gyros and correct bias
		gyros->x += gyro_correct_int[0];
		gyros->y += gyro_correct_int[1];
		gyros->z += gyro_correct_int[2];
	}
	
	// Because most crafts wont get enough information from gravity to zero yaw gyro, we try
	// and make it average zero (weakly)
	gyro_correct_int[2] += - gyros->z * yawBiasRate;

	GyrosSet(gyros);
	AccelsSet(accels);

	return 0;
}

/**
 * Get an update from the sensors
 * @param[in] attitudeRaw Populate the UAVO instead of saving right here
 * @return 0 if successfull, -1 if not
 */
struct pios_mpu6000_data mpu6000_data;
static int32_t updateSensorsCC3D(AccelsData * accelsData, GyrosData * gyrosData)
{
	float accels[3], gyros[3];
	
#if defined(PIOS_INCLUDE_MPU6000)
	
	xQueueHandle queue = PIOS_MPU6000_GetQueue();
	
	if(xQueueReceive(queue, (void *) &mpu6000_data, SENSOR_PERIOD) == errQUEUE_EMPTY)
		return -1;	// Error, no data

	// Do not read raw sensor data in simulation mode
	if (GyrosReadOnly() || AccelsReadOnly())
		return 0;

	gyros[0] = mpu6000_data.gyro_x * PIOS_MPU6000_GetScale();
	gyros[1] = mpu6000_data.gyro_y * PIOS_MPU6000_GetScale();
	gyros[2] = mpu6000_data.gyro_z * PIOS_MPU6000_GetScale();
	
	accels[0] = mpu6000_data.accel_x * PIOS_MPU6000_GetAccelScale();
	accels[1] = mpu6000_data.accel_y * PIOS_MPU6000_GetAccelScale();
	accels[2] = mpu6000_data.accel_z * PIOS_MPU6000_GetAccelScale();

	gyrosData->temperature = 35.0f + ((float) mpu6000_data.temperature + 512.0f) / 340.0f;
	accelsData->temperature = 35.0f + ((float) mpu6000_data.temperature + 512.0f) / 340.0f;
#endif

	if(rotate) {
		// TODO: rotate sensors too so stabilization is well behaved
		float vec_out[3];
		rot_mult(R, accels, vec_out, false);
		accels[0] = vec_out[0];
		accels[1] = vec_out[1];
		accels[2] = vec_out[2];
		rot_mult(R, gyros, vec_out, false);
		gyros[0] = vec_out[0];
		gyros[1] = vec_out[1];
		gyros[2] = vec_out[2];
	}

	accelsData->x = accels[0] - accelbias[0] * ACCEL_SCALE; // Applying arbitrary scale here to match CC v1
	accelsData->y = accels[1] - accelbias[1] * ACCEL_SCALE;
	accelsData->z = accels[2] - accelbias[2] * ACCEL_SCALE;

	gyrosData->x = gyros[0];
	gyrosData->y = gyros[1];
	gyrosData->z = gyros[2];

	if(bias_correct_gyro) {
		// Applying integral component here so it can be seen on the gyros and correct bias
		gyrosData->x += gyro_correct_int[0];
		gyrosData->y += gyro_correct_int[1];
		gyrosData->z += gyro_correct_int[2];
	}

	// Because most crafts wont get enough information from gravity to zero yaw gyro, we try
	// and make it average zero (weakly)
	gyro_correct_int[2] += - gyrosData->z * yawBiasRate;

	GyrosSet(gyrosData);
	AccelsSet(accelsData);

	return 0;
}

static inline void apply_accel_filter(const float * raw, float * filtered)
{
	if(accel_filter_enabled) {
		filtered[0] = filtered[0] * accel_alpha + raw[0] * (1 - accel_alpha);
		filtered[1] = filtered[1] * accel_alpha + raw[1] * (1 - accel_alpha);
		filtered[2] = filtered[2] * accel_alpha + raw[2] * (1 - accel_alpha);
	} else {
		filtered[0] = raw[0];
		filtered[1] = raw[1];
		filtered[2] = raw[2];
	}
}

static void updateAttitude(AccelsData * accelsData, GyrosData * gyrosData)
{
	float dT;
	portTickType thisSysTime = xTaskGetTickCount();
	static portTickType lastSysTime = 0;
	static float accels_filtered[3] = {0,0,0};
	static float grot_filtered[3] = {0,0,0};

	dT = (thisSysTime == lastSysTime) ? 0.001 : (portMAX_DELAY & (thisSysTime - lastSysTime)) / portTICK_RATE_MS / 1000.0f;
	lastSysTime = thisSysTime;
	
	// Bad practice to assume structure order, but saves memory
	float * gyros = &gyrosData->x;
	float * accels = &accelsData->x;
	
	float grot[3];
	float accel_err[3];

	// Apply smoothing to accel values, to reduce vibration noise before main calculations.
	apply_accel_filter(accels,accels_filtered);
	
	// Rotate gravity to body frame, filter and cross with accels
	grot[0] = -(2 * (q[1] * q[3] - q[0] * q[2]));
	grot[1] = -(2 * (q[2] * q[3] + q[0] * q[1]));
	grot[2] = -(q[0] * q[0] - q[1]*q[1] - q[2]*q[2] + q[3]*q[3]);

	// Apply same filtering to the rotated attitude to match delays
	apply_accel_filter(grot,grot_filtered);
	
	// Compute the error between the predicted direction of gravity and smoothed acceleration
	CrossProduct((const float *) accels_filtered, (const float *) grot_filtered, accel_err);
	
	// Account for accel magnitude
	float accel_mag = sqrtf(accels_filtered[0]*accels_filtered[0] + accels_filtered[1]*accels_filtered[1] + accels_filtered[2]*accels_filtered[2]);

	// Account for filtered gravity vector magnitude
	float grot_mag;

	if (accel_filter_enabled)
		grot_mag = sqrtf(grot_filtered[0]*grot_filtered[0] + grot_filtered[1]*grot_filtered[1] + grot_filtered[2]*grot_filtered[2]);
	else
		grot_mag = 1.0f;

	if (grot_mag > 1.0e-3f && accel_mag > 1.0e-3f) {
		accel_err[0] /= (accel_mag*grot_mag);
		accel_err[1] /= (accel_mag*grot_mag);
		accel_err[2] /= (accel_mag*grot_mag);
		
		// Accumulate integral of error.  Scale here so that units are (deg/s) but Ki has units of s
		gyro_correct_int[0] += accel_err[0] * accelKi;
		gyro_correct_int[1] += accel_err[1] * accelKi;
		
		//gyro_correct_int[2] += accel_err[2] * accelKi;
		
		// Correct rates based on error, integral component dealt with in updateSensors
		gyros[0] += accel_err[0] * accelKp / dT;
		gyros[1] += accel_err[1] * accelKp / dT;
		gyros[2] += accel_err[2] * accelKp / dT;
	}
	
	{ // scoping variables to save memory
		// Work out time derivative from INSAlgo writeup
		// Also accounts for the fact that gyros are in deg/s
		float qdot[4];
		qdot[0] = (-q[1] * gyros[0] - q[2] * gyros[1] - q[3] * gyros[2]) * dT * M_PI / 180 / 2;
		qdot[1] = (q[0] * gyros[0] - q[3] * gyros[1] + q[2] * gyros[2]) * dT * M_PI / 180 / 2;
		qdot[2] = (q[3] * gyros[0] + q[0] * gyros[1] - q[1] * gyros[2]) * dT * M_PI / 180 / 2;
		qdot[3] = (-q[2] * gyros[0] + q[1] * gyros[1] + q[0] * gyros[2]) * dT * M_PI / 180 / 2;
		
		// Take a time step
		q[0] = q[0] + qdot[0];
		q[1] = q[1] + qdot[1];
		q[2] = q[2] + qdot[2];
		q[3] = q[3] + qdot[3];
		
		if(q[0] < 0) {
			q[0] = -q[0];
			q[1] = -q[1];
			q[2] = -q[2];
			q[3] = -q[3];
		}
	}
	
	// Renomalize
	float qmag = sqrtf(q[0]*q[0] + q[1]*q[1] + q[2]*q[2] + q[3]*q[3]);
	q[0] = q[0] / qmag;
	q[1] = q[1] / qmag;
	q[2] = q[2] / qmag;
	q[3] = q[3] / qmag;
	
	// If quaternion has become inappropriately short or is nan reinit.
	// THIS SHOULD NEVER ACTUALLY HAPPEN
	if((fabs(qmag) < 1e-3) || (qmag != qmag)) {
		q[0] = 1;
		q[1] = 0;
		q[2] = 0;
		q[3] = 0;
	}
	
	AttitudeActualData attitudeActual;
	AttitudeActualGet(&attitudeActual);
	
	quat_copy(q, &attitudeActual.q1);
	
	// Convert into eueler degrees (makes assumptions about RPY order)
	Quaternion2RPY(&attitudeActual.q1,&attitudeActual.Roll);
	
	AttitudeActualSet(&attitudeActual);
}

static void settingsUpdatedCb(UAVObjEvent * objEv) {
	AttitudeSettingsData attitudeSettings;
	AttitudeSettingsGet(&attitudeSettings);
	
	
	accelKp = attitudeSettings.AccelKp;
	accelKi = attitudeSettings.AccelKi;
	yawBiasRate = attitudeSettings.YawBiasRate;
	gyroGain = attitudeSettings.GyroGain;

	// Calculate accel filter alpha, in the same way as for gyro data in stabilization module.
	const float fakeDt = 0.0025;
	if(attitudeSettings.AccelTau < 0.0001) {
		accel_alpha = 0;   // not trusting this to resolve to 0
		accel_filter_enabled = false;
	} else {
		accel_alpha = expf(-fakeDt  / attitudeSettings.AccelTau);
		accel_filter_enabled = true;
	}
	
	zero_during_arming = attitudeSettings.ZeroDuringArming == ATTITUDESETTINGS_ZERODURINGARMING_TRUE;
	bias_correct_gyro = attitudeSettings.BiasCorrectGyro == ATTITUDESETTINGS_BIASCORRECTGYRO_TRUE;
	
	accelbias[0] = attitudeSettings.AccelBias[ATTITUDESETTINGS_ACCELBIAS_X];
	accelbias[1] = attitudeSettings.AccelBias[ATTITUDESETTINGS_ACCELBIAS_Y];
	accelbias[2] = attitudeSettings.AccelBias[ATTITUDESETTINGS_ACCELBIAS_Z];
	
	gyro_correct_int[0] = attitudeSettings.InitialGyroBias[ATTITUDESETTINGS_INITIALGYROBIAS_X];
	gyro_correct_int[1] = attitudeSettings.InitialGyroBias[ATTITUDESETTINGS_INITIALGYROBIAS_Y];
	gyro_correct_int[2] = attitudeSettings.InitialGyroBias[ATTITUDESETTINGS_INITIALGYROBIAS_Z];
	
	// Indicates not to expend cycles on rotation
	if(attitudeSettings.BoardRotation[0] == 0 && attitudeSettings.BoardRotation[1] == 0 &&
	   attitudeSettings.BoardRotation[2] == 0) {
		rotate = 0;
		
		// Shouldn't be used but to be safe
		float rotationQuat[4] = {1,0,0,0};
		Quaternion2R(rotationQuat, R);
	} else {
		float rotationQuat[4];
		const float rpy[3] = {attitudeSettings.BoardRotation[ATTITUDESETTINGS_BOARDROTATION_ROLL] / 100.0f,
			attitudeSettings.BoardRotation[ATTITUDESETTINGS_BOARDROTATION_PITCH] / 100.0f,
			attitudeSettings.BoardRotation[ATTITUDESETTINGS_BOARDROTATION_YAW] / 100.0f};
		RPY2Quaternion(rpy, rotationQuat);
		Quaternion2R(rotationQuat, R);
		rotate = 1;
	}
	
	if (attitudeSettings.TrimFlight == ATTITUDESETTINGS_TRIMFLIGHT_START) {
		trim_accels[0] = 0;
		trim_accels[1] = 0;
		trim_accels[2] = 0;
		trim_samples = 0;
		trim_requested = true;
	} else if (attitudeSettings.TrimFlight == ATTITUDESETTINGS_TRIMFLIGHT_LOAD) {
		trim_requested = false;
		attitudeSettings.AccelBias[ATTITUDESETTINGS_ACCELBIAS_X] = trim_accels[0] / trim_samples;
		attitudeSettings.AccelBias[ATTITUDESETTINGS_ACCELBIAS_Y] = trim_accels[1] / trim_samples;
		// Z should average -grav
		attitudeSettings.AccelBias[ATTITUDESETTINGS_ACCELBIAS_Z] = trim_accels[2] / trim_samples + GRAV / ACCEL_SCALE;
		attitudeSettings.TrimFlight = ATTITUDESETTINGS_TRIMFLIGHT_NORMAL;
		AttitudeSettingsSet(&attitudeSettings);
	} else
		trim_requested = false;
}
/**
 * @}
 * @}
 */
