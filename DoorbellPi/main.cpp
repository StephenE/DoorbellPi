#include <wiringPi.h>

#include <chrono>
#include <iostream>
#include <thread>

int main()
{
    // #ToDo: Read these inputs from a command line
    int const c_InputPin = 17;
    int const c_OutputPin = 27;
    bool const c_PullUpInput = true;
    std::chrono::milliseconds c_MinimumTriggerDuration = std::chrono::milliseconds(200);
    std::chrono::milliseconds c_OutputDuration = std::chrono::milliseconds(1000);

#   // #ToDo: Exit early if not running with root access

    // Setup wiringPi for GPIO access. We need to be root so we can set the pull up/down state
    wiringPiSetupGpio();

    pinMode(c_InputPin, INPUT);
    pullUpDnControl(c_InputPin, c_PullUpInput ? PUD_UP : PUD_DOWN);
    pinMode(c_OutputPin, OUTPUT);

    std::chrono::time_point<std::chrono::system_clock> triggerAtTime = std::chrono::time_point<std::chrono::system_clock>::max();
    while (true)
    {
        int const inputState = digitalRead(c_InputPin);

        if (inputState != 0)
        {
            digitalWrite(c_OutputPin, LOW);
            triggerAtTime = std::chrono::time_point<std::chrono::system_clock>::max();
            std::this_thread::sleep_for(std::chrono::milliseconds(10));

            // #ToDo: Instead of using a busy loop, we should register a callback for notification of state changes
        }
        else if(triggerAtTime < std::chrono::system_clock::now())
        {
            digitalWrite(c_OutputPin, HIGH);
            std::this_thread::sleep_for(c_OutputDuration);
        }
        else if (triggerAtTime == std::chrono::time_point<std::chrono::system_clock>::max())
        {
            triggerAtTime = std::chrono::system_clock::now() + c_MinimumTriggerDuration;
        }
    }
    return 0;
}