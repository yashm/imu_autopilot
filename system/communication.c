/*=====================================================================

 PIXHAWK Micro Air Vehicle Flying Robotics Toolkit

 (c) 2009, 2010 PIXHAWK PROJECT  <http://pixhawk.ethz.ch>

 This file is part of the PIXHAWK project

 PIXHAWK is free software: you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation, either version 3 of the License, or
 (at your option) any later version.

 PIXHAWK is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with PIXHAWK. If not, see <http://www.gnu.org/licenses/>.

 ======================================================================*/

/**
 * @file
 * @brief Communication reception on both UARTS
 *   @author Lorenz Meier
 *   @author Laurens MacKay
 */

#include "arm7/led.h"

#include "communication.h"
#include "control.h"
#include "ppm.h"
#include "conf.h"
#include "shutter.h"
#include "comm.h"
#include "sys_time.h"
#include "gps.h"

#include "float_checks.h"
#include "debug.h"
#include "calibration.h"
#include "sys_state.h"

#include "lookup_sin_cos.h"

#include "vision_buffer.h"

#include "control_quadrotor_position.h"
#include "params.h"
#include "gps_transformations.h"
#include "outdoor_position_kalman.h"

static uint32_t m_parameter_i = 0;

static void send_system_state(void)
{
	// Send heartbeat to announce presence of this system
	// Send over both communication links
	// Send first message heartbeat
	mavlink_msg_heartbeat_send(MAVLINK_COMM_1,
			global_data.param[PARAM_SYSTEM_TYPE], MAV_AUTOPILOT_PIXHAWK, global_data.state.mav_mode, global_data.state.mav_mode,
			global_data.state.status);
	mavlink_msg_heartbeat_send(MAVLINK_COMM_0,
			global_data.param[PARAM_SYSTEM_TYPE], MAV_AUTOPILOT_PIXHAWK, global_data.state.mav_mode, global_data.state.mav_mode,
			global_data.state.status);
	// Send first global system status
	mavlink_msg_sys_status_send(MAVLINK_COMM_0, global_data.state.control_sensors_present_mask, global_data.state.control_sensors_enabled_mask,
			global_data.state.control_sensors_health_mask, global_data.cpu_usage, global_data.battery_voltage, -1, -1, -1, communication_get_uart_drop_rate(), global_data.i2c0_err_count,
			global_data.i2c1_err_count, global_data.spi_err_count, global_data.spi_err_count);
	mavlink_msg_sys_status_send(MAVLINK_COMM_1, global_data.state.control_sensors_present_mask, global_data.state.control_sensors_enabled_mask,
				global_data.state.control_sensors_health_mask, global_data.cpu_usage, global_data.battery_voltage, -1, -1, -1, communication_get_uart_drop_rate(), global_data.i2c0_err_count,
				global_data.i2c1_err_count, global_data.spi_err_count, global_data.spi_err_count);
}

void execute_command(mavlink_command_long_t* cmd)
{
	switch (cmd->command)
	{
	case MAV_CMD_PREFLIGHT_STORAGE:
	{
		if (cmd->param1 == 0)
		{
			param_read_all();
			debug_message_buffer("Started reading params from eeprom");
		}
		if (cmd->param1 == 1)
		{
			debug_message_buffer("Started writing params to eeprom");
			param_write_all();
		}
	}
	break;
	case MAV_CMD_PREFLIGHT_CALIBRATION:
	{
		if (cmd->param1 == 1)
		{
			start_gyro_calibration();
			m_parameter_i = 0;
		}
	}
	break;
	default:
		// Should never be reached, ignore unknown commands
		debug_message_buffer_sprintf("Rejected unknown command number: %u", cmd->command);
		break;
	}
}

/** @addtogroup COMM */
//@{
/** @name Communication functions
 *  abstraction layer for comm */
//@{
void handle_mavlink_message(mavlink_channel_t chan,
		mavlink_message_t* msg)
{
	uint8_t buf[MAVLINK_MAX_PACKET_LEN];
	uint32_t len;
	switch (chan)
	{
	case MAVLINK_COMM_0:
	{
		if (msg->msgid != MAVLINK_MSG_ID_VISION_POSITION_ESTIMATE && msg->msgid != MAVLINK_MSG_ID_VICON_POSITION_ESTIMATE && msg->msgid != MAVLINK_MSG_ID_IMAGE_TRIGGER_CONTROL && msg->msgid != MAVLINK_MSG_ID_OPTICAL_FLOW)
		{
			// Copy to COMM 1
			len = mavlink_msg_to_send_buffer(buf, msg);
			for (int i = 0; i < len; i++)
			{
				uart1_transmit(buf[i]);
			}
		}
	}
	break;
	case MAVLINK_COMM_1:
	{
		if (msg->msgid != MAVLINK_MSG_ID_VISION_POSITION_ESTIMATE && msg->msgid != MAVLINK_MSG_ID_VICON_POSITION_ESTIMATE && msg->msgid != MAVLINK_MSG_ID_IMAGE_TRIGGER_CONTROL)
		{
			// Copy to COMM 0
			len = mavlink_msg_to_send_buffer(buf, msg);
			for (int i = 0; i < len; i++)
			{
				uart0_transmit(buf[i]);
			}
			break;
		}
	}
	break;
	default:
		break;
	}


	switch (msg->msgid)
	{
	case MAVLINK_MSG_ID_SET_MODE:
	{
		mavlink_set_mode_t mode;
		mavlink_msg_set_mode_decode(msg, &mode);
		// Check if this system should change the mode
		if (mode.target_system == (uint8_t)global_data.param[PARAM_SYSTEM_ID])
		{
			sys_set_mode(mode.base_mode);

			// Emit current mode
			send_system_state();

		}
	}
	break;
	case MAVLINK_MSG_ID_COMMAND_LONG:
	{
		mavlink_command_long_t cmd;
		mavlink_msg_command_long_decode(msg, &cmd);
		execute_command(&cmd);
	}
	break;
	case MAVLINK_MSG_ID_SYSTEM_TIME:
	{
		if (!sys_time_clock_get_unix_offset())
		{
			int64_t offset = ((int64_t) mavlink_msg_system_time_get_time_unix_usec(
					msg)) - (int64_t) sys_time_clock_get_time_usec();
			sys_time_clock_set_unix_offset(offset);

			debug_message_buffer("UNIX offset updated");
		}
		else
		{

			//			debug_message_buffer("UNIX offset REFUSED");
		}
	}
	break;
	case MAVLINK_MSG_ID_REQUEST_DATA_STREAM:
	{
		mavlink_request_data_stream_t stream;
		mavlink_msg_request_data_stream_decode(msg, &stream);
		debug_message_buffer_sprintf("REQUEST_DATA_STREAM #%i changed",stream.req_stream_id);
		switch (stream.req_stream_id)
		{
		case 0: // UNIMPLEMENTED
			break;
		case 1: // RAW SENSOR DATA
			global_data.param[PARAM_SEND_SLOT_RAW_IMU] = stream.start_stop;
			break;
		case 2: // EXTENDED SYSTEM STATUS
			global_data.param[PARAM_SEND_SLOT_ATTITUDE] = stream.start_stop;
			break;
		case 3: // REMOTE CONTROL CHANNELS
			global_data.param[PARAM_SEND_SLOT_REMOTE_CONTROL] = stream.start_stop;
			break;
		case 4: // RAW CONTROLLER
			//global_data.param[PARAM_SEND_SLOT_DEBUG_5] = stream.start_stop;
			//global_data.param[PARAM_SEND_SLOT_DEBUG_3] = stream.start_stop;
			global_data.param[PARAM_SEND_SLOT_CONTROLLER_OUTPUT] = stream.start_stop;
			break;
		case 5: // SENSOR FUSION

			//LOST IN GROUDNCONTROL
			//			global_data.param[PARAM_SEND_SLOT_DEBUG_5] = stream.start_stop;
			break;
		case 6: // POSITION
			global_data.param[PARAM_SEND_SLOT_DEBUG_5] = stream.start_stop;
			break;
		case 10: // EXTRA1
			global_data.param[PARAM_SEND_SLOT_DEBUG_2] = stream.start_stop;
			break;
		case 11: // EXTRA2
			global_data.param[PARAM_SEND_SLOT_DEBUG_4] = stream.start_stop;
			break;
		case 12: // EXTRA3
			global_data.param[PARAM_SEND_SLOT_DEBUG_6] = stream.start_stop;
			break;
		default:
			// Do nothing
			break;
		}
	}
	break;
	case MAVLINK_MSG_ID_PARAM_REQUEST_READ:
	{
		mavlink_param_request_read_t set;
		mavlink_msg_param_request_read_decode(msg, &set);

		// Check if this message is for this system
		if ((uint8_t) set.target_system
				== (uint8_t) global_data.param[PARAM_SYSTEM_ID]
				                               && (uint8_t) set.target_component
				                               == (uint8_t) global_data.param[PARAM_COMPONENT_ID])
		{
			char* key = (char*) set.param_id;

			if (set.param_id[0] == '\0')
			{
				// Choose parameter based on index
				if (set.param_index < ONBOARD_PARAM_COUNT)
				{
					// Report back value
					mavlink_msg_param_value_send(chan,
							(int8_t*) global_data.param_name[set.param_index],
							global_data.param[set.param_index], MAVLINK_TYPE_FLOAT, ONBOARD_PARAM_COUNT, set.param_index);
				}
			}
			else
			{
				for (int i = 0; i < ONBOARD_PARAM_COUNT; i++)
				{
					bool match = true;
					for (int j = 0; j < ONBOARD_PARAM_NAME_LENGTH; j++)
					{
						// Compare
						if (((char) (global_data.param_name[i][j]))
								!= (char) (key[j]))
						{
							match = false;
						}

						// End matching if null termination is reached
						if (((char) global_data.param_name[i][j]) == '\0')
						{
							break;
						}
					}

					// Check if matched
					if (match)
					{
						// Report back value
						mavlink_msg_param_value_send(chan,
								(int8_t*) global_data.param_name[i],
								global_data.param[i], MAVLINK_TYPE_FLOAT, ONBOARD_PARAM_COUNT, m_parameter_i);
					}
				}
			}
		}
	}
	break;
	case MAVLINK_MSG_ID_PARAM_REQUEST_LIST:
	{
		// Start sending parameters
		m_parameter_i = 0;
	}
	break;
	case MAVLINK_MSG_ID_PARAM_SET:
	{
		mavlink_param_set_t set;
		mavlink_msg_param_set_decode(msg, &set);

		// Check if this message is for this system
		if ((uint8_t) set.target_system
				== (uint8_t) global_data.param[PARAM_SYSTEM_ID]
				                               && (uint8_t) set.target_component
				                               == (uint8_t) global_data.param[PARAM_COMPONENT_ID])
		{
			char* key = (char*) set.param_id;

			for (int i = 0; i < ONBOARD_PARAM_COUNT; i++)
			{
				bool match = true;
				for (int j = 0; j < ONBOARD_PARAM_NAME_LENGTH; j++)
				{
					// Compare
					if (((char) (global_data.param_name[i][j]))
							!= (char) (key[j]))
					{
						match = false;
					}

					// End matching if null termination is reached
					if (((char) global_data.param_name[i][j]) == '\0')
					{
						break;
					}
				}

				// Check if matched
				if (match)
				{
					// Only write and emit changes if there is actually a difference
					// AND only write if new value is NOT "not-a-number"
					// AND is NOT infy
					if (global_data.param[i] != set.param_value
							&& !isnan(set.param_value)
							&& !isinf(set.param_value))
					{
						global_data.param[i] = set.param_value;
						// Report back new value
						mavlink_msg_param_value_send(MAVLINK_COMM_0,
								(int8_t*) global_data.param_name[i],
								global_data.param[i], MAVLINK_TYPE_FLOAT, ONBOARD_PARAM_COUNT, m_parameter_i);
						mavlink_msg_param_value_send(MAVLINK_COMM_1,
								(int8_t*) global_data.param_name[i],
								global_data.param[i], MAVLINK_TYPE_FLOAT, ONBOARD_PARAM_COUNT, m_parameter_i);

						debug_message_buffer_sprintf("Parameter received param id=%i",i);
					}
				}
			}
		}
	}
	break;
	case MAVLINK_MSG_ID_SET_POSITION_CONTROL_OFFSET:
	{
		mavlink_set_position_control_offset_t set;
		mavlink_msg_set_position_control_offset_decode(msg, &set);
		//global_data.attitude_setpoint_pos_body_offset.z = set.yaw;

		//Ball Tracking
		if (global_data.param[PARAM_POSITIONSETPOINT_ACCEPT] == 1.0f && global_data.param[PARAM_POSITION_YAW_TRACKING]==1.0f)
		{
			global_data.param[PARAM_POSITION_SETPOINT_YAW]
			                  = global_data.attitude.z + set.yaw;

			mavlink_msg_debug_send(global_data.param[PARAM_SEND_DEBUGCHAN], 0, 92, set.yaw);
		}
	}
	break;
	case MAVLINK_MSG_ID_SET_CAM_SHUTTER:
	{
		// Decode the desired shutter
		mavlink_set_cam_shutter_t cam;
		mavlink_msg_set_cam_shutter_decode(msg, &cam);
		shutter_set(cam.interval, cam.exposure);
		debug_message_buffer_sprintf("set_cam_shutter. interval %i", cam.interval);

	}
	break;
	case MAVLINK_MSG_ID_IMAGE_TRIGGER_CONTROL:
	{
		mavlink_image_trigger_control_t trigger;
		uint8_t enable = mavlink_msg_image_trigger_control_get_enable(msg);

		shutter_control(enable);
		if (enable)
		{
			debug_message_buffer("CAM: Enabling hardware trigger");
		}
		else
		{
			debug_message_buffer("CAM: Disabling hardware trigger");
		}
	}
	break;
	case MAVLINK_MSG_ID_VISION_POSITION_ESTIMATE:
	{
		mavlink_vision_position_estimate_t pos;
		mavlink_msg_vision_position_estimate_decode(msg, &pos);

		vision_buffer_handle_data(&pos);
		// Update validity time is done in vision buffer

	}
	break;
	case MAVLINK_MSG_ID_GLOBAL_VISION_POSITION_ESTIMATE:
		{
			mavlink_global_vision_position_estimate_t pos;
			mavlink_msg_global_vision_position_estimate_decode(msg, &pos);

			vision_buffer_handle_global_data(&pos);
			// Update validity time is done in vision buffer
		}
		break;
	case MAVLINK_MSG_ID_VICON_POSITION_ESTIMATE:
	{
		mavlink_vicon_position_estimate_t pos;
		mavlink_msg_vicon_position_estimate_decode(msg, &pos);

		global_data.vicon_data.x = pos.x;
		global_data.vicon_data.y = pos.y;
		global_data.vicon_data.z = pos.z;
		global_data.state.vicon_new_data=1;
		// Update validity time
		global_data.vicon_last_valid = sys_time_clock_get_time_usec();
		global_data.state.vicon_ok=1;
		global_data.state.vicon_attitude_new_data=1;

		global_data.vicon_magnetometer_replacement.x = 230.0f*lookup_cos(pos.yaw);
		global_data.vicon_magnetometer_replacement.y = -230.0f*lookup_sin(pos.yaw);
		global_data.vicon_magnetometer_replacement.z = 480.f;

//#if HMC5843_I2C_BUS == 0 //external mag
//		global_data.magnet_raw.x = (mag.x - (int16_t)global_data.param[PARAM_CAL_MAG_OFFSET_X]);
//		global_data.magnet_raw.y = -(mag.y - (int16_t)global_data.param[PARAM_CAL_MAG_OFFSET_Y]);
//		global_data.magnet_raw.z = -(mag.z - (int16_t)global_data.param[PARAM_CAL_MAG_OFFSET_Z]);
//#else	//this is the imu mag
//		global_data.magnet_raw.x = -(mag.x - (int16_t)global_data.param[PARAM_CAL_MAG_OFFSET_X]);
//		global_data.magnet_raw.y = (mag.y - (int16_t)global_data.param[PARAM_CAL_MAG_OFFSET_Y]);
//		global_data.magnet_raw.z = -(mag.z - (int16_t)global_data.param[PARAM_CAL_MAG_OFFSET_Z]);
//#endif

//		debug_vect("mag_rep", global_data.vicon_magnetometer_replacement);
//		float_vect3 mag_corr;
//		mag_corr.x = global_data.magnet_corrected.x;
//		mag_corr.y = global_data.magnet_corrected.y;
//		mag_corr.z = global_data.magnet_corrected.z;
//		debug_vect("mag_corr", mag_corr);

		if (!global_data.state.vision_ok)
		{
			global_data.vision_magnetometer_replacement.x = 230.0f*lookup_cos(pos.yaw);
			global_data.vision_magnetometer_replacement.y = -230.0f*lookup_sin(pos.yaw);
			global_data.vision_magnetometer_replacement.z = 0.f;
		}

		// Set position setpoint offset to compensate optical flow drift
		if (global_data.state.position_estimation_mode == POSITION_ESTIMATION_MODE_OPTICAL_FLOW_ULTRASONIC_ADD_VICON_AS_OFFSET)
		{
			global_data.position_setpoint_offset.x = global_data.position_setpoint_offset.x*0.8f + 0.2f*(global_data.position.x - global_data.vicon_data.x);
			global_data.position_setpoint_offset.y = global_data.position_setpoint_offset.y*0.8f + 0.2f*(global_data.position.y - global_data.vicon_data.y);
			global_data.position_setpoint_offset.z = 0; // Z estimation has no drift, no offset desirable
		}

		//send the vicon message to UART0 with new timestamp, but only if the message was not received over UART0 (otherwise we'll have message echoing)
		if (chan != MAVLINK_COMM_0)
			mavlink_msg_vicon_position_estimate_send(MAVLINK_COMM_0, sys_time_clock_get_unix_loop_start_time(), pos.x, pos.y, pos.z, pos.roll, pos.pitch, pos.yaw);

	}
	break;
	case MAVLINK_MSG_ID_OPTICAL_FLOW:
	{
		mavlink_optical_flow_t flow;
		mavlink_msg_optical_flow_decode(msg, &flow);
		global_data.optflow.x = -flow.flow_comp_m_y;//physical mounting direction of flow sensor compensated here
		global_data.optflow.y = flow.flow_comp_m_x;//physical mounting direction of flow sensor compensated here
		global_data.optflow.z = flow.quality;
		global_data.ground_distance = flow.ground_distance;

		if (global_data.optflow.z > 20)
		{
			global_data.flow_last_valid = sys_time_clock_get_time_usec();
		}

//		float_vect3 flowQuad, flowWorld;
//		float x_comp =  -global_data.attitude_rate.y * global_data.ground_distance;
//		float y_comp =  global_data.attitude_rate.x * global_data.ground_distance;
//		flowQuad.x = (global_data.optflow.x == global_data.optflow.x) ? global_data.optflow.x + x_comp : 0;
//		flowQuad.y = (global_data.optflow.y == global_data.optflow.y) ? global_data.optflow.y + y_comp : 0;
//		flowQuad.z = 0;
//		body2navi(&flowQuad, &global_data.attitude, &flowWorld);
//		debug_vect("flowC", flowWorld);
//		flowQuad.x = (global_data.optflow.x == global_data.optflow.x) ? global_data.optflow.x : 0;
//		flowQuad.y = (global_data.optflow.y == global_data.optflow.y) ? global_data.optflow.y : 0;
//		flowQuad.z = 0;
//		body2navi(&flowQuad, &global_data.attitude, &flowWorld);
//		debug_vect("flowNC", flowWorld);

		//mavlink_msg_optical_flow_send(MAVLINK_COMM_0, flow.time_usec, flow.sensor_id, global_data.optflow.x, global_data.optflow.y, global_data.optflow.z, global_data.ground_distance);
		break;
	}
	case MAVLINK_MSG_ID_PING:
	{
		mavlink_ping_t ping;
		mavlink_msg_ping_decode(msg, &ping);
		if (ping.target_system == 0 && ping.target_component == 0)
		{
			// Respond to ping
			uint64_t r_timestamp = sys_time_clock_get_unix_time();
			mavlink_msg_ping_send(chan, ping.seq, msg->sysid, msg->compid, r_timestamp);
		}
	}
	break;
	case MAVLINK_MSG_ID_SET_LOCAL_POSITION_SETPOINT:
	{
		mavlink_set_local_position_setpoint_t sp;
		mavlink_msg_set_local_position_setpoint_decode(msg, &sp);
		if (sp.target_system == global_data.param[PARAM_SYSTEM_ID])
		{
			if (global_data.param[PARAM_POSITIONSETPOINT_ACCEPT] == 1)
			{
				if (sp.x >= global_data.position_setpoint_min.x && sp.y
						>= global_data.position_setpoint_min.y && sp.z
						>= global_data.position_setpoint_min.z && sp.x
						<= global_data.position_setpoint_max.x && sp.y
						<= global_data.position_setpoint_max.y && sp.z
						<= global_data.position_setpoint_max.z)
				{
					global_data.param[PARAM_POSITION_SETPOINT_X] = sp.x;
					global_data.param[PARAM_POSITION_SETPOINT_Y] = sp.y;
					global_data.param[PARAM_POSITION_SETPOINT_Z] = sp.z;

					if (global_data.param[PARAM_POSITION_YAW_TRACKING] == 0)
					{
						// Only update yaw if we are not tracking ball.
						global_data.param[PARAM_POSITION_SETPOINT_YAW] = sp.yaw/180*M_PI;
					}

					//check if we want to start or land
					if (global_data.state.status == MAV_STATE_ACTIVE || global_data.state.status == MAV_STATE_CRITICAL)
					{
						if (sp.z > -0.1)
						{
							if (!(global_data.state.fly == FLY_GROUNDED
									|| global_data.state.fly == FLY_SINKING
									|| global_data.state.fly == FLY_WAIT_LANDING
									|| global_data.state.fly == FLY_LANDING
									|| global_data.state.fly == FLY_RAMP_DOWN))
							{
								//if setpoint is lower that ground initiate landing
								global_data.state.fly = FLY_SINKING;
								global_data.param[PARAM_POSITION_SETPOINT_Z] = -0.2;//with lowpass
								debug_message_buffer("Sinking for LANDING. (z-sp lower than 10cm)");
							}
							else if (!(global_data.state.fly == FLY_GROUNDED))
							{
								global_data.param[PARAM_POSITION_SETPOINT_Z] = -0.2;//with lowpass
							}
						}
						else if (global_data.state.fly == FLY_GROUNDED && sp.z < -0.50)
						{
							//start if we were grounded and get a sp over 0.5m
							if (global_data.state.mav_mode & MAV_MODE_FLAG_SAFETY_ARMED)
							{
								global_data.state.fly = FLY_WAIT_MOTORS;
								debug_message_buffer("STARTING wait motors. (z-sp higher than 50cm)");
							}
							//set point changed with lowpass, after 5s it will be ok.
						}
					}

					//SINK TO 0.7m if we are critical or emergency
					if (global_data.state.status == MAV_STATE_EMERGENCY || global_data.state.status == MAV_STATE_CRITICAL)
					{
						global_data.param[PARAM_POSITION_SETPOINT_Z] = -0.7;//with lowpass
					}

					debug_message_buffer("Setpoint accepted and set.");
				}
				else
				{
					debug_message_buffer("Setpoint refused. Out of range.");
				}
			}
			else
			{
				debug_message_buffer("Setpoint refused. Param setpoint accept=0.");
			}
		}
	}
	break;
	default:
		break;
	}
}

