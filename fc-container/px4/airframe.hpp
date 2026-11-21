/**
 * @file airframe.hpp
 * @brief Generates the PX4 SITL airframe (an init.d-posix script) from the aircraft data file.
 *
 * Pure text generation from an astrosim::aircraft::Aircraft: no I/O, no PX4 dependency. The generated script sets
 * the control allocator geometry (8 rotors, in protocol motor order, positions in PX4's FRD body frame relative to
 * the loaded centre of mass), the thrust model, the hover thrust, the stick scaling and the parameters a SITL run
 * with no RC and no GCS needs, each with a comment.
 */
#pragma once
#include <string>

#include "astrosim/aircraft/aircraft.hpp"

namespace astrosim::px4 {

/// @brief PX4 SYS_AUTOSTART id of the generated airframe (the script is named `<id>_astrosim_aircraft`).
inline constexpr int kAirframeAutostartId = 99001;
/// @brief Model name part of the generated script's file name.
inline constexpr const char* kAirframeModelName = "astrosim_aircraft";

/// @brief Settings that are not aircraft data (they come from the bridge configuration, [px4]).
struct AirframeOptions {
  double max_tilt_deg = 25;        ///< Tilt of a full roll/pitch stick (MPC_MAN_TILT_MAX), degrees.
  double max_yaw_rate_dps = 45;    ///< Yaw rate of a full yaw stick (MPC_MAN_Y_MAX), degrees per second.
};

/**
 * @brief Control gains derived from the aircraft data (see generateAirframe()).
 *
 * PX4's multicopter defaults assume a small, fast quadrotor. This aircraft has a much lower control authority per
 * unit of inertia and slow rotors (time constants around 0.1 s), so the defaults leave the cascaded loops
 * (rate, attitude, velocity, position) with too little separation and the aircraft oscillates (observed in SITL).
 * The gains below are derived from the aircraft file instead.
 */
struct AirframeTuning {
  double rotor_lag_s = 0;              ///< Slowest rotor lag, (time_constant_up + time_constant_down) / 2, s.
  double rate_bandwidth_rad_s = 0;     ///< Targeted rate-loop crossover, 0.6 / rotor_lag_s.
  double roll_authority_rad_s2 = 0;    ///< Angular acceleration per unit normalized roll command.
  double pitch_authority_rad_s2 = 0;   ///< Same, pitch.
  double yaw_authority_rad_s2 = 0;     ///< Same, yaw.
  double roll_rate_p = 0;              ///< MC_ROLLRATE_P.
  double pitch_rate_p = 0;             ///< MC_PITCHRATE_P.
  double yaw_rate_p = 0;               ///< MC_YAWRATE_P.
  double roll_pitch_att_p = 0;         ///< MC_ROLL_P and MC_PITCH_P.
  double yaw_att_p = 0;                ///< MC_YAW_P.
  double xy_vel_p_acc = 0;             ///< MPC_XY_VEL_P_ACC.
  double xy_p = 0;                     ///< MPC_XY_P.
};

/// @brief A generated airframe.
struct Airframe {
  int autostart_id = kAirframeAutostartId;  ///< SYS_AUTOSTART value selecting the script.
  std::string file_name;                    ///< `<id>_astrosim_aircraft`.
  std::string text;                         ///< Script contents.
  double hover_thrust = 0;                  ///< MPC_THR_HOVER: loaded weight over the sum of the rotors' maximum thrust.
  AirframeTuning tuning;                    ///< The derived control gains written to the script.
};

/**
 * @brief Generate the airframe script.
 *
 * - CA_AIRFRAME 0 (multirotor), CA_ROTOR_COUNT = number of motors; PWM_MAIN_FUNCn = Motor n so PX4 output i is
 *   protocol motor i (identity mapping).
 * - CA_ROTORn_PX/PY/PZ: hub position relative to the loaded (airframe + pilot) centre of mass, FLU to FRD.
 * - CA_ROTORn_KM: signed moment coefficient. PX4 (control_allocator module.yaml): torque = KM * thrust, positive for a
 *   counter-clockwise rotor (seen from above), negative for clockwise. The magnitude is the aircraft's
 *   moment_constant_m.
 * - CA_ROTORn_CT: thrust coefficient relative to the mean rotor, so the allocator knows rotors differ
 *   (for example the lower coaxial rotor of the sample aircraft produces less thrust).
 * - THR_MDL_FAC 1: PX4's model is rel_thrust = f * s^2 + (1 - f) * s; the engine maps power linearly to rotor speed
 *   and thrust is proportional to speed squared, so f = 1.
 * - MPC_THR_HOVER = loaded weight / sum of max thrust (motor_constant * max_rot_velocity^2).
 * - Control gains (see AirframeTuning): PX4's normalized torque command is turned into a physical torque by the
 *   allocator's normalization (control_allocation/ControlAllocationPseudoInverse.cpp: roll and pitch share a scale,
 *   yaw has its own); the angular acceleration per unit command follows from the loaded inertia. The rate loop gain
 *   is set so its crossover is 1 / rotor lag, the attitude loop to a quarter of that, the velocity loop to a third
 *   of the attitude loop and the position loop to 0.4 of the velocity loop.
 * @param aircraft A validated aircraft.
 * @param options Stick scaling.
 * @return The airframe.
 * @throws std::invalid_argument if the aircraft cannot hover (maximum thrust below 1.1 times the weight), has more
 * than 12 motors (PX4's control allocator limit), or the options are not positive and finite.
 */
Airframe generateAirframe(const aircraft::Aircraft& aircraft, const AirframeOptions& options);

}  // namespace astrosim::px4
