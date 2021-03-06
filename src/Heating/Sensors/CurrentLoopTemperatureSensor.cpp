/*
 * LinearAdcTemperatureSensor.cpp
 *
 *  Created on: 8 Jun 2017
 *      Author: David
 */

#include "CurrentLoopTemperatureSensor.h"
#include "RepRap.h"
#include "Platform.h"
#include "GCodes/GCodeBuffer.h"

const uint32_t MCP3204_Frequency = 1000000;		// maximum for MCP3204 is 1MHz @ 2.7V, will be slightly higher at 3.3V

// The MCP3204 samples input data on the rising edge and changes the output data on the rising edge.
const uint8_t MCP3204_SpiMode = SPI_MODE_0;

// Define the minimum interval between readings
const uint32_t MinimumReadInterval = 100;		// minimum interval between reads, in milliseconds

CurrentLoopTemperatureSensor::CurrentLoopTemperatureSensor(unsigned int channel)
	: SpiTemperatureSensor(channel, "Current Loop", channel - FirstLinearAdcChannel, MCP3204_SpiMode, MCP3204_Frequency),
	  tempAt4mA(DefaultTempAt4mA), tempAt20mA(DefaultTempAt20mA)
{
	CalcDerivedParameters();
}

// Initialise the linear ADC
void CurrentLoopTemperatureSensor::Init()
{
	InitSpi();

	for (unsigned int i = 0; i < 3; ++i)		// try 3 times
	{
		TryGetLinearAdcTemperature();
		if (lastResult == TemperatureError::success)
		{
			break;
		}
		delay(MinimumReadInterval);
	}

	lastReadingTime = millis();

	if (lastResult != TemperatureError::success)
	{
		reprap.GetPlatform().MessageF(ErrorMessage, "Failed to initialise daughter board ADC: %s\n", TemperatureErrorString(lastResult));
	}
}

// Configure this temperature sensor
bool CurrentLoopTemperatureSensor::Configure(unsigned int mCode, unsigned int heater, GCodeBuffer& gb, const StringRef& reply, bool& error)
{
	if (mCode == 305)
	{
		bool seen = false;
		gb.TryGetFValue('L', tempAt4mA, seen);
		gb.TryGetFValue('H', tempAt20mA, seen);
		TryConfigureHeaterName(gb, seen);

		if (seen)
		{
			CalcDerivedParameters();
		}
		else if (!gb.Seen('X'))
		{
			CopyBasicHeaterDetails(heater, reply);
			reply.catf(", temperature range %.1f to %.1fC", (double)tempAt4mA, (double)tempAt20mA);
		}
	}
	return false;
}

TemperatureError CurrentLoopTemperatureSensor::GetTemperature(float& t)
{
	if (!inInterrupt() && millis() - lastReadingTime >= MinimumReadInterval)
	{
		TryGetLinearAdcTemperature();
	}

	t = lastTemperature;
	return lastResult;
}

void CurrentLoopTemperatureSensor::CalcDerivedParameters()
{
	minLinearAdcTemp = tempAt4mA - 0.25 * (tempAt20mA - tempAt4mA);
	linearAdcDegCPerCount = (tempAt20mA - minLinearAdcTemp) / 4096.0;
}

// Try to get a temperature reading from the linear ADC by doing an SPI transaction
void CurrentLoopTemperatureSensor::TryGetLinearAdcTemperature()
{
	// The MCP3204 waits for a high input input bit before it does anything. Call this clock 1.
	// The next input bit it high for single-ended operation, low for differential. This is clock 2.
	// The next 3 input bits are the channel selection bits. These are clocks 3..5.
	// Clock 6 produces a null bit on its trailing edge, which is read by the processor on clock 7.
	// Clocks 7..18 produce data bits B11..B0 on their trailing edges, which are read by the MCU on the leading edges of clocks 8-19.
	// If we supply further clocks, then clocks 18..29 are the same data but LSB first, omitting bit 0.
	// Clocks 30 onwards will be zeros.
	// So we need to use at least 19 clocks. We round this up to 24 clocks, and we check that the extra 5 bits we receive are the 5 least significant data bits in reverse order.

	static const uint8_t adcData[] = { 0xC0, 0x00, 0x00 };		// start bit, single ended, channel 0
	uint32_t rawVal;
	lastResult = DoSpiTransaction(adcData, 3, rawVal);
	//debugPrintf("ADC data %u\n", rawVal);

	if (lastResult == TemperatureError::success)
	{
		const uint32_t adcVal1 = (rawVal >> 5) & ((1 << 13) - 1);
		const uint32_t adcVal2 = ((rawVal & 1) << 5) | ((rawVal & 2) << 3) | ((rawVal & 4) << 1) | ((rawVal & 8) >> 1) | ((rawVal & 16) >> 3) | ((rawVal & 32) >> 5);
		if (adcVal1 >= 4096 || adcVal2 != (adcVal1 & ((1 << 6) - 1)))
		{
			lastResult = TemperatureError::badResponse;
		}
		else
		{
			lastTemperature = minLinearAdcTemp + (linearAdcDegCPerCount * (float)adcVal1);
		}
	}
}

// End