/**
 * @brief Send low-priority messages at a maximum rate of xx Hertz
 *
 * This function sends messages at a lower rate to not exceed the wireless
 * bandwidth. It sends one message each time it is called until the buffer is empty.
 * Call this function with xx Hertz to increase/decrease the bandwidth.
 */
void communication_queued_send(void)
{
	//send parameters one by one
	//	for (int m_parameter_i = 0; m_parameter_i < ONBOARD_PARAM_COUNT; i++)
	if (m_parameter_i < ONBOARD_PARAM_COUNT)
	{
		mavlink_msg_param_value_send(MAVLINK_COMM_0,
				(int8_t*) global_data.param_name[m_parameter_i],
				global_data.param[m_parameter_i], MAVLINK_TYPE_FLOAT, ONBOARD_PARAM_COUNT, m_parameter_i);
		mavlink_msg_param_value_send(MAVLINK_COMM_1,
				(int8_t*) global_data.param_name[m_parameter_i],
				global_data.param[m_parameter_i], MAVLINK_TYPE_FLOAT, ONBOARD_PARAM_COUNT, m_parameter_i);
		m_parameter_i++;
	}
}

uint32_t communication_get_uart_drop_rate(void)
{
	return ((global_data.comm.uart0_rx_drop_count*1000+1)/(global_data.comm.uart0_rx_success_count+1)) + ((global_data.comm.uart1_rx_drop_count*1000+1)/(global_data.comm.uart1_rx_success_count+1));
}


