#include "FanController.hpp"
#include "Settings.h"
#include <chrono>
#include <cstdint>
#include <stdint.h>
#include "Utilities.hpp"

FanController::FanController(const PinName& tachometerPin_, const PinName& pwmOutputPin_)
    : tachometerPin(tachometerPin_)
    , pwmOutputPin(pwmOutputPin_)
    // set Main Thread with normal priority and 2048 bytes stack size
    , mainThread(FanControllerPriority, 2048, nullptr, "FanController") 
    , pulseStretchingThread(PulseStretchingPriority, 1024, nullptr, "PulseStretching")
{
    /* set PWM frequency higher than maximum tachometer frequency to ensure PWM pulse is not 
    mistaken as a tachometer reading during bandpass filtering */
    pwmOutputPin.period_us(PWMOutPeriod_us);

    // set tachometer ISR to bandpass or pulse stretching
    if(Settings::Fan::PulseStretchingActive)
    {
        // attach ISR to falling edge of tacho signal
        tachometerPin.fall(callback(this, &FanController::pulseStretchingISR));
    }
    else 
    {
        // attach ISR to both rising and falling edge of tacho signal
        tachometerPin.rise(callback(this, &FanController::bandpassFilterISR));
        tachometerPin.fall(callback(this, &FanController::bandpassFilterISR));
    }
    
}


void FanController::init()
{
    // start main thread and pulse stretching thread (if required)
    mainThread.start(callback(this, &FanController::MainThread));

    if(Settings::Fan::PulseStretchingActive)
        pulseStretchingThread.start(callback(this, &FanController::pulseStretching));
}


void FanController::deinit()
{
    // stops fan and terminates thread
    pwmOutputPin.write(0);
    mainThread.terminate();
}


void FanController::MainThread()
{
    while(true)
    {
        // calculate new speed
        calculateCurrentSpeed();

        // printf("Current Speed RPM: %u\n", currentSpeed_RPM);

        // // error is difference between desired speed and actual speed
        // float error = currentSpeed_RPM - desiredSpeed_RPM;
        
        // /* PWM Fan Output Calculated from PID */
        // static uint32_t previousTime = 0;
        // static uint32_t previousError = 0;
        // static float integralError = 0;

        // // calculate time elapsed since last call
        // uint64_t timeElapsed = mainTimer.elapsed_time().count();
        // // reset ISR timer
        // mainTimer.reset();
        // mainTimer.start();

        // // Do not calculate PID on first call since timeElapsed is undetermined
        // static bool firstCall = true;
        // if(!firstCall)
        // {
        //     firstCall = false;

        //     // calculate cumaltive error
        //     integralError += error * timeElapsed;

        //     // calculate change in error
        //     float derivativeError = (error - previousError)/timeElapsed;
        //     previousError = error;

        //     float PWM_Output = Settings::Fan::kp * (error + (Settings::Fan::ki * integralError) + (Settings::Fan::kd * derivativeError));

        //     if(PWM_Output > 1.0) PWM_Output = 1.0;
        //     else if (PWM_Output < 0.0) PWM_Output = 0.0;
        //     pwmOutputPin.write(PWM_Output);
        // }
        
        ThisThread::sleep_for(FanControlYieldTime);
    }
}


void FanController::bandpassFilterISR()
{
    /* In a given window, set by how often the method variables tachoCount and averagePulseTime_us 
    are reset (when calculateCurrentSpeed is called), the average time between pulses will be calculated */
    
    // calculate latest pulse time (between rising and falling edge)
    uint64_t pulseTime_us = ISRTimer.elapsed_time().count();
    
    // reset ISR timer
    ISRTimer.reset();
    ISRTimer.start();

    /* Bandpass Filter attenuates the tachometer frequencies that are not in range of the minimum and maximum frequencies, including PWM artifacts */
    // Pulse width is divided by two to account for both the rising and falling edge of a single pulse
    if((pulseTime_us > (MinTachoPulseWidth_us / 2)) && (pulseTime_us < (MaxTachoPulseWidth_us / 2)))
    {
        // ignore avg caluclation on first interrupt, since pulse time is unknown
        if (tachoCount != 0)
        {
            // weight latest pulse time with previous average pulse times
            averagePulseTime_us = ((averagePulseTime_us * (tachoCount-1)) + pulseTime_us) / tachoCount;
        }
        // increment tachometer counter
        tachoCount++;
    }
}


