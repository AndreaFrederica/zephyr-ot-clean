/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include "curve_profiles.hpp"

#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/sys/util.h>

#include "generated/default_curve_assets.hpp"
#include "storage.hpp"

namespace fanctl::curves {

namespace {

constexpr const char *kAdcToVoltagePath = "/etc/fanctl/curves/adc_to_voltage.json";
constexpr const char *kVoltageToPercentPath = "/etc/fanctl/curves/voltage_to_percent.json";
constexpr const char *kPercentToPwmPath = "/etc/fanctl/curves/percent_to_pwm.json";
constexpr const char *kPercentToRpmPath = "/etc/fanctl/curves/percent_to_rpm.json";

void SkipWhitespace(const char **cursor)
{
	while (**cursor != '\0' && isspace(static_cast<unsigned char>(**cursor)) != 0) {
		++(*cursor);
	}
}

bool SeekChar(const char **cursor, char needle)
{
	while (**cursor != '\0') {
		if (**cursor == needle) {
			return true;
		}
		++(*cursor);
	}

	return false;
}

bool ParseInt(const char **cursor, int *value)
{
	SkipWhitespace(cursor);

	char *end = nullptr;
	long parsed = strtol(*cursor, &end, 10);
	if (end == *cursor) {
		return false;
	}

	*value = static_cast<int>(parsed);
	*cursor = end;
	return true;
}

bool ParsePointPairs(const char *json, CurveProfiles::CurveData *curve)
{
	const char *points = strstr(json, "\"points\"");
	if (points == nullptr || curve == nullptr) {
		return false;
	}

	const char *cursor = strchr(points, '[');
	if (cursor == nullptr) {
		return false;
	}

	cursor++;
	curve->point_count = 0U;

	while (*cursor != '\0') {
		SkipWhitespace(&cursor);
		if (*cursor == ']') {
			break;
		}

		if (*cursor != '[') {
			++cursor;
			continue;
		}

		++cursor;
		if (curve->point_count >= ARRAY_SIZE(curve->points)) {
			return false;
		}

		CurvePoint point = {};
		if (!ParseInt(&cursor, &point.x)) {
			return false;
		}

		if (!SeekChar(&cursor, ',')) {
			return false;
		}
		++cursor;

		if (!ParseInt(&cursor, &point.y)) {
			return false;
		}

		if (!SeekChar(&cursor, ']')) {
			return false;
		}
		++cursor;

		curve->points[curve->point_count++] = point;
	}

	return curve->point_count >= 2U;
}

} // namespace

CurveProfiles::CurveProfiles()
	: adc_to_voltage_{}, voltage_to_percent_{}, percent_to_pwm_{}, percent_to_rpm_{}
{
}

int CurveProfiles::EnsureDefaultFiles() const
{
	for (size_t i = 0; i < kDefaultCurveAssetCount; ++i) {
		if (storage::PathExists(kDefaultCurveAssets[i].path)) {
			continue;
		}

		int rc = storage::WriteTextFile(kDefaultCurveAssets[i].path,
						reinterpret_cast<const char *>(kDefaultCurveAssets[i].data),
						kDefaultCurveAssets[i].size);
		if (rc != 0) {
			return rc;
		}
	}

	return 0;
}

int CurveProfiles::LoadCurveFile(const char *path, CurveData *curve) const
{
	char json[768];
	size_t json_len = 0U;
	int rc = storage::ReadTextFile(path, json, sizeof(json), &json_len);
	if (rc != 0) {
		return rc;
	}

	return ParsePointPairs(json, curve) ? 0 : -EINVAL;
}

int CurveProfiles::Init()
{
	int rc = EnsureDefaultFiles();
	if (rc != 0) {
		return rc;
	}

	rc = LoadCurveFile(kAdcToVoltagePath, &adc_to_voltage_);
	if (rc != 0) {
		return rc;
	}

	rc = LoadCurveFile(kVoltageToPercentPath, &voltage_to_percent_);
	if (rc != 0) {
		return rc;
	}

	rc = LoadCurveFile(kPercentToPwmPath, &percent_to_pwm_);
	if (rc != 0) {
		return rc;
	}

	return LoadCurveFile(kPercentToRpmPath, &percent_to_rpm_);
}

int CurveProfiles::EvaluateCurve(const CurveData &curve, int input)
{
	if (curve.point_count == 0U) {
		return 0;
	}

	if (input <= curve.points[0].x) {
		return curve.points[0].y;
	}

	for (size_t i = 1U; i < curve.point_count; ++i) {
		if (input <= curve.points[i].x) {
			const CurvePoint &a = curve.points[i - 1U];
			const CurvePoint &b = curve.points[i];
			const int dx = b.x - a.x;
			if (dx <= 0) {
				return b.y;
			}

			const int64_t dy = static_cast<int64_t>(b.y - a.y);
			const int64_t nx = static_cast<int64_t>(input - a.x);
			return a.y + static_cast<int>((dy * nx) / dx);
		}
	}

	return curve.points[curve.point_count - 1U].y;
}

int CurveProfiles::EvaluateCurveInverse(const CurveData &curve, int output)
{
	if (curve.point_count == 0U) {
		return 0;
	}

	if (output <= curve.points[0].y) {
		return curve.points[0].x;
	}

	for (size_t i = 1U; i < curve.point_count; ++i) {
		if (output <= curve.points[i].y) {
			const CurvePoint &a = curve.points[i - 1U];
			const CurvePoint &b = curve.points[i];
			const int dy = b.y - a.y;
			if (dy <= 0) {
				return b.x;
			}

			const int64_t dx = static_cast<int64_t>(b.x - a.x);
			const int64_t ny = static_cast<int64_t>(output - a.y);
			return a.x + static_cast<int>((dx * ny) / dy);
		}
	}

	return curve.points[curve.point_count - 1U].x;
}

int32_t CurveProfiles::EvaluateAdcRawToVoltage(int32_t adc_raw) const
{
	return EvaluateCurve(adc_to_voltage_, adc_raw);
}

uint8_t CurveProfiles::EvaluateVoltageToPercent(int32_t voltage_mv) const
{
	return static_cast<uint8_t>(CLAMP(EvaluateCurve(voltage_to_percent_, voltage_mv), 0, 100));
}

uint8_t CurveProfiles::EvaluateAdcRawToPercent(int32_t adc_raw) const
{
	return EvaluateVoltageToPercent(EvaluateAdcRawToVoltage(adc_raw));
}

uint8_t CurveProfiles::EvaluatePercentToPwm(uint8_t percent) const
{
	return static_cast<uint8_t>(CLAMP(EvaluateCurve(percent_to_pwm_, percent), 0, 100));
}

int32_t CurveProfiles::EvaluatePercentToRpm(uint8_t percent) const
{
	return EvaluateCurve(percent_to_rpm_, percent);
}

uint8_t CurveProfiles::EvaluateRpmToPercent(int32_t rpm) const
{
	return static_cast<uint8_t>(CLAMP(EvaluateCurveInverse(percent_to_rpm_, rpm), 0, 100));
}

const char *CurveProfiles::GetAdcToVoltagePath()
{
	return kAdcToVoltagePath;
}

const char *CurveProfiles::GetVoltageToPercentPath()
{
	return kVoltageToPercentPath;
}

const char *CurveProfiles::GetPercentToPwmPath()
{
	return kPercentToPwmPath;
}

const char *CurveProfiles::GetPercentToRpmPath()
{
	return kPercentToRpmPath;
}

} // namespace fanctl::curves
