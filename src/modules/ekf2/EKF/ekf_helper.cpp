/****************************************************************************
 *
 *   Copyright (c) 2015 Estimation and Control Library (ECL). All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name ECL nor the names of its contributors may be
 *    used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 ****************************************************************************/

/**
 * @file ekf_helper.cpp
 * Definition of ekf helper functions.
 *
 * @author Roman Bast <bapstroman@gmail.com>
 *
 */

#include "ekf.h"

#include <mathlib/mathlib.h>
#include <cstdlib>

void Ekf::resetVelocityToGps(const gpsSample &gps_sample)
{
	_information_events.flags.reset_vel_to_gps = true;
	ECL_INFO("reset velocity to GPS");
	resetVelocityTo(gps_sample.vel);
	P.uncorrelateCovarianceSetVariance<3>(4, sq(gps_sample.sacc));
}

void Ekf::resetHorizontalVelocityToOpticalFlow()
{
	_information_events.flags.reset_vel_to_flow = true;
	ECL_INFO("reset velocity to flow");
	// constrain height above ground to be above minimum possible
	const float heightAboveGndEst = fmaxf((_terrain_vpos - _state.pos(2)), _params.rng_gnd_clearance);

	// calculate absolute distance from focal point to centre of frame assuming a flat earth
	const float range = heightAboveGndEst / _range_sensor.getCosTilt();

	if ((range - _params.rng_gnd_clearance) > 0.3f) {
		// we should have reliable OF measurements so
		// calculate X and Y body relative velocities from OF measurements
		Vector3f vel_optflow_body;
		vel_optflow_body(0) = - range * _flow_compensated_XY_rad(1) / _flow_sample_delayed.dt;
		vel_optflow_body(1) =   range * _flow_compensated_XY_rad(0) / _flow_sample_delayed.dt;
		vel_optflow_body(2) = 0.0f;

		// rotate from body to earth frame
		const Vector3f vel_optflow_earth = _R_to_earth * vel_optflow_body;

		resetHorizontalVelocityTo(Vector2f(vel_optflow_earth));

	} else {
		resetHorizontalVelocityTo(Vector2f{0.f, 0.f});
	}

	// reset the horizontal velocity variance using the optical flow noise variance
	P.uncorrelateCovarianceSetVariance<2>(4, sq(range) * calcOptFlowMeasVar());
}

void Ekf::resetVelocityToVision()
{
	_information_events.flags.reset_vel_to_vision = true;
	ECL_INFO("reset to vision velocity");
	resetVelocityTo(getVisionVelocityInEkfFrame());
	P.uncorrelateCovarianceSetVariance<3>(4, getVisionVelocityVarianceInEkfFrame());
}

void Ekf::resetHorizontalVelocityToZero()
{
	_information_events.flags.reset_vel_to_zero = true;
	ECL_INFO("reset velocity to zero");
	// Used when falling back to non-aiding mode of operation
	resetHorizontalVelocityTo(Vector2f{0.f, 0.f});
	P.uncorrelateCovarianceSetVariance<2>(4, 25.0f);
}

void Ekf::resetVelocityTo(const Vector3f &new_vel)
{
	resetHorizontalVelocityTo(Vector2f(new_vel));
	resetVerticalVelocityTo(new_vel(2));
}

void Ekf::resetHorizontalVelocityTo(const Vector2f &new_horz_vel)
{
	const Vector2f delta_horz_vel = new_horz_vel - Vector2f(_state.vel);
	_state.vel.xy() = new_horz_vel;

	for (uint8_t index = 0; index < _output_buffer.get_length(); index++) {
		_output_buffer[index].vel.xy() += delta_horz_vel;
	}

	_output_new.vel.xy() += delta_horz_vel;

	_state_reset_status.velNE_change = delta_horz_vel;
	_state_reset_status.velNE_counter++;

	// Reset the timout timer
	_time_last_hor_vel_fuse = _imu_sample_delayed.time_us;
}

void Ekf::resetVerticalVelocityTo(float new_vert_vel)
{
	const float delta_vert_vel = new_vert_vel - _state.vel(2);
	_state.vel(2) = new_vert_vel;

	for (uint8_t index = 0; index < _output_buffer.get_length(); index++) {
		_output_buffer[index].vel(2) += delta_vert_vel;
		_output_vert_buffer[index].vert_vel += delta_vert_vel;
	}

	_output_new.vel(2) += delta_vert_vel;
	_output_vert_new.vert_vel += delta_vert_vel;

	_state_reset_status.velD_change = delta_vert_vel;
	_state_reset_status.velD_counter++;

	// Reset the timout timer
	_time_last_ver_vel_fuse = _imu_sample_delayed.time_us;
}

void Ekf::resetHorizontalPositionToGps(const gpsSample &gps_sample)
{
	_information_events.flags.reset_pos_to_gps = true;
	ECL_INFO("reset position to GPS");
	resetHorizontalPositionTo(gps_sample.pos);
	P.uncorrelateCovarianceSetVariance<2>(7, sq(gps_sample.hacc));
}

void Ekf::resetHorizontalPositionToVision()
{
	_information_events.flags.reset_pos_to_vision = true;
	ECL_INFO("reset position to ev position");
	Vector3f _ev_pos = _ev_sample_delayed.pos;

	if (_params.fusion_mode & SensorFusionMask::ROTATE_EXT_VIS) {
		_ev_pos = _R_ev_to_ekf * _ev_sample_delayed.pos;
	}

	resetHorizontalPositionTo(Vector2f(_ev_pos));
	P.uncorrelateCovarianceSetVariance<2>(7, _ev_sample_delayed.posVar.slice<2, 1>(0, 0));

	// let the next odometry update know that the previous value of states cannot be used to calculate the change in position
	_hpos_prev_available = false;
}

void Ekf::resetHorizontalPositionToOpticalFlow()
{
	_information_events.flags.reset_pos_to_last_known = true;
	ECL_INFO("reset position to last known position");

	if (!_control_status.flags.in_air) {
		// we are likely starting OF for the first time so reset the horizontal position
		resetHorizontalPositionTo(Vector2f(0.f, 0.f));

	} else {
		resetHorizontalPositionTo(_last_known_pos.xy());
	}

	// estimate is relative to initial position in this mode, so we start with zero error.
	P.uncorrelateCovarianceSetVariance<2>(7, 0.0f);
}

void Ekf::resetHorizontalPositionToLastKnown()
{
	_information_events.flags.reset_pos_to_last_known = true;
	ECL_INFO("reset position to last known position");
	// Used when falling back to non-aiding mode of operation
	resetHorizontalPositionTo(_last_known_pos.xy());
	P.uncorrelateCovarianceSetVariance<2>(7, sq(_params.pos_noaid_noise));
}

void Ekf::resetHorizontalPositionTo(const Vector2f &new_horz_pos)
{
	const Vector2f delta_horz_pos{new_horz_pos - Vector2f{_state.pos}};
	_state.pos.xy() = new_horz_pos;

	for (uint8_t index = 0; index < _output_buffer.get_length(); index++) {
		_output_buffer[index].pos.xy() += delta_horz_pos;
	}

	_output_new.pos.xy() += delta_horz_pos;

	_state_reset_status.posNE_change = delta_horz_pos;
	_state_reset_status.posNE_counter++;

	// Reset the timout timer
	_time_last_hor_pos_fuse = _imu_sample_delayed.time_us;
}

bool Ekf::isHeightResetRequired() const
{
	// check if height is continuously failing because of accel errors
	const bool continuous_bad_accel_hgt = isTimedOut(_time_good_vert_accel, (uint64_t)_params.bad_acc_reset_delay_us);

	// check if height has been inertial deadreckoning for too long
	const bool hgt_fusion_timeout = isTimedOut(_time_last_hgt_fuse, _params.hgt_fusion_timeout_max);

	return (continuous_bad_accel_hgt || hgt_fusion_timeout);
}


void Ekf::resetVerticalPositionTo(const float new_vert_pos)
{
	const float old_vert_pos = _state.pos(2);
	_state.pos(2) = new_vert_pos;

	// store the reset amount and time to be published
	_state_reset_status.posD_change = new_vert_pos - old_vert_pos;
	_state_reset_status.posD_counter++;

	// apply the change in height / height rate to our newest height / height rate estimate
	// which have already been taken out from the output buffer
	_output_new.pos(2) += _state_reset_status.posD_change;

	// add the reset amount to the output observer buffered data
	for (uint8_t i = 0; i < _output_buffer.get_length(); i++) {
		_output_buffer[i].pos(2) += _state_reset_status.posD_change;
		_output_vert_buffer[i].vert_vel_integ += _state_reset_status.posD_change;
	}

	// add the reset amount to the output observer vertical position state
	_output_vert_new.vert_vel_integ = _state.pos(2);

	// Reset the timout timer
	_time_last_hgt_fuse = _imu_sample_delayed.time_us;
}

void Ekf::resetVerticalVelocityToGps(const gpsSample &gps_sample)
{
	resetVerticalVelocityTo(gps_sample.vel(2));

	// the state variance is the same as the observation
	P.uncorrelateCovarianceSetVariance<1>(6, sq(1.5f * gps_sample.sacc));
}

void Ekf::resetVerticalVelocityToEv(const extVisionSample &ev_sample)
{
	resetVerticalVelocityTo(ev_sample.vel(2));

	// the state variance is the same as the observation
	P.uncorrelateCovarianceSetVariance<1>(6, ev_sample.velVar(2));
}