void FanController::pulseStretchingISR()
{
    /* In a given window, set by how often the method variables tachoCount and averagePulseTime_us 
    are reset (when calculateCurrentSpeed is called), the average time between pulses will be calculated */
    
    // calculate latest pulse time
    uint64_t pulseTime_us = ISRTimer.elapsed_time().count();
    
    // reset ISR timer
    ISRTimer.reset();
    ISRTimer.start();

    /* Pulse stretching ensures tachometer does not lose power and give false reading during pwm OFF cycle, by outputting an ocassional 100% duty cycle pulse*/
    if (pulseStretchingActive)
    {
        // ignore avg caluclation on first interrupt, since pulse time is unknown
        if (tachoCount != 0)
        {
            // weight latest pulse time with previous average pulse times
            averagePulseTime_us = ((averagePulseTime_us * (tachoCount-1)) + pulseTime_us) / tachoCount;
        }
        // increment tachometer counter
        tachoCount++;
    }
}


void FanController::pulseStretching()
{
    // set to default PWM frequency
    pwmOutputPin.period_ms(20);

    // calculate delays in advance to save computational time
    // twice the time period to ensure at least one complete tachometer pulse is measured
    const uint32_t activeDelay_ms = (2 * MaxTachoPulseWidth_us) / 1e3; 
    const uint32_t inactiveDelay_ms = activeDelay_ms * Settings::Fan::PulseStretchRatio;

    printf("active delay: %u", activeDelay_ms);
    printf("Inactive delay: %u", inactiveDelay_ms);

    // every x tachometer pulses, determined by PulsesPerPulseStretch, set duty cycle to 100% for one tachometer pulse width
    while (true)
    {
        // do not conduct pulse stretching if desired fan speed is zero
        if(desiredSpeed_RPM != 0)
        {
            // store current duty cycle
            float currentDutyCycle = pwmOutputPin.read();

            // set duty cycle to 100% for one duty cycle and enable tachometer reading
            pwmOutputPin.write(1.0);
            pulseStretchingActive = true;
            ThisThread::sleep_for(std::chrono::milliseconds(activeDelay_ms));

            // Go back to previous duty cycle for x pulses
            pulseStretchingActive = false;
            pwmOutputPin.write(currentDutyCycle);
        }
        // reset flag to mark pulse stretch as complete
        pulseStretchingComplete = true;
        ThisThread::sleep_for(std::chrono::milliseconds(inactiveDelay_ms));
    }
}


void FanController::calculateCurrentSpeed()
{
    // convert time between rising and falling edge of one pulse to speed in RPM 
    uint16_t currentSpeed_RPM_Temp = 60 / (Settings::Fan::TachoPulsesPerRev * 2 * averagePulseTime_us * usToS);

    // update only if pulseStretching has been completed, update speed
    if (pulseStretchingComplete) 
    {
        // reset flag
        pulseStretchingComplete = false;
        currentSpeed_RPM = currentSpeed_RPM_Temp;
    }

    // reset ISR variables
    tachoCount = 0;
    averagePulseTime_us = 0;
}


void FanController::setDesiredSpeed_RPM(const uint16_t speed)
{
    desiredSpeed_RPM = speed;
    float pwmOut = static_cast<float>(speed)/Settings::Fan::MaxSpeed_RPM;
    printf("PWM OUT: %f\n", pwmOut);
    pwmOutputPin.write(pwmOut);
}


void FanController::setDesiredSpeed_Percentage(const float speed)
{
    desiredSpeed_RPM = speed * Settings::Fan::MaxSpeed_RPM;
    pwmOutputPin.write(Utilities::map(speed, 0.0, 1.0, Settings::Fan::minPWMOut, 1.0));
}


uint16_t FanController::getCurrentSpeed_RPM() const
{
    return currentSpeed_RPM;
}

void FanController::setPWMOutFrequency_Hz(uint16_t frequency_Hz)
{
    uint16_t timePeriod_us = (1.0/frequency_Hz)*1e6;
    pwmOutputPin.period_us(timePeriod_us);
}