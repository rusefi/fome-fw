#include "pch.h"

#include "injector_model.h"
#include "fuel_computer.h"

void InjectorModelBase::prepare() {
	float flowRatio = getInjectorFlowRatio();

	// "large pulse" flow rate
	m_massFlowRate = flowRatio * getBaseFlowRate();
	m_deadtime = getDeadtime();

	if (getNonlinearMode() == INJ_FordModel) {
		m_smallPulseFlowRate = flowRatio * getSmallPulseFlowRate();
		m_smallPulseBreakPoint = getSmallPulseBreakPoint();

		// amount added to small pulses to correct for the "kink" from low flow region
		m_smallPulseOffset = 1000 * ((m_smallPulseBreakPoint / m_massFlowRate) - (m_smallPulseBreakPoint / m_smallPulseFlowRate));
	}
}

constexpr float convertToGramsPerSecond(float ccPerMinute) {
	return ccPerMinute * (fuelDensity / 60.f);
}

float InjectorModelWithConfig::getBaseFlowRate() const {
	if (engineConfiguration->injectorFlowAsMassFlow) {
		return m_cfg->flow;
	} else {
		return convertToGramsPerSecond(m_cfg->flow);
	}
}

float InjectorModelWithConfig::getMinimumPulse() const {
	return engineConfiguration->minimumInjectionPulseWidth;
}

float InjectorModelPrimary::getSmallPulseFlowRate() const {
	return engineConfiguration->fordInjectorSmallPulseSlope;
}

float InjectorModelPrimary::getSmallPulseBreakPoint() const {
	// convert milligrams -> grams
	return 0.001f * engineConfiguration->fordInjectorSmallPulseBreakPoint;
}

InjectorNonlinearMode InjectorModelPrimary::getNonlinearMode() const {
	return engineConfiguration->injectorNonlinearMode;
}

float InjectorModelSecondary::getSmallPulseFlowRate() const {
	// not supported on second bank
	return 0;
}

float InjectorModelSecondary::getSmallPulseBreakPoint() const {
	// not supported on second bank
	return 0;
}

InjectorNonlinearMode InjectorModelSecondary::getNonlinearMode() const {
	// nonlinear not supported on second bank
	return InjectorNonlinearMode::INJ_None;
}

expected<float> InjectorModelWithConfig::getFuelDifferentialPressure() const {
	auto map = Sensor::get(SensorType::Map);
	auto baro = Sensor::get(SensorType::BarometricPressure);

	float baroKpa = baro.Value;
	if (!baro || baro.Value > 120 || baro.Value < 50) {
		baroKpa = 101.325f;
	}

	switch (engineConfiguration->injectorCompensationMode) {
		case ICM_FixedRailPressure:
			// Add barometric pressure, as "fixed" really means "fixed pressure above atmosphere"
			return
				  engineConfiguration->fuelReferencePressure
				+ baroKpa
				- map.value_or(101.325);
		case ICM_SensedRailPressure: {
			if (!Sensor::hasSensor(SensorType::FuelPressureInjector)) {
				warning(ObdCode::OBD_Fuel_Pressure_Sensor_Missing, "Fuel pressure compensation is set to use a pressure sensor, but none is configured.");
				return unexpected;
			}

			auto fps = Sensor::get(SensorType::FuelPressureInjector);

			// TODO: what happens when the sensor fails?
			if (!fps) {
				return unexpected;
			}

			switch (engineConfiguration->fuelPressureSensorMode) {
				case FPM_Differential:
					// This sensor directly measures delta-P, no math needed!
					return fps.Value;
				case FPM_Gauge:
					if (!map) {
						return unexpected;
					}

					return fps.Value + baroKpa - map.Value;
				case FPM_Absolute:
				default:
					if (!map) {
						return unexpected;
					}

					return fps.Value - map.Value;
			}
		} default: return unexpected;
	}
}