void Ekf::resetVerticalVelocityToZero()
{
	// we don't know what the vertical velocity is, so set it to zero
	resetVerticalVelocityTo(0.0f);

	// Set the variance to a value large enough to allow the state to converge quickly
	// that does not destabilise the filter
	P.uncorrelateCovarianceSetVariance<1>(6, 10.0f);
}

// align output filter states to match EKF states at the fusion time horizon
void Ekf::alignOutputFilter()
{
	const outputSample &output_delayed = _output_buffer.get_oldest();

	// calculate the quaternion rotation delta from the EKF to output observer states at the EKF fusion time horizon
	Quatf q_delta{_state.quat_nominal * output_delayed.quat_nominal.inversed()};
	q_delta.normalize();

	// calculate the velocity and position deltas between the output and EKF at the EKF fusion time horizon
	const Vector3f vel_delta = _state.vel - output_delayed.vel;
	const Vector3f pos_delta = _state.pos - output_delayed.pos;

	// loop through the output filter state history and add the deltas
	for (uint8_t i = 0; i < _output_buffer.get_length(); i++) {
		_output_buffer[i].quat_nominal = q_delta * _output_buffer[i].quat_nominal;
		_output_buffer[i].quat_nominal.normalize();
		_output_buffer[i].vel += vel_delta;
		_output_buffer[i].pos += pos_delta;
	}

	_output_new = _output_buffer.get_newest();
}

// Do a forced re-alignment of the yaw angle to align with the horizontal velocity vector from the GPS.
// It is used to align the yaw angle after launch or takeoff for fixed wing vehicle only.
bool Ekf::realignYawGPS(const Vector3f &mag)
{
	const float gpsSpeed = sqrtf(sq(_gps_sample_delayed.vel(0)) + sq(_gps_sample_delayed.vel(1)));

	// Need at least 5 m/s of GPS horizontal speed and
	// ratio of velocity error to velocity < 0.15  for a reliable alignment
	const bool gps_yaw_alignment_possible = (gpsSpeed > 5.0f) && (_gps_sample_delayed.sacc < (0.15f * gpsSpeed));

	if (!gps_yaw_alignment_possible) {
		// attempt a normal alignment using the magnetometer
		return resetMagHeading();
	}

	// check for excessive horizontal GPS velocity innovations
	const float gps_vel_test_ratio = fmaxf(_aid_src_gnss_vel.test_ratio[0], _aid_src_gnss_vel.test_ratio[1]);
	const bool badVelInnov = (gps_vel_test_ratio > 1.0f) && _control_status.flags.gps;

	// calculate GPS course over ground angle
	const float gpsCOG = atan2f(_gps_sample_delayed.vel(1), _gps_sample_delayed.vel(0));

	// calculate course yaw angle
	const float ekfCOG = atan2f(_state.vel(1), _state.vel(0));

	// Check the EKF and GPS course over ground for consistency
	const float courseYawError = wrap_pi(gpsCOG - ekfCOG);

	// If the angles disagree and horizontal GPS velocity innovations are large or no previous yaw alignment, we declare the magnetic yaw as bad
	const bool badYawErr = fabsf(courseYawError) > 0.5f;
	const bool badMagYaw = (badYawErr && badVelInnov);

	if (badMagYaw) {
		_num_bad_flight_yaw_events++;
	}

	// correct yaw angle using GPS ground course if compass yaw bad or yaw is previously not aligned
	if (badMagYaw || !_control_status.flags.yaw_align) {
		_warning_events.flags.bad_yaw_using_gps_course = true;
		ECL_WARN("bad yaw, using GPS course");

		// declare the magnetometer as failed if a bad yaw has occurred more than once
		if (_control_status.flags.mag_aligned_in_flight && (_num_bad_flight_yaw_events >= 2)
		    && !_control_status.flags.mag_fault) {
			_warning_events.flags.stopping_mag_use = true;
			ECL_WARN("stopping mag use");
			_control_status.flags.mag_fault = true;
		}

		// calculate new yaw estimate
		float yaw_new;

		if (!_control_status.flags.mag_aligned_in_flight) {
			// This is our first flight alignment so we can assume that the recent change in velocity has occurred due to a
			// forward direction takeoff or launch and therefore the inertial and GPS ground course discrepancy is due to yaw error
			const float current_yaw = getEulerYaw(_R_to_earth);
			yaw_new = current_yaw + courseYawError;
			_control_status.flags.mag_aligned_in_flight = true;

		} else if (_control_status.flags.wind) {
			// we have previously aligned yaw in-flight and have wind estimates so set the yaw such that the vehicle nose is
			// aligned with the wind relative GPS velocity vector
			yaw_new = atan2f((_gps_sample_delayed.vel(1) - _state.wind_vel(1)),
					 (_gps_sample_delayed.vel(0) - _state.wind_vel(0)));

		} else {
			// we don't have wind estimates, so align yaw to the GPS velocity vector
			yaw_new = atan2f(_gps_sample_delayed.vel(1), _gps_sample_delayed.vel(0));

		}

		// use the combined EKF and GPS speed variance to calculate a rough estimate of the yaw error after alignment
		const float SpdErrorVariance = sq(_gps_sample_delayed.sacc) + P(4, 4) + P(5, 5);
		const float sineYawError = math::constrain(sqrtf(SpdErrorVariance) / gpsSpeed, 0.0f, 1.0f);
		const float yaw_variance_new = sq(asinf(sineYawError));

		// Apply updated yaw and yaw variance to states and covariances
		resetQuatStateYaw(yaw_new, yaw_variance_new);

		// Use the last magnetometer measurements to reset the field states
		_state.mag_B.zero();
		_state.mag_I = _R_to_earth * mag;

		resetMagCov();

		// record the start time for the magnetic field alignment
		_flt_mag_align_start_time = _imu_sample_delayed.time_us;

		// If heading was bad, then we also need to reset the velocity and position states
		if (badMagYaw) {
			resetVelocityToGps(_gps_sample_delayed);
			resetHorizontalPositionToGps(_gps_sample_delayed);
		}

		return true;

	} else {
		// align mag states only

		// calculate initial earth magnetic field states
		_state.mag_I = _R_to_earth * mag;

		resetMagCov();

		// record the start time for the magnetic field alignment
		_flt_mag_align_start_time = _imu_sample_delayed.time_us;

		return true;
	}
}

