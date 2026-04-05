/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include "fan_shared_state.hpp"

namespace fanctl {

void FanSharedStateInit(FanControllerSharedState *shared)
{
	for (size_t i = 0; i < kFanCount; ++i) {
		atomic_set(&shared->channels[i].enabled, 0);
		atomic_set(&shared->channels[i].percent, 40);
		atomic_set(&shared->channels[i].effective_percent, 40);
		atomic_set(&shared->channels[i].pwm_percent, 40);
		atomic_set(&shared->channels[i].pwm_pulse_ns, 0);
		atomic_set(&shared->channels[i].adc_raw, 0);
		atomic_set(&shared->channels[i].adc_mv, 0);
		atomic_set(&shared->channels[i].mapped_voltage_mv, 0);
		atomic_set(&shared->channels[i].actual_rpm, 0);
		atomic_set(&shared->channels[i].target_rpm, 0);
		atomic_set(&shared->channels[i].actual_percent, 0);
		atomic_set(&shared->channels[i].use_adc_target, 0);
		atomic_set(&shared->channels[i].tach_edges, 0);
		shared->last_tach_edges[i] = 0;
	}
	
	atomic_set(&shared->control_loop_running, 0);
	atomic_set(&shared->loop_counter, 0);
	atomic_set(&shared->last_loop_duration_us, 0);
	atomic_set(&shared->max_loop_duration_us, 0);
	shared->last_sample_time_ms = 0;
	
	k_mutex_init(&shared->snapshot_mutex);
}

void FanSharedStateRead(const FanControllerSharedState *shared, size_t index, FanState *state)
{
	if (index >= kFanCount || state == nullptr) {
		return;
	}
	
	const FanChannelSharedState *ch = &shared->channels[index];
	
	state->enabled = atomic_get(&ch->enabled) != 0;
	state->percent = static_cast<uint8_t>(atomic_get(&ch->percent));
	state->effective_percent = static_cast<uint8_t>(atomic_get(&ch->effective_percent));
	state->pwm_percent = static_cast<uint8_t>(atomic_get(&ch->pwm_percent));
	state->pwm_pulse_ns = static_cast<uint32_t>(atomic_get(&ch->pwm_pulse_ns));
	state->adc_raw = static_cast<int32_t>(atomic_get(&ch->adc_raw));
	state->adc_mv = static_cast<int32_t>(atomic_get(&ch->adc_mv));
	state->mapped_voltage_mv = static_cast<int32_t>(atomic_get(&ch->mapped_voltage_mv));
	state->actual_rpm = static_cast<int32_t>(atomic_get(&ch->actual_rpm));
	state->target_rpm = static_cast<int32_t>(atomic_get(&ch->target_rpm));
	state->actual_percent = static_cast<uint8_t>(atomic_get(&ch->actual_percent));
	state->use_adc_target = atomic_get(&ch->use_adc_target) != 0;
	state->tach_valid = true;
}

void FanSharedStateReadAll(const FanControllerSharedState *shared, FanState states[kFanCount])
{
	if (states == nullptr) {
		return;
	}
	
	// 使用互斥锁保证批量读取的一致性
	k_mutex_lock(const_cast<k_mutex *>(&shared->snapshot_mutex), K_FOREVER);
	
	for (size_t i = 0; i < kFanCount; ++i) {
		FanSharedStateRead(shared, i, &states[i]);
	}
	
	k_mutex_unlock(const_cast<k_mutex *>(&shared->snapshot_mutex));
}

void FanSharedStateWriteOutput(FanChannelSharedState *ch, bool enabled, uint8_t percent,
				uint8_t effective_percent, uint8_t pwm_percent, uint32_t pwm_pulse_ns)
{
	atomic_set(&ch->enabled, enabled ? 1 : 0);
	atomic_set(&ch->percent, percent);
	atomic_set(&ch->effective_percent, effective_percent);
	atomic_set(&ch->pwm_percent, pwm_percent);
	atomic_set(&ch->pwm_pulse_ns, static_cast<atomic_val_t>(pwm_pulse_ns));
}

void FanSharedStateWriteAdc(FanChannelSharedState *ch, int32_t adc_raw, int32_t adc_mv,
			     int32_t mapped_voltage_mv)
{
	atomic_set(&ch->adc_raw, adc_raw);
	atomic_set(&ch->adc_mv, adc_mv);
	atomic_set(&ch->mapped_voltage_mv, mapped_voltage_mv);
}

void FanSharedStateWriteRpm(FanChannelSharedState *ch, int32_t actual_rpm, int32_t target_rpm,
			     uint8_t actual_percent)
{
	atomic_set(&ch->actual_rpm, actual_rpm);
	atomic_set(&ch->target_rpm, target_rpm);
	atomic_set(&ch->actual_percent, actual_percent);
}

bool FanSharedStateReadUseAdcTarget(const FanChannelSharedState *ch)
{
	return atomic_get(&ch->use_adc_target) != 0;
}

void FanSharedStateWriteConfig(FanChannelSharedState *ch, bool use_adc_target)
{
	atomic_set(&ch->use_adc_target, use_adc_target ? 1 : 0);
}

} // namespace fanctl