void communication_init(void)
{
	if (global_data.param[PARAM_GPS_MODE] > 0)
	{
		global_data.state.uart1mode = UART_MODE_GPS;
	}
	else
	{
		global_data.state.uart1mode = UART_MODE_MAVLINK;
	}
}

/**
 * @brief Receive communication packets and handle them
 *
 * This function decodes packets on the protocol level and also handles
 * their value by calling the appropriate functions.
 */
void communication_receive(void)
{
	mavlink_message_t msg;
	mavlink_status_t status =
	{ 0 };
	status.packet_rx_drop_count = 0;

	// COMMUNICATION WITH ONBOARD COMPUTER

	while (uart0_char_available())
	{
		uint8_t c = uart0_get_char();


		if (global_data.state.uart0mode == UART_MODE_MAVLINK)
		{
			// Try to get a new message
			if (mavlink_parse_char(MAVLINK_COMM_0, c, &msg, &status))
			{
				// Handle message
				handle_mavlink_message(MAVLINK_COMM_0, &msg);
			}
		}
		else if (global_data.state.uart0mode == UART_MODE_BYTE_FORWARD)
		{
			uart1_transmit(c);
		}
		// And get the next one
	}

	// Update global packet drops counter
	global_data.comm.uart0_rx_drop_count += status.packet_rx_drop_count;
	global_data.comm.uart0_rx_success_count += status.packet_rx_success_count;
	status.packet_rx_drop_count = 0;

	// COMMUNICATION WITH EXTERNAL COMPUTER

	while (uart1_char_available())
	{
		uint8_t c = uart1_get_char();

		// Check if this link is used for MAVLink or GPS
		if (global_data.state.uart1mode == UART_MODE_MAVLINK)
		{
			//uart0_transmit((unsigned char)c);
			// Try to get a new message
			if (mavlink_parse_char(MAVLINK_COMM_1, c, &msg, &status))
			{
				// Handle message
				handle_mavlink_message(MAVLINK_COMM_1, &msg);
			}
		}
		else if (global_data.state.uart1mode == UART_MODE_GPS)
		{
			if (global_data.state.gps_mode == 10)
			{
				static uint8_t gps_i = 0;
				static char gps_chars[MAVLINK_MSG_STATUSTEXT_FIELD_TEXT_LEN];
				if (c == '$' || gps_i == MAVLINK_MSG_STATUSTEXT_FIELD_TEXT_LEN
						- 1)
				{
					gps_i = 0;
					char gps_chars_buf[MAVLINK_MSG_STATUSTEXT_FIELD_TEXT_LEN];
					strncpy(gps_chars_buf, gps_chars,
							MAVLINK_MSG_STATUSTEXT_FIELD_TEXT_LEN);
					debug_message_buffer(gps_chars_buf);

				}
				gps_chars[gps_i++] = c;
			}
			if (gps_parse(c))
			{
				// New GPS data received
				//debug_message_buffer("RECEIVED NEW GPS DATA");
				parse_gps_msg();

				if (gps_lat == 0)
				{
					global_data.state.gps_ok = 0;
					//debug_message_buffer("GPS Signal Lost");
				}
				else
				{
					global_data.state.gps_ok = 1;

//					mavlink_msg_gps_raw_send(
//							global_data.param[PARAM_SEND_DEBUGCHAN],
//							sys_time_clock_get_unix_loop_start_time(), gps_mode, gps_lat
//									/ 1e7f, gps_lon / 1e7f, gps_alt / 100.0f,
//							0.0f, 0.0f, gps_gspeed / 100.0f, gps_course / 10.0f);
				}
				//				// Output satellite info
				//				for (int i = 0; i < gps_nb_channels; i++)
				//				{
				//					mavlink_msg_gps_status_send(global_data.param[PARAM_SEND_DEBUGCHAN], gps_numSV, gps_svinfos[i].svid, gps_satellite_used(gps_svinfos[i].qi), gps_svinfos[i].elev, ((gps_svinfos[i].azim/360.0f)*255.0f), gps_svinfos[i].cno);
				//				}
			}

		}
		else if (global_data.state.uart1mode == UART_MODE_BYTE_FORWARD)
		{
			uart0_transmit(c);
			led_toggle(LED_YELLOW);
		}
		// And get the next one
	}

	// Update global packet drops counter
	global_data.comm.uart0_rx_drop_count += status.packet_rx_drop_count;
	global_data.comm.uart0_rx_success_count += status.packet_rx_success_count;
	status.packet_rx_drop_count = 0;
}