float InjectorModelWithConfig::getInjectorFlowRatio() {
	// Compensation disabled, use reference flow.
	if (engineConfiguration->injectorCompensationMode == ICM_None) {
		return 1.0f;
	}

	float referencePressure = engineConfiguration->fuelReferencePressure;

	if (referencePressure < 50) {
		// impossibly low fuel ref pressure
		firmwareError(ObdCode::OBD_PCM_Processor_Fault, "Impossible fuel reference pressure: %f", referencePressure);

		return 1.0f;
	}

	expected<float> diffPressure = getFuelDifferentialPressure();

	// If sensor failed, best we can do is disable correction
	if (!diffPressure) {
		return 1.0f;
	}

	pressureDelta = diffPressure.Value;

	// Somehow pressure delta is less than 0, assume failed sensor and return default flow
	if (pressureDelta <= 0) {
		return 1.0f;
	}

	pressureRatio = pressureDelta / referencePressure;
	// todo: live data model?
	float flowRatio = sqrtf(pressureRatio);

	// TODO: should the flow ratio be clamped?
	return flowRatio;
}

float InjectorModelWithConfig::getDeadtime() const {
	return interpolate2d(
		Sensor::get(SensorType::BatteryVoltage).value_or(VBAT_FALLBACK_VALUE),
		m_cfg->battLagCorrBins,
		m_cfg->battLagCorr
	);
}

float InjectorModelBase::getInjectionDuration(float fuelMassGram) const {
	if (fuelMassGram <= 0) {
		// If 0 mass, don't do any math, just skip the injection.
		return 0.0f;
	}

	// Get the no-offset duration
	float baseDuration = getBaseDurationImpl(fuelMassGram);

	// Clamp to minimum injection pulse
	baseDuration = std::max(baseDuration, getMinimumPulse());

	// Add deadtime offset
	return baseDuration + m_deadtime;
}

float InjectorModelBase::getFuelMassForDuration(floatms_t duration) const {
	// Convert from ms -> grams
	return duration * m_massFlowRate * 0.001f;
}

float InjectorModelBase::getBaseDurationImpl(float fuelMassGram) const {
	floatms_t baseDuration = fuelMassGram / m_massFlowRate * 1000;

	switch (getNonlinearMode()) {
	case INJ_FordModel:
		if (fuelMassGram < m_smallPulseBreakPoint) {
			// Small pulse uses a different slope, and adds the "zero fuel pulse" offset
			return (fuelMassGram / m_smallPulseFlowRate * 1000) + m_smallPulseOffset;
		}

		// large pulse uses base duration
		break;
	case INJ_PolynomialAdder:
		return correctInjectionPolynomial(baseDuration);
	case INJ_SmallPulseAdder:
		if (baseDuration < engineConfiguration->applyNonlinearBelowPulse) {
			return baseDuration +
				interpolate2d(
					baseDuration,
					config->smallPulseAdderBins,
					config->smallPulseAdderValues
				);
		}

		// large pulse uses base duration
		break;
	case INJ_None:
	default:
		// no correction, use base duration
		break;
	}

	return baseDuration;
}

float InjectorModelBase::correctInjectionPolynomial(float baseDuration) const {
	if (baseDuration > engineConfiguration->applyNonlinearBelowPulse) {
		// Large pulse, skip correction.
		return baseDuration;
	}

	auto& is = engineConfiguration->injectorCorrectionPolynomial;
	float xi = 1;

	float adder = 0;

	// Add polynomial terms, starting with x^0
	for (size_t i = 0; i < efi::size(is); i++) {
		adder += is[i] * xi;
		xi *= baseDuration;
	}

	return baseDuration + adder;
}

InjectorModelWithConfig::InjectorModelWithConfig(const injector_s* const cfg)
	: m_cfg(cfg)
{
}

InjectorModelPrimary::InjectorModelPrimary()
	: InjectorModelWithConfig(&engineConfiguration->injector)
{
}

// TODO: actual separate config for second bank!
InjectorModelSecondary::InjectorModelSecondary()
	: InjectorModelWithConfig(&engineConfiguration->injectorSecondary)
{
}