// Reset heading and magnetic field states
bool Ekf::resetMagHeading()
{
	// prevent a reset being performed more than once on the same frame
	if (_imu_sample_delayed.time_us == _flt_mag_align_start_time) {
		return true;
	}

	const Vector3f mag_init = _mag_lpf.getState();

	const bool mag_available = (_mag_counter != 0) && isNewestSampleRecent(_time_last_mag_buffer_push, 500'000)
				   && !magFieldStrengthDisturbed(mag_init);

	// low pass filtered mag required
	if (!mag_available) {
		return false;
	}

	const bool heading_required_for_navigation = _control_status.flags.gps;

	if ((_params.mag_fusion_type <= MagFuseType::MAG_3D) || ((_params.mag_fusion_type == MagFuseType::INDOOR) && heading_required_for_navigation)) {

		// rotate the magnetometer measurements into earth frame using a zero yaw angle
		const Dcmf R_to_earth = updateYawInRotMat(0.f, _R_to_earth);

		// the angle of the projection onto the horizontal gives the yaw angle
		const Vector3f mag_earth_pred = R_to_earth * mag_init;

		// calculate the observed yaw angle and yaw variance
		float yaw_new = -atan2f(mag_earth_pred(1), mag_earth_pred(0)) + getMagDeclination();
		float yaw_new_variance = sq(fmaxf(_params.mag_heading_noise, 1.e-2f));

		// update quaternion states and corresponding covarainces
		resetQuatStateYaw(yaw_new, yaw_new_variance);

		// set the earth magnetic field states using the updated rotation
		_state.mag_I = _R_to_earth * mag_init;

		resetMagCov();

		// record the time for the magnetic field alignment event
		_flt_mag_align_start_time = _imu_sample_delayed.time_us;

		return true;
	}

	return false;
}

bool Ekf::resetYawToEv()
{
	const float yaw_new = getEulerYaw(_ev_sample_delayed.quat);
	const float yaw_new_variance = fmaxf(_ev_sample_delayed.angVar, sq(1.0e-2f));

	resetQuatStateYaw(yaw_new, yaw_new_variance);
	_R_ev_to_ekf.setIdentity();

	return true;
}

// Return the magnetic declination in radians to be used by the alignment and fusion processing
float Ekf::getMagDeclination()
{
	// set source of magnetic declination for internal use
	if (_control_status.flags.mag_aligned_in_flight) {
		// Use value consistent with earth field state
		return atan2f(_state.mag_I(1), _state.mag_I(0));

	} else if (_params.mag_declination_source & GeoDeclinationMask::USE_GEO_DECL) {
		// use parameter value until GPS is available, then use value returned by geo library
		if (_NED_origin_initialised || PX4_ISFINITE(_mag_declination_gps)) {
			return _mag_declination_gps;

		} else {
			return math::radians(_params.mag_declination_deg);
		}

	} else {
		// always use the parameter value
		return math::radians(_params.mag_declination_deg);
	}
}

void Ekf::constrainStates()
{
	_state.quat_nominal = matrix::constrain(_state.quat_nominal, -1.0f, 1.0f);
	_state.vel = matrix::constrain(_state.vel, -1000.0f, 1000.0f);
	_state.pos = matrix::constrain(_state.pos, -1.e6f, 1.e6f);

	const float delta_ang_bias_limit = math::radians(20.f) * _dt_ekf_avg;
	_state.delta_ang_bias = matrix::constrain(_state.delta_ang_bias, -delta_ang_bias_limit, delta_ang_bias_limit);

	const float delta_vel_bias_limit = _params.acc_bias_lim * _dt_ekf_avg;
	_state.delta_vel_bias = matrix::constrain(_state.delta_vel_bias, -delta_vel_bias_limit, delta_vel_bias_limit);

	_state.mag_I = matrix::constrain(_state.mag_I, -1.0f, 1.0f);
	_state.mag_B = matrix::constrain(_state.mag_B, -0.5f, 0.5f);
	_state.wind_vel = matrix::constrain(_state.wind_vel, -100.0f, 100.0f);
}

float Ekf::compensateBaroForDynamicPressure(const float baro_alt_uncompensated) const
{
	// calculate static pressure error = Pmeas - Ptruth
	// model position error sensitivity as a body fixed ellipse with a different scale in the positive and
	// negative X and Y directions. Used to correct baro data for positional errors
	const matrix::Dcmf R_to_body(_output_new.quat_nominal.inversed());

	// Calculate airspeed in body frame
	const Vector3f velocity_earth = _output_new.vel - _vel_imu_rel_body_ned;

	const Vector3f wind_velocity_earth(_state.wind_vel(0), _state.wind_vel(1), 0.0f);

	const Vector3f airspeed_earth = velocity_earth - wind_velocity_earth;

	const Vector3f airspeed_body = R_to_body * airspeed_earth;

	const Vector3f K_pstatic_coef(airspeed_body(0) >= 0.0f ? _params.static_pressure_coef_xp :
				      _params.static_pressure_coef_xn,
				      airspeed_body(1) >= 0.0f ? _params.static_pressure_coef_yp : _params.static_pressure_coef_yn,
				      _params.static_pressure_coef_z);

	const Vector3f airspeed_squared = matrix::min(airspeed_body.emult(airspeed_body), sq(_params.max_correction_airspeed));

	const float pstatic_err = 0.5f * _air_density * (airspeed_squared.dot(K_pstatic_coef));

	// correct baro measurement using pressure error estimate and assuming sea level gravity
	return baro_alt_uncompensated + pstatic_err / (_air_density * CONSTANTS_ONE_G);
}

// calculate the earth rotation vector
Vector3f Ekf::calcEarthRateNED(float lat_rad) const
{
	return Vector3f(CONSTANTS_EARTH_SPIN_RATE * cosf(lat_rad),
			0.0f,
			-CONSTANTS_EARTH_SPIN_RATE * sinf(lat_rad));
}

void Ekf::getGpsVelPosInnov(float hvel[2], float &vvel, float hpos[2],  float &vpos) const
{
	hvel[0] = _aid_src_gnss_vel.innovation[0];
	hvel[1] = _aid_src_gnss_vel.innovation[1];
	vvel    = _aid_src_gnss_vel.innovation[2];

	hpos[0] = _aid_src_gnss_pos.innovation[0];
	hpos[1] = _aid_src_gnss_pos.innovation[1];
	vpos    = _aid_src_gnss_pos.innovation[2];
}

void Ekf::getGpsVelPosInnovVar(float hvel[2], float &vvel, float hpos[2], float &vpos)  const
{
	hvel[0] = _aid_src_gnss_vel.innovation_variance[0];
	hvel[1] = _aid_src_gnss_vel.innovation_variance[1];
	vvel    = _aid_src_gnss_vel.innovation_variance[2];

	hpos[0] = _aid_src_gnss_pos.innovation_variance[0];
	hpos[1] = _aid_src_gnss_pos.innovation_variance[1];
	vpos    = _aid_src_gnss_pos.innovation_variance[2];
}

void Ekf::getGpsVelPosInnovRatio(float &hvel, float &vvel, float &hpos, float &vpos) const
{
	hvel = fmaxf(_aid_src_gnss_vel.test_ratio[0], _aid_src_gnss_vel.test_ratio[1]);
	vvel = _aid_src_gnss_vel.test_ratio[2];

	hpos = fmaxf(_aid_src_gnss_pos.test_ratio[0], _aid_src_gnss_pos.test_ratio[1]);
	vpos = _aid_src_gnss_pos.test_ratio[2];
}

void Ekf::getEvVelPosInnov(float hvel[2], float &vvel, float hpos[2], float &vpos) const
{
	hvel[0] = _ev_vel_innov(0);
	hvel[1] = _ev_vel_innov(1);
	vvel    = _ev_vel_innov(2);
	hpos[0] = _ev_pos_innov(0);
	hpos[1] = _ev_pos_innov(1);
	vpos    = _ev_pos_innov(2);
}

void Ekf::getEvVelPosInnovVar(float hvel[2], float &vvel, float hpos[2], float &vpos) const
{
	hvel[0] = _ev_vel_innov_var(0);
	hvel[1] = _ev_vel_innov_var(1);
	vvel    = _ev_vel_innov_var(2);
	hpos[0] = _ev_pos_innov_var(0);
	hpos[1] = _ev_pos_innov_var(1);
	vpos    = _ev_pos_innov_var(2);
}

void Ekf::getEvVelPosInnovRatio(float &hvel, float &vvel, float &hpos, float &vpos) const
{
	hvel = _ev_vel_test_ratio(0);
	vvel = _ev_vel_test_ratio(1);
	hpos = _ev_pos_test_ratio(0);
	vpos = _ev_pos_test_ratio(1);
}

void Ekf::getAuxVelInnov(float aux_vel_innov[2]) const
{
	aux_vel_innov[0] = _aid_src_aux_vel.innovation[0];
	aux_vel_innov[1] = _aid_src_aux_vel.innovation[1];
}

void Ekf::getAuxVelInnovVar(float aux_vel_innov_var[2]) const
{
	aux_vel_innov_var[0] = _aid_src_aux_vel.innovation_variance[0];
	aux_vel_innov_var[1] = _aid_src_aux_vel.innovation_variance[1];
}

// get the state vector at the delayed time horizon
matrix::Vector<float, 24> Ekf::getStateAtFusionHorizonAsVector() const
{
	matrix::Vector<float, 24> state;
	state.slice<4, 1>(0, 0) = _state.quat_nominal;
	state.slice<3, 1>(4, 0) = _state.vel;
	state.slice<3, 1>(7, 0) = _state.pos;
	state.slice<3, 1>(10, 0) = _state.delta_ang_bias;
	state.slice<3, 1>(13, 0) = _state.delta_vel_bias;
	state.slice<3, 1>(16, 0) = _state.mag_I;
	state.slice<3, 1>(19, 0) = _state.mag_B;
	state.slice<2, 1>(22, 0) = _state.wind_vel;
	return state;
}

bool Ekf::getEkfGlobalOrigin(uint64_t &origin_time, double &latitude, double &longitude, float &origin_alt) const
{
	origin_time = _pos_ref.getProjectionReferenceTimestamp();
	latitude = _pos_ref.getProjectionReferenceLat();
	longitude = _pos_ref.getProjectionReferenceLon();
	origin_alt  = getEkfGlobalOriginAltitude();
	return _NED_origin_initialised;
}

bool Ekf::setEkfGlobalOrigin(const double latitude, const double longitude, const float altitude)
{
	// sanity check valid latitude/longitude and altitude anywhere between the Mariana Trench and edge of Space
	if (PX4_ISFINITE(latitude) && (abs(latitude) <= 90)
	&& PX4_ISFINITE(longitude) && (abs(longitude) <= 180)
	&& PX4_ISFINITE(altitude) && (altitude > -12'000.f) && (altitude < 100'000.f)
	) {
		bool current_pos_available = false;
		double current_lat = static_cast<double>(NAN);
		double current_lon = static_cast<double>(NAN);

		// if we are already doing aiding, correct for the change in position since the EKF started navigating
		if (_pos_ref.isInitialized() && isHorizontalAidingActive()) {
			_pos_ref.reproject(_state.pos(0), _state.pos(1), current_lat, current_lon);
			current_pos_available = true;
		}

		const float gps_alt_ref_prev = getEkfGlobalOriginAltitude();

		// reinitialize map projection to latitude, longitude, altitude, and reset position
		_pos_ref.initReference(latitude, longitude, _imu_sample_delayed.time_us);
		_gps_alt_ref = altitude;

		// minimum change in position or height that triggers a reset
		static constexpr float MIN_RESET_DIST_M = 0.01f;

		if (current_pos_available) {
			// reset horizontal position
			Vector2f position = _pos_ref.project(current_lat, current_lon);

			if (Vector2f(position - Vector2f(_state.pos)).longerThan(MIN_RESET_DIST_M)) {
				resetHorizontalPositionTo(position);
			}
		}

		// reset vertical position (if there's any change)
		if (fabsf(altitude - gps_alt_ref_prev) > MIN_RESET_DIST_M) {
			// determine current z
			float current_alt = -_state.pos(2) + gps_alt_ref_prev;

			resetVerticalPositionTo(_gps_alt_ref - current_alt);

			_baro_b_est.setBias(_baro_b_est.getBias() + _state_reset_status.posD_change);
			_rng_hgt_b_est.setBias(_rng_hgt_b_est.getBias() + _state_reset_status.posD_change);
			_ev_hgt_b_est.setBias(_ev_hgt_b_est.getBias() - _state_reset_status.posD_change);
		}

		return true;
	}

	return false;
}

// get the 1-sigma horizontal and vertical position uncertainty of the ekf WGS-84 position
void Ekf::get_ekf_gpos_accuracy(float *ekf_eph, float *ekf_epv) const
{
	// report absolute accuracy taking into account the uncertainty in location of the origin
	// If not aiding, return 0 for horizontal position estimate as no estimate is available
	// TODO - allow for baro drift in vertical position error
	float hpos_err = sqrtf(P(7, 7) + P(8, 8) + sq(_gps_origin_eph));

	// If we are dead-reckoning, use the innovations as a conservative alternate measure of the horizontal position error
	// The reason is that complete rejection of measurements is often caused by heading misalignment or inertial sensing errors
	// and using state variances for accuracy reporting is overly optimistic in these situations
	if (_control_status.flags.inertial_dead_reckoning) {
		if (_control_status.flags.gps) {
			hpos_err = math::max(hpos_err, Vector2f(_aid_src_gnss_pos.innovation).norm());
		}

		if (_control_status.flags.ev_pos) {
			hpos_err = math::max(hpos_err, sqrtf(sq(_ev_pos_innov(0)) + sq(_ev_pos_innov(1))));
		}
	}

	*ekf_eph = hpos_err;
	*ekf_epv = sqrtf(P(9, 9) + sq(_gps_origin_epv));
}

// get the 1-sigma horizontal and vertical position uncertainty of the ekf local position
void Ekf::get_ekf_lpos_accuracy(float *ekf_eph, float *ekf_epv) const
{
	// TODO - allow for baro drift in vertical position error
	float hpos_err = sqrtf(P(7, 7) + P(8, 8));

	// If we are dead-reckoning for too long, use the innovations as a conservative alternate measure of the horizontal position error
	// The reason is that complete rejection of measurements is often caused by heading misalignment or inertial sensing errors
	// and using state variances for accuracy reporting is overly optimistic in these situations
	if (_deadreckon_time_exceeded && _control_status.flags.gps) {
		hpos_err = math::max(hpos_err, Vector2f(_aid_src_gnss_pos.innovation).norm());
	}

	*ekf_eph = hpos_err;
	*ekf_epv = sqrtf(P(9, 9));
}

// get the 1-sigma horizontal and vertical velocity uncertainty
void Ekf::get_ekf_vel_accuracy(float *ekf_evh, float *ekf_evv) const
{
	float hvel_err = sqrtf(P(4, 4) + P(5, 5));

	// If we are dead-reckoning for too long, use the innovations as a conservative alternate measure of the horizontal velocity error
	// The reason is that complete rejection of measurements is often caused by heading misalignment or inertial sensing errors
	// and using state variances for accuracy reporting is overly optimistic in these situations
	if (_deadreckon_time_exceeded) {
		float vel_err_conservative = 0.0f;

		if (_control_status.flags.opt_flow) {
			float gndclearance = math::max(_params.rng_gnd_clearance, 0.1f);
			vel_err_conservative = math::max((_terrain_vpos - _state.pos(2)), gndclearance) * _flow_innov.norm();
		}

		if (_control_status.flags.gps) {
			vel_err_conservative = math::max(vel_err_conservative, Vector2f(_aid_src_gnss_pos.innovation).norm());

		} else if (_control_status.flags.ev_pos) {
			vel_err_conservative = math::max(vel_err_conservative, sqrtf(sq(_ev_pos_innov(0)) + sq(_ev_pos_innov(1))));
		}

		if (_control_status.flags.ev_vel) {
			vel_err_conservative = math::max(vel_err_conservative, sqrtf(sq(_ev_vel_innov(0)) + sq(_ev_vel_innov(1))));
		}

		hvel_err = math::max(hvel_err, vel_err_conservative);
	}

	*ekf_evh = hvel_err;
	*ekf_evv = sqrtf(P(6, 6));
}

/*
Returns the following vehicle control limits required by the estimator to keep within sensor limitations.
vxy_max : Maximum ground relative horizontal speed (meters/sec). NaN when limiting is not needed.
vz_max : Maximum ground relative vertical speed (meters/sec). NaN when limiting is not needed.
hagl_min : Minimum height above ground (meters). NaN when limiting is not needed.
hagl_max : Maximum height above ground (meters). NaN when limiting is not needed.
*/
void Ekf::get_ekf_ctrl_limits(float *vxy_max, float *vz_max, float *hagl_min, float *hagl_max) const
{
	// Do not require limiting by default
	*vxy_max = NAN;
	*vz_max = NAN;
	*hagl_min = NAN;
	*hagl_max = NAN;

	// Calculate range finder limits
	const float rangefinder_hagl_min = _range_sensor.getValidMinVal();

	// Allow use of 75% of rangefinder maximum range to allow for angular motion
	const float rangefinder_hagl_max = 0.75f * _range_sensor.getValidMaxVal();

	// TODO : calculate visual odometry limits
	const bool relying_on_rangefinder = isOnlyActiveSourceOfVerticalPositionAiding(_control_status.flags.rng_hgt);
	const bool relying_on_optical_flow = isOnlyActiveSourceOfHorizontalAiding(_control_status.flags.opt_flow);

	// Keep within range sensor limit when using rangefinder as primary height source
	if (relying_on_rangefinder) {
		*hagl_min = rangefinder_hagl_min;
		*hagl_max = rangefinder_hagl_max;
	}

	// Keep within flow AND range sensor limits when exclusively using optical flow
	if (relying_on_optical_flow) {
		// Calculate optical flow limits
		const float flow_hagl_min = fmaxf(rangefinder_hagl_min, _flow_min_distance);
		const float flow_hagl_max = fminf(rangefinder_hagl_max, _flow_max_distance);

		const float flow_constrained_height = math::constrain(_terrain_vpos - _state.pos(2), flow_hagl_min, flow_hagl_max);

		// Allow ground relative velocity to use 50% of available flow sensor range to allow for angular motion
		const float flow_vxy_max = 0.5f * _flow_max_rate * flow_constrained_height;

		*vxy_max = flow_vxy_max;
		*hagl_min = flow_hagl_min;
		*hagl_max = flow_hagl_max;
	}
}

void Ekf::resetImuBias()
{
	resetGyroBias();
	resetAccelBias();
}

void Ekf::resetGyroBias()
{
	// Zero the delta angle and delta velocity bias states
	_state.delta_ang_bias.zero();

	// Zero the corresponding covariances and set
	// variances to the values use for initial alignment
	P.uncorrelateCovarianceSetVariance<3>(10, sq(_params.switch_on_gyro_bias * _dt_ekf_avg));
}

void Ekf::resetAccelBias()
{
	// Zero the delta angle and delta velocity bias states
	_state.delta_vel_bias.zero();

	// Zero the corresponding covariances and set
	// variances to the values use for initial alignment
	P.uncorrelateCovarianceSetVariance<3>(13, sq(_params.switch_on_accel_bias * _dt_ekf_avg));

	// Set previous frame values
	_prev_dvel_bias_var = P.slice<3, 3>(13, 13).diag();
}

void Ekf::resetMagBiasAndYaw()
{
	// Zero the magnetometer bias states
	_state.mag_B.zero();

	// Zero the corresponding covariances and set
	// variances to the values use for initial alignment
	P.uncorrelateCovarianceSetVariance<3>(19, sq(_params.mag_noise));

	// reset any saved covariance data for re-use when auto-switching between heading and 3-axis fusion
	_saved_mag_bf_variance.zero();

	if (_control_status.flags.mag_hdg || _control_status.flags.mag_3D) {
		_mag_yaw_reset_req = true;
	}

	_control_status.flags.mag_fault = false;

	_mag_counter = 0;
}

// get EKF innovation consistency check status information comprising of:
// status - a bitmask integer containing the pass/fail status for each EKF measurement innovation consistency check
// Innovation Test Ratios - these are the ratio of the innovation to the acceptance threshold.
// A value > 1 indicates that the sensor measurement has exceeded the maximum acceptable level and has been rejected by the EKF
// Where a measurement type is a vector quantity, eg magnetometer, GPS position, etc, the maximum value is returned.
void Ekf::get_innovation_test_status(uint16_t &status, float &mag, float &vel, float &pos, float &hgt, float &tas,
				     float &hagl, float &beta) const
{
	// return the integer bitmask containing the consistency check pass/fail status
	status = _innov_check_fail_status.value;

	// return the largest magnetometer innovation test ratio
	if (_control_status.flags.mag_hdg) {
		mag = sqrtf(_aid_src_mag_heading.test_ratio);

	} else if (_control_status.flags.mag_3D) {
		mag = sqrtf(Vector3f(_aid_src_mag.test_ratio).max());

	} else if (_control_status.flags.gps_yaw) {
		mag = sqrtf(_aid_src_gnss_yaw.test_ratio);
	} else {
		mag = NAN;
	}

	// return the largest velocity and position innovation test ratio
	vel = NAN;
	pos = NAN;

	if (_control_status.flags.gps) {
		float gps_vel = sqrtf(Vector3f(_aid_src_gnss_vel.test_ratio).max());
		vel = math::max(gps_vel, FLT_MIN);

		float gps_pos = sqrtf(Vector2f(_aid_src_gnss_pos.test_ratio).max());
		pos = math::max(gps_pos, FLT_MIN);
	}

	if (_control_status.flags.ev_vel) {
		float ev_vel = sqrtf(math::max(_ev_vel_test_ratio(0), _ev_vel_test_ratio(1)));
		vel = math::max(vel, ev_vel, FLT_MIN);
	}

	if (_control_status.flags.ev_pos) {
		float ev_pos = sqrtf(_ev_pos_test_ratio(0));
		pos = math::max(pos, ev_pos, FLT_MIN);
	}

	if (isOnlyActiveSourceOfHorizontalAiding(_control_status.flags.opt_flow)) {
		float of_vel = sqrtf(_optflow_test_ratio);
		vel = math::max(of_vel, FLT_MIN);
	}

	// return the combined vertical position innovation test ratio
	float hgt_sum = 0.f;

	if (_control_status.flags.baro_hgt) {
		hgt_sum += sqrtf(_aid_src_baro_hgt.test_ratio);
	}

	if (_control_status.flags.gps_hgt) {
		hgt_sum += sqrtf(_aid_src_gnss_pos.test_ratio[2]);
	}

	if (_control_status.flags.rng_hgt) {
		hgt_sum += sqrtf(_aid_src_rng_hgt.test_ratio);
	}

	if (_control_status.flags.ev_hgt) {
		hgt_sum += sqrtf(_ev_pos_test_ratio(1));
	}

	const int n_hgt_sources = getNumberOfActiveVerticalPositionAidingSources();

	if (n_hgt_sources > 0) {
		hgt = math::max(hgt_sum / static_cast<float>(n_hgt_sources), FLT_MIN);

	} else {
		hgt = NAN;
	}

	// return the airspeed fusion innovation test ratio
	tas = sqrtf(_aid_src_airspeed.test_ratio);

	// return the terrain height innovation test ratio
	hagl = sqrtf(_hagl_test_ratio);

	// return the synthetic sideslip innovation test ratio
	beta = sqrtf(_beta_test_ratio);
}

// return a bitmask integer that describes which state estimates are valid
void Ekf::get_ekf_soln_status(uint16_t *status) const
{
	ekf_solution_status_u soln_status;
	// TODO: Is this accurate enough?
	soln_status.flags.attitude = _control_status.flags.tilt_align && _control_status.flags.yaw_align && (_fault_status.value == 0);
	soln_status.flags.velocity_horiz = (isHorizontalAidingActive() || (_control_status.flags.fuse_beta && _control_status.flags.fuse_aspd)) && (_fault_status.value == 0);
	soln_status.flags.velocity_vert = (_control_status.flags.baro_hgt || _control_status.flags.ev_hgt || _control_status.flags.gps_hgt || _control_status.flags.rng_hgt) && (_fault_status.value == 0);
	soln_status.flags.pos_horiz_rel = (_control_status.flags.gps || _control_status.flags.ev_pos || _control_status.flags.opt_flow) && (_fault_status.value == 0);
	soln_status.flags.pos_horiz_abs = (_control_status.flags.gps || _control_status.flags.ev_pos) && (_fault_status.value == 0);
	soln_status.flags.pos_vert_abs = soln_status.flags.velocity_vert;
	soln_status.flags.pos_vert_agl = isTerrainEstimateValid();
	soln_status.flags.const_pos_mode = !soln_status.flags.velocity_horiz;
	soln_status.flags.pred_pos_horiz_rel = soln_status.flags.pos_horiz_rel;
	soln_status.flags.pred_pos_horiz_abs = soln_status.flags.pos_horiz_abs;

	bool mag_innov_good = true;

	if (_control_status.flags.mag_hdg) {
		if (_aid_src_mag_heading.test_ratio < 1.f) {
			mag_innov_good = false;
		}

	} else if (_control_status.flags.mag_3D) {
		if (Vector3f(_aid_src_mag.test_ratio).max() < 1.f) {
			mag_innov_good = false;
		}
	}

	const bool gps_vel_innov_bad = Vector3f(_aid_src_gnss_vel.test_ratio).max() > 1.f;
	const bool gps_pos_innov_bad = Vector2f(_aid_src_gnss_pos.test_ratio).max() > 1.f;

	soln_status.flags.gps_glitch = (gps_vel_innov_bad || gps_pos_innov_bad) && mag_innov_good;
	soln_status.flags.accel_error = _fault_status.flags.bad_acc_vertical;
	*status = soln_status.value;
}

void Ekf::fuse(const Vector24f &K, float innovation)
{
	_state.quat_nominal -= K.slice<4, 1>(0, 0) * innovation;
	_state.quat_nominal.normalize();
	_state.vel -= K.slice<3, 1>(4, 0) * innovation;
	_state.pos -= K.slice<3, 1>(7, 0) * innovation;
	_state.delta_ang_bias -= K.slice<3, 1>(10, 0) * innovation;
	_state.delta_vel_bias -= K.slice<3, 1>(13, 0) * innovation;
	_state.mag_I -= K.slice<3, 1>(16, 0) * innovation;
	_state.mag_B -= K.slice<3, 1>(19, 0) * innovation;
	_state.wind_vel -= K.slice<2, 1>(22, 0) * innovation;
}

void Ekf::uncorrelateQuatFromOtherStates()
{
	P.slice<_k_num_states - 4, 4>(4, 0) = 0.f;
	P.slice<4, _k_num_states - 4>(0, 4) = 0.f;
}

// return true if we are totally reliant on inertial dead-reckoning for position
void Ekf::update_deadreckoning_status()
{
	const bool velPosAiding = (_control_status.flags.gps || _control_status.flags.ev_pos || _control_status.flags.ev_vel)
				  && (isRecent(_time_last_hor_pos_fuse, _params.no_aid_timeout_max)
				      || isRecent(_time_last_hor_vel_fuse, _params.no_aid_timeout_max));

	const bool optFlowAiding = _control_status.flags.opt_flow && isRecent(_time_last_of_fuse, _params.no_aid_timeout_max);

	const bool airDataAiding = _control_status.flags.wind &&
				   isRecent(_aid_src_airspeed.time_last_fuse, _params.no_aid_timeout_max) &&
				   isRecent(_time_last_beta_fuse, _params.no_aid_timeout_max);

	_control_status.flags.wind_dead_reckoning = !velPosAiding && !optFlowAiding && airDataAiding;
	_control_status.flags.inertial_dead_reckoning = !velPosAiding && !optFlowAiding && !airDataAiding;

	if (!_control_status.flags.inertial_dead_reckoning) {
		if (_imu_sample_delayed.time_us > _params.no_aid_timeout_max) {
			_time_last_aiding = _imu_sample_delayed.time_us - _params.no_aid_timeout_max;
		}
	}

	// report if we have been deadreckoning for too long, initial state is deadreckoning until aiding is present
	bool deadreckon_time_exceeded = (_time_last_aiding == 0)
				    || isTimedOut(_time_last_aiding, (uint64_t)_params.valid_timeout_max);

	if (!_deadreckon_time_exceeded && deadreckon_time_exceeded) {
		// deadreckon time now exceeded
		ECL_WARN("dead reckon time exceeded");
	}

	_deadreckon_time_exceeded = deadreckon_time_exceeded;
}

// calculate the variances for the rotation vector equivalent
Vector3f Ekf::calcRotVecVariances()
{
	Vector3f rot_var_vec;
	float q0, q1, q2, q3;

	if (_state.quat_nominal(0) >= 0.0f) {
		q0 = _state.quat_nominal(0);
		q1 = _state.quat_nominal(1);
		q2 = _state.quat_nominal(2);
		q3 = _state.quat_nominal(3);

	} else {
		q0 = -_state.quat_nominal(0);
		q1 = -_state.quat_nominal(1);
		q2 = -_state.quat_nominal(2);
		q3 = -_state.quat_nominal(3);
	}
	float t2 = q0*q0;
	float t3 = acosf(q0);
	float t4 = -t2+1.0f;
	float t5 = t2-1.0f;
	if ((t4 > 1e-9f) && (t5 < -1e-9f)) {
		float t6 = 1.0f/t5;
		float t7 = q1*t6*2.0f;
		float t8 = 1.0f/powf(t4,1.5f);
		float t9 = q0*q1*t3*t8*2.0f;
		float t10 = t7+t9;
		float t11 = 1.0f/sqrtf(t4);
		float t12 = q2*t6*2.0f;
		float t13 = q0*q2*t3*t8*2.0f;
		float t14 = t12+t13;
		float t15 = q3*t6*2.0f;
		float t16 = q0*q3*t3*t8*2.0f;
		float t17 = t15+t16;
		rot_var_vec(0) = t10*(P(0,0)*t10+P(1,0)*t3*t11*2.0f)+t3*t11*(P(0,1)*t10+P(1,1)*t3*t11*2.0f)*2.0f;
		rot_var_vec(1) = t14*(P(0,0)*t14+P(2,0)*t3*t11*2.0f)+t3*t11*(P(0,2)*t14+P(2,2)*t3*t11*2.0f)*2.0f;
		rot_var_vec(2) = t17*(P(0,0)*t17+P(3,0)*t3*t11*2.0f)+t3*t11*(P(0,3)*t17+P(3,3)*t3*t11*2.0f)*2.0f;
	} else {
		rot_var_vec = 4.0f * P.slice<3,3>(1,1).diag();
	}

	return rot_var_vec;
}

// initialise the quaternion covariances using rotation vector variances
// do not call before quaternion states are initialised
void Ekf::initialiseQuatCovariances(Vector3f &rot_vec_var)
{
	// calculate an equivalent rotation vector from the quaternion
	float q0,q1,q2,q3;
	if (_state.quat_nominal(0) >= 0.0f) {
		q0 = _state.quat_nominal(0);
		q1 = _state.quat_nominal(1);
		q2 = _state.quat_nominal(2);
		q3 = _state.quat_nominal(3);

	} else {
		q0 = -_state.quat_nominal(0);
		q1 = -_state.quat_nominal(1);
		q2 = -_state.quat_nominal(2);
		q3 = -_state.quat_nominal(3);
	}
	float delta = 2.0f*acosf(q0);
	float scaler = (delta/sinf(delta*0.5f));
	float rotX = scaler*q1;
	float rotY = scaler*q2;
	float rotZ = scaler*q3;

	// autocode generated using matlab symbolic toolbox
	float t2 = rotX*rotX;
	float t4 = rotY*rotY;
	float t5 = rotZ*rotZ;
	float t6 = t2+t4+t5;
	if (t6 > 1e-9f) {
		float t7 = sqrtf(t6);
		float t8 = t7*0.5f;
		float t3 = sinf(t8);
		float t9 = t3*t3;
		float t10 = 1.0f/t6;
		float t11 = 1.0f/sqrtf(t6);
		float t12 = cosf(t8);
		float t13 = 1.0f/powf(t6,1.5f);
		float t14 = t3*t11;
		float t15 = rotX*rotY*t3*t13;
		float t16 = rotX*rotZ*t3*t13;
		float t17 = rotY*rotZ*t3*t13;
		float t18 = t2*t10*t12*0.5f;
		float t27 = t2*t3*t13;
		float t19 = t14+t18-t27;
		float t23 = rotX*rotY*t10*t12*0.5f;
		float t28 = t15-t23;
		float t20 = rotY*rot_vec_var(1)*t3*t11*t28*0.5f;
		float t25 = rotX*rotZ*t10*t12*0.5f;
		float t31 = t16-t25;
		float t21 = rotZ*rot_vec_var(2)*t3*t11*t31*0.5f;
		float t22 = t20+t21-rotX*rot_vec_var(0)*t3*t11*t19*0.5f;
		float t24 = t15-t23;
		float t26 = t16-t25;
		float t29 = t4*t10*t12*0.5f;
		float t34 = t3*t4*t13;
		float t30 = t14+t29-t34;
		float t32 = t5*t10*t12*0.5f;
		float t40 = t3*t5*t13;
		float t33 = t14+t32-t40;
		float t36 = rotY*rotZ*t10*t12*0.5f;
		float t39 = t17-t36;
		float t35 = rotZ*rot_vec_var(2)*t3*t11*t39*0.5f;
		float t37 = t15-t23;
		float t38 = t17-t36;
		float t41 = rot_vec_var(0)*(t15-t23)*(t16-t25);
		float t42 = t41-rot_vec_var(1)*t30*t39-rot_vec_var(2)*t33*t39;
		float t43 = t16-t25;
		float t44 = t17-t36;

		// zero all the quaternion covariances
		P.uncorrelateCovarianceSetVariance<2>(0, 0.0f);
		P.uncorrelateCovarianceSetVariance<2>(2, 0.0f);


		// Update the quaternion internal covariances using auto-code generated using matlab symbolic toolbox
		P(0,0) = rot_vec_var(0)*t2*t9*t10*0.25f+rot_vec_var(1)*t4*t9*t10*0.25f+rot_vec_var(2)*t5*t9*t10*0.25f;
		P(0,1) = t22;
		P(0,2) = t35+rotX*rot_vec_var(0)*t3*t11*(t15-rotX*rotY*t10*t12*0.5f)*0.5f-rotY*rot_vec_var(1)*t3*t11*t30*0.5f;
		P(0,3) = rotX*rot_vec_var(0)*t3*t11*(t16-rotX*rotZ*t10*t12*0.5f)*0.5f+rotY*rot_vec_var(1)*t3*t11*(t17-rotY*rotZ*t10*t12*0.5f)*0.5f-rotZ*rot_vec_var(2)*t3*t11*t33*0.5f;
		P(1,0) = t22;
		P(1,1) = rot_vec_var(0)*(t19*t19)+rot_vec_var(1)*(t24*t24)+rot_vec_var(2)*(t26*t26);
		P(1,2) = rot_vec_var(2)*(t16-t25)*(t17-rotY*rotZ*t10*t12*0.5f)-rot_vec_var(0)*t19*t28-rot_vec_var(1)*t28*t30;
		P(1,3) = rot_vec_var(1)*(t15-t23)*(t17-rotY*rotZ*t10*t12*0.5f)-rot_vec_var(0)*t19*t31-rot_vec_var(2)*t31*t33;
		P(2,0) = t35-rotY*rot_vec_var(1)*t3*t11*t30*0.5f+rotX*rot_vec_var(0)*t3*t11*(t15-t23)*0.5f;
		P(2,1) = rot_vec_var(2)*(t16-t25)*(t17-t36)-rot_vec_var(0)*t19*t28-rot_vec_var(1)*t28*t30;
		P(2,2) = rot_vec_var(1)*(t30*t30)+rot_vec_var(0)*(t37*t37)+rot_vec_var(2)*(t38*t38);
		P(2,3) = t42;
		P(3,0) = rotZ*rot_vec_var(2)*t3*t11*t33*(-0.5f)+rotX*rot_vec_var(0)*t3*t11*(t16-t25)*0.5f+rotY*rot_vec_var(1)*t3*t11*(t17-t36)*0.5f;
		P(3,1) = rot_vec_var(1)*(t15-t23)*(t17-t36)-rot_vec_var(0)*t19*t31-rot_vec_var(2)*t31*t33;
		P(3,2) = t42;
		P(3,3) = rot_vec_var(2)*(t33*t33)+rot_vec_var(0)*(t43*t43)+rot_vec_var(1)*(t44*t44);

	} else {
		// the equations are badly conditioned so use a small angle approximation
		P.uncorrelateCovarianceSetVariance<1>(0, 0.0f);
		P.uncorrelateCovarianceSetVariance<3>(1, 0.25f * rot_vec_var);
	}
}

void Ekf::stopMagFusion()
{
	stopMag3DFusion();
	stopMagHdgFusion();
	clearMagCov();
}

void Ekf::stopMag3DFusion()
{
	// save covariance data for re-use if currently doing 3-axis fusion
	if (_control_status.flags.mag_3D) {
		saveMagCovData();

		_control_status.flags.mag_3D = false;
		_control_status.flags.mag_dec = false;

		_fault_status.flags.bad_mag_x = false;
		_fault_status.flags.bad_mag_y = false;
		_fault_status.flags.bad_mag_z = false;

		_fault_status.flags.bad_mag_decl = false;
	}
}

void Ekf::stopMagHdgFusion()
{
	if (_control_status.flags.mag_hdg) {
		_control_status.flags.mag_hdg = false;

		_fault_status.flags.bad_hdg = false;
	}
}

void Ekf::startMagHdgFusion()
{
	if (!_control_status.flags.mag_hdg) {
		stopMag3DFusion();
		ECL_INFO("starting mag heading fusion");
		_control_status.flags.mag_hdg = true;
	}
}

void Ekf::startMag3DFusion()
{
	if (!_control_status.flags.mag_3D) {

		stopMagHdgFusion();

		zeroMagCov();
		loadMagCovData();
		_control_status.flags.mag_3D = true;
	}
}

float Ekf::getGpsHeightVariance(const gpsSample &gps_sample)
{
	// observation variance - receiver defined and parameter limited
	// use 1.5 as a typical ratio of vacc/hacc
	const float lower_limit = fmaxf(1.5f * _params.gps_pos_noise, 0.01f);
	const float upper_limit = fmaxf(1.5f * _params.pos_noaid_noise, lower_limit);
	const float gps_alt_var = sq(math::constrain(gps_sample.vacc, lower_limit, upper_limit));
	return gps_alt_var;
}

float Ekf::getRngHeightVariance() const
{
	const float dist_dependant_var = sq(_params.range_noise_scaler * _range_sensor.getDistBottom());
	const float var = sq(_params.range_noise) + dist_dependant_var;
	const float var_sat = fmaxf(var, 0.001f);
	return var_sat;
}

void Ekf::updateGroundEffect()
{
	if (_control_status.flags.in_air && !_control_status.flags.fixed_wing) {
		if (isTerrainEstimateValid()) {
			// automatically set ground effect if terrain is valid
			float height = _terrain_vpos - _state.pos(2);
			_control_status.flags.gnd_effect = (height < _params.gnd_effect_max_hgt);

		} else if (_control_status.flags.gnd_effect) {
			// Turn off ground effect compensation if it times out
			if (isTimedOut(_time_last_gnd_effect_on, GNDEFFECT_TIMEOUT)) {
				_control_status.flags.gnd_effect = false;
			}
		}

	} else {
		_control_status.flags.gnd_effect = false;
	}
}

Vector3f Ekf::getVisionVelocityInEkfFrame() const
{
	Vector3f vel;
	// correct velocity for offset relative to IMU
	const Vector3f pos_offset_body = _params.ev_pos_body - _params.imu_pos_body;
	const Vector3f vel_offset_body = _ang_rate_delayed_raw % pos_offset_body;

	// rotate measurement into correct earth frame if required
	switch (_ev_sample_delayed.vel_frame) {
	case VelocityFrame::BODY_FRAME_FRD:
		vel = _R_to_earth * (_ev_sample_delayed.vel - vel_offset_body);
		break;

	case VelocityFrame::LOCAL_FRAME_FRD:
		const Vector3f vel_offset_earth = _R_to_earth * vel_offset_body;

		if (_params.fusion_mode & SensorFusionMask::ROTATE_EXT_VIS) {
			vel = _R_ev_to_ekf * _ev_sample_delayed.vel - vel_offset_earth;

		} else {
			vel = _ev_sample_delayed.vel - vel_offset_earth;
		}

		break;
	}

	return vel;
}

Vector3f Ekf::getVisionVelocityVarianceInEkfFrame() const
{
	Matrix3f ev_vel_cov = matrix::diag(_ev_sample_delayed.velVar);

	// rotate measurement into correct earth frame if required
	switch (_ev_sample_delayed.vel_frame) {
	case VelocityFrame::BODY_FRAME_FRD:
		ev_vel_cov = _R_to_earth * ev_vel_cov * _R_to_earth.transpose();
		break;

	case VelocityFrame::LOCAL_FRAME_FRD:
		if (_params.fusion_mode & SensorFusionMask::ROTATE_EXT_VIS) {
			ev_vel_cov = _R_ev_to_ekf * ev_vel_cov * _R_ev_to_ekf.transpose();
		}

		break;
	}

	return ev_vel_cov.diag();
}

// update the rotation matrix which rotates EV measurements into the EKF's navigation frame
void Ekf::calcExtVisRotMat()
{
	// Calculate the quaternion delta that rotates from the EV to the EKF reference frame at the EKF fusion time horizon.
	const Quatf q_error((_state.quat_nominal * _ev_sample_delayed.quat.inversed()).normalized());
	_R_ev_to_ekf = Dcmf(q_error);
}

// Increase the yaw error variance of the quaternions
// Argument is additional yaw variance in rad**2
void Ekf::increaseQuatYawErrVariance(float yaw_variance)
{
	// See DeriveYawResetEquations.m for derivation which produces code fragments in C_code4.txt file
	// The auto-code was cleaned up and had terms multiplied by zero removed to give the following:

	// Intermediate variables
	float SG[3];
	SG[0] = sq(_state.quat_nominal(0)) - sq(_state.quat_nominal(1)) - sq(_state.quat_nominal(2)) + sq(_state.quat_nominal(3));
	SG[1] = 2*_state.quat_nominal(0)*_state.quat_nominal(2) - 2*_state.quat_nominal(1)*_state.quat_nominal(3);
	SG[2] = 2*_state.quat_nominal(0)*_state.quat_nominal(1) + 2*_state.quat_nominal(2)*_state.quat_nominal(3);

	float SQ[4];
	SQ[0] = 0.5f * ((_state.quat_nominal(1)*SG[0]) - (_state.quat_nominal(0)*SG[2]) + (_state.quat_nominal(3)*SG[1]));
	SQ[1] = 0.5f * ((_state.quat_nominal(0)*SG[1]) - (_state.quat_nominal(2)*SG[0]) + (_state.quat_nominal(3)*SG[2]));
	SQ[2] = 0.5f * ((_state.quat_nominal(3)*SG[0]) - (_state.quat_nominal(1)*SG[1]) + (_state.quat_nominal(2)*SG[2]));
	SQ[3] = 0.5f * ((_state.quat_nominal(0)*SG[0]) + (_state.quat_nominal(1)*SG[2]) + (_state.quat_nominal(2)*SG[1]));

	// Limit yaw variance increase to prevent a badly conditioned covariance matrix
	yaw_variance = fminf(yaw_variance, 1.0e-2f);

	// Add covariances for additonal yaw uncertainty to existing covariances.
	// This assumes that the additional yaw error is uncorrrelated to existing errors
	P(0,0) += yaw_variance*sq(SQ[2]);
	P(0,1) += yaw_variance*SQ[1]*SQ[2];
	P(1,1) += yaw_variance*sq(SQ[1]);
	P(0,2) += yaw_variance*SQ[0]*SQ[2];
	P(1,2) += yaw_variance*SQ[0]*SQ[1];
	P(2,2) += yaw_variance*sq(SQ[0]);
	P(0,3) -= yaw_variance*SQ[2]*SQ[3];
	P(1,3) -= yaw_variance*SQ[1]*SQ[3];
	P(2,3) -= yaw_variance*SQ[0]*SQ[3];
	P(3,3) += yaw_variance*sq(SQ[3]);
	P(1,0) += yaw_variance*SQ[1]*SQ[2];
	P(2,0) += yaw_variance*SQ[0]*SQ[2];
	P(2,1) += yaw_variance*SQ[0]*SQ[1];
	P(3,0) -= yaw_variance*SQ[2]*SQ[3];
	P(3,1) -= yaw_variance*SQ[1]*SQ[3];
	P(3,2) -= yaw_variance*SQ[0]*SQ[3];
}

// save covariance data for re-use when auto-switching between heading and 3-axis fusion
void Ekf::saveMagCovData()
{
	// save variances for XYZ body axis field
	_saved_mag_bf_variance(0) = P(19, 19);
	_saved_mag_bf_variance(1) = P(20, 20);
	_saved_mag_bf_variance(2) = P(21, 21);

	// save the NE axis covariance sub-matrix
	_saved_mag_ef_ne_covmat = P.slice<2, 2>(16, 16);

	// save variance for the D earth axis
	_saved_mag_ef_d_variance = P(18, 18);
}

void Ekf::loadMagCovData()
{
	// re-instate variances for the XYZ body axis field
	P(19, 19) = _saved_mag_bf_variance(0);
	P(20, 20) = _saved_mag_bf_variance(1);
	P(21, 21) = _saved_mag_bf_variance(2);

	// re-instate the NE axis covariance sub-matrix
	P.slice<2, 2>(16, 16) = _saved_mag_ef_ne_covmat;

	// re-instate the D earth axis variance
	P(18, 18) = _saved_mag_ef_d_variance;
}

void Ekf::startAirspeedFusion()
{
	// If starting wind state estimation, reset the wind states and covariances before fusing any data
	if (!_control_status.flags.wind) {
		// activate the wind states
		_control_status.flags.wind = true;
		// reset the wind speed states and corresponding covariances
		resetWindUsingAirspeed();
	}

	_control_status.flags.fuse_aspd = true;
}

void Ekf::stopAirspeedFusion()
{
	_control_status.flags.fuse_aspd = false;
}

void Ekf::startGpsFusion(const gpsSample &gps_sample)
{
	if (!_control_status.flags.gps) {
		resetHorizontalPositionToGps(gps_sample);

		// when already using another velocity source velocity reset is not necessary
		if (!_control_status.flags.opt_flow && !_control_status.flags.ev_vel) {
			resetVelocityToGps(gps_sample);
		}

		_information_events.flags.starting_gps_fusion = true;
		ECL_INFO("starting GPS fusion");
		_control_status.flags.gps = true;
	}
}

void Ekf::stopGpsFusion()
{
	if (_control_status.flags.gps) {
		stopGpsPosFusion();
		stopGpsVelFusion();

		_control_status.flags.gps = false;
	}

	if (_control_status.flags.gps_yaw) {
		stopGpsYawFusion();
	}

	// We do not need to know the true North anymore
	// EV yaw can start again
	_inhibit_ev_yaw_use = false;
}

void Ekf::stopGpsPosFusion()
{
	if (_control_status.flags.gps) {
		ECL_INFO("stopping GPS position fusion");
		_control_status.flags.gps = false;
		stopGpsHgtFusion();

		resetEstimatorAidStatus(_aid_src_gnss_pos);
	}
}

void Ekf::stopGpsVelFusion()
{
	ECL_INFO("stopping GPS velocity fusion");

	resetEstimatorAidStatus(_aid_src_gnss_vel);
}

void Ekf::startGpsYawFusion(const gpsSample &gps_sample)
{
	if (!_control_status.flags.gps_yaw && resetYawToGps(gps_sample.yaw)) {
		ECL_INFO("starting GPS yaw fusion");
		_control_status.flags.yaw_align = true;
		_control_status.flags.mag_dec = false;
		stopEvYawFusion();
		stopMagHdgFusion();
		stopMag3DFusion();
		_control_status.flags.gps_yaw = true;
	}
}

void Ekf::stopGpsYawFusion()
{
	if (_control_status.flags.gps_yaw) {
		ECL_INFO("stopping GPS yaw fusion");
		_control_status.flags.gps_yaw = false;
		resetEstimatorAidStatus(_aid_src_gnss_yaw);
	}
}

void Ekf::startEvPosFusion()
{
	_control_status.flags.ev_pos = true;
	resetHorizontalPositionToVision();
	_information_events.flags.starting_vision_pos_fusion = true;
	ECL_INFO("starting vision pos fusion");
}

void Ekf::startEvVelFusion()
{
	_control_status.flags.ev_vel = true;
	resetVelocityToVision();
	_information_events.flags.starting_vision_vel_fusion = true;
	ECL_INFO("starting vision vel fusion");
}

void Ekf::startEvYawFusion()
{
	// turn on fusion of external vision yaw measurements and disable all magnetometer fusion
	_control_status.flags.ev_yaw = true;
	_control_status.flags.mag_dec = false;

	stopMagHdgFusion();
	stopMag3DFusion();

	_information_events.flags.starting_vision_yaw_fusion = true;
	ECL_INFO("starting vision yaw fusion");
}

void Ekf::stopEvFusion()
{
	stopEvPosFusion();
	stopEvVelFusion();
	stopEvYawFusion();
}

void Ekf::stopEvPosFusion()
{
	_control_status.flags.ev_pos = false;
	_ev_pos_innov.setZero();
	_ev_pos_innov_var.setZero();
	_ev_pos_test_ratio.setZero();
}

void Ekf::stopEvVelFusion()
{
	_control_status.flags.ev_vel = false;
	_ev_vel_innov.setZero();
	_ev_vel_innov_var.setZero();
	_ev_vel_test_ratio.setZero();
}

void Ekf::stopEvYawFusion()
{
	if (_control_status.flags.ev_yaw) {
		ECL_INFO("stopping EV yaw fusion");
		_control_status.flags.ev_yaw = false;
	}
}

void Ekf::stopAuxVelFusion()
{
	ECL_INFO("stopping aux vel fusion");
	//_control_status.flags.aux_vel = false;
	resetEstimatorAidStatus(_aid_src_aux_vel);
}

void Ekf::stopFlowFusion()
{
	if (_control_status.flags.opt_flow) {
		ECL_INFO("stopping optical flow fusion");
		_control_status.flags.opt_flow = false;
		_flow_innov.setZero();
		_flow_innov_var.setZero();
		_optflow_test_ratio = 0.0f;
	}
}

void Ekf::resetQuatStateYaw(float yaw, float yaw_variance)
{
	// save a copy of the quaternion state for later use in calculating the amount of reset change
	const Quatf quat_before_reset = _state.quat_nominal;

	// update transformation matrix from body to world frame using the current estimate
	// update the rotation matrix using the new yaw value
	_R_to_earth = updateYawInRotMat(yaw, Dcmf(_state.quat_nominal));

	// calculate the amount that the quaternion has changed by
	const Quatf quat_after_reset(_R_to_earth);
	const Quatf q_error((quat_after_reset * quat_before_reset.inversed()).normalized());

	// update quaternion states
	_state.quat_nominal = quat_after_reset;
	uncorrelateQuatFromOtherStates();

	// record the state change
	_state_reset_status.quat_change = q_error;

	// update the yaw angle variance
	if (yaw_variance > FLT_EPSILON) {
		increaseQuatYawErrVariance(yaw_variance);
	}

	// add the reset amount to the output observer buffered data
	for (uint8_t i = 0; i < _output_buffer.get_length(); i++) {
		_output_buffer[i].quat_nominal = _state_reset_status.quat_change * _output_buffer[i].quat_nominal;
	}

	// apply the change in attitude quaternion to our newest quaternion estimate
	// which was already taken out from the output buffer
	_output_new.quat_nominal = _state_reset_status.quat_change * _output_new.quat_nominal;

	_last_static_yaw = NAN;

	// capture the reset event
	_state_reset_status.quat_counter++;
}

// Resets the main Nav EKf yaw to the estimator from the EKF-GSF yaw estimator
// Resets the horizontal velocity and position to the default navigation sensor
// Returns true if the reset was successful
bool Ekf::resetYawToEKFGSF()
{
	if (!isYawEmergencyEstimateAvailable()) {
		return false;
	}

	resetQuatStateYaw(_yawEstimator.getYaw(), _yawEstimator.getYawVar());

	// record a magnetic field alignment event to prevent possibility of the EKF trying to reset the yaw to the mag later in flight
	_flt_mag_align_start_time = _imu_sample_delayed.time_us;
	_control_status.flags.yaw_align = true;

	if (_control_status.flags.mag_hdg || _control_status.flags.mag_3D) {
		// stop using the magnetometer in the main EKF otherwise it's fusion could drag the yaw around
		// and cause another navigation failure
		_control_status.flags.mag_fault = true;
		_warning_events.flags.emergency_yaw_reset_mag_stopped = true;

	} else if (_control_status.flags.gps_yaw) {
		_control_status.flags.gps_yaw_fault = true;
		_warning_events.flags.emergency_yaw_reset_gps_yaw_stopped = true;

	} else if (_control_status.flags.ev_yaw) {
		_inhibit_ev_yaw_use = true;
	}

	_ekfgsf_yaw_reset_time = _imu_sample_delayed.time_us;
	_ekfgsf_yaw_reset_count++;

	return true;
}

bool Ekf::isYawEmergencyEstimateAvailable() const
{
	// don't allow reet using the EKF-GSF estimate until the filter has started fusing velocity
	// data and the yaw estimate has converged
	if (!_yawEstimator.isActive()) {
		return false;
	}

	return _yawEstimator.getYawVar() < sq(_params.EKFGSF_yaw_err_max);
}

bool Ekf::getDataEKFGSF(float *yaw_composite, float *yaw_variance, float yaw[N_MODELS_EKFGSF],
			float innov_VN[N_MODELS_EKFGSF], float innov_VE[N_MODELS_EKFGSF], float weight[N_MODELS_EKFGSF])
{
	return _yawEstimator.getLogData(yaw_composite, yaw_variance, yaw, innov_VN, innov_VE, weight);
}

void Ekf::runYawEKFGSF()
{
	float TAS = 0.f;

	if (_control_status.flags.fixed_wing) {
		if (isTimedOut(_airspeed_sample_delayed.time_us, 1000000)) {
			TAS = _params.EKFGSF_tas_default;

		} else if (_airspeed_sample_delayed.true_airspeed >= _params.arsp_thr) {
			TAS = _airspeed_sample_delayed.true_airspeed;
		}
	}

	const Vector3f imu_gyro_bias = getGyroBias();
	_yawEstimator.update(_imu_sample_delayed, _control_status.flags.in_air, TAS, imu_gyro_bias);
}

void Ekf::resetGpsDriftCheckFilters()
{
	_gps_velNE_filt.setZero();
	_gps_pos_deriv_filt.setZero();

	_gps_horizontal_position_drift_rate_m_s = NAN;
	_gps_vertical_position_drift_rate_m_s = NAN;
	_gps_filtered_horizontal_velocity_m_s = NAN;
}

matrix::SquareMatrix<float, 3> Ekf::orientation_covariances_euler() const
{
	// Jacobian matrix (3x4) containing the partial derivatives of the
	// Euler angle equations with respect to the quaternions
	matrix::Matrix<float, 3, 4> G;

	// quaternion components
	float q1 = _state.quat_nominal(0);
	float q2 = _state.quat_nominal(1);
	float q3 = _state.quat_nominal(2);
	float q4 = _state.quat_nominal(3);

	// numerator components
	float n1 =  2 * q1 * q2 + 2 * q2 * q4;
	float n2 = -2 * q2 * q2 - 2 * q3 * q3 + 1;
	float n3 =  2 * q1 * q4 + 2 * q2 * q3;
	float n4 = -2 * q3 * q3 - 2 * q4 * q4 + 1;
	float n5 =  2 * q1 * q3 + 2 * q2 * q4;
	float n6 = -2 * q1 * q2 - 2 * q2 * q4;
	float n7 = -2 * q1 * q4 - 2 * q2 * q3;

	// Protect against division by 0
	float d1 = n1 * n1 + n2 * n2;
	float d2 = n3 * n3 + n4 * n4;

	if (fabsf(d1) < FLT_EPSILON) {
		d1 = FLT_EPSILON;
	}

	if (fabsf(d2) < FLT_EPSILON) {
		d2 = FLT_EPSILON;
	}

	// Protect against square root of negative numbers
	float x = math::max(-n5 * n5 + 1, 0.0f);

	// compute G matrix
	float sqrt_x = sqrtf(x);
	float g00_03 = 2 * q2 * n2 / d1;
	G(0, 0) =  g00_03;
	G(0, 1) = -4 * q2 * n6 / d1 + (2 * q1 + 2 * q4) * n2 / d1;
	G(0, 2) = -4 * q3 * n6 / d1;
	G(0, 3) =  g00_03;
	G(1, 0) =  2 * q3 / sqrt_x;
	G(1, 1) =  2 * q4 / sqrt_x;
	G(1, 2) =  2 * q1 / sqrt_x;
	G(1, 3) =  2 * q2 / sqrt_x;
	G(2, 0) =  2 * q4 * n4 / d2;
	G(2, 1) =  2 * q3 * n4 / d2;
	G(2, 2) =  2 * q2 * n4 / d2 - 4 * q3 * n7 / d2;
	G(2, 3) =  2 * q1 * n4 / d2 - 4 * q4 * n7 / d2;

	const matrix::SquareMatrix<float, 4> quat_covariances = P.slice<4, 4>(0, 0);

	return G * quat_covariances * G.transpose();
}
