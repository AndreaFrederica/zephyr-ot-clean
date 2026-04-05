/*
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef FAN_CONTROLLER_CURVE_PROFILES_HPP_
#define FAN_CONTROLLER_CURVE_PROFILES_HPP_

#include <stddef.h>
#include <stdint.h>

namespace fanctl::curves {

struct CurvePoint {
	int x;
	int y;
};

class CurveProfiles {
public:
	struct CurveData {
		CurvePoint points[16];
		size_t point_count;
	};

	CurveProfiles();

	int Init();
	int32_t EvaluateAdcRawToVoltage(int32_t adc_raw) const;
	uint8_t EvaluateVoltageToPercent(int32_t voltage_mv) const;
	uint8_t EvaluateAdcRawToPercent(int32_t adc_raw) const;
	uint8_t EvaluatePercentToPwm(uint8_t percent) const;
	int32_t EvaluatePercentToRpm(uint8_t percent) const;
	uint8_t EvaluateRpmToPercent(int32_t rpm) const;

	static const char *GetAdcToVoltagePath();
	static const char *GetVoltageToPercentPath();
	static const char *GetPercentToPwmPath();
	static const char *GetPercentToRpmPath();

private:
	int EnsureDefaultFiles() const;
	int LoadCurveFile(const char *path, CurveData *curve) const;
	static int EvaluateCurve(const CurveData &curve, int input);
	static int EvaluateCurveInverse(const CurveData &curve, int output);

	CurveData adc_to_voltage_;
	CurveData voltage_to_percent_;
	CurveData percent_to_pwm_;
	CurveData percent_to_rpm_;
};

} // namespace fanctl::curves

#endif
