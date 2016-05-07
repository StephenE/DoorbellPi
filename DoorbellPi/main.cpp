#include <curl/curl.h>
#include <wiringPi.h>

#include <chrono>
#include <iostream>
#include <thread>

int main()
{
    // #ToDo: Read these inputs from a config file
    int const c_InputPin = 17;
    int const c_OutputPin = 27;
    bool const c_PullUpInput = true;
    std::chrono::milliseconds const c_MinimumTriggerDuration = std::chrono::milliseconds(200);
    std::chrono::milliseconds const c_OutputDuration = std::chrono::milliseconds(1000);
    std::string const c_CloudServiceUrl = "http://192.168.0.17:8888";
    bool const c_VerboseLibcurl = false;

#   // #ToDo: Exit early if not running with root access
    std::cout << "Starting up with root access" << std::endl;

    // Setup libcurl
    curl_global_init(CURL_GLOBAL_SSL);
    
    // Verify libcurl
    auto const* libcurlVersionInfo = curl_version_info(CURLVERSION_NOW);
    bool const libcurlHasSslSupport = libcurlVersionInfo->features & CURL_VERSION_SSL;
    if (!libcurlHasSslSupport)
    {
        std::cerr << "Libcurl does not have SSL support" << std::endl;
        return 1;
    }

    std::cout << "SSL support verified: " << libcurlVersionInfo->ssl_version << std::endl;

    // Proof of concept: Make a GET request using libcurl
    auto libcurlEasyHandle = curl_easy_init();
    curl_easy_setopt(libcurlEasyHandle, CURLOPT_URL, c_CloudServiceUrl.c_str());
#ifndef _DEBUG
    if (c_VerboseLibcurl)
    {
        curl_easy_setopt(libcurlEasyHandle, CURLOPT_VERBOSE, 1);
    }
#endif

    std::cout << "Attempting to register with cloud service" << std::endl;
    auto libcurlRegisterOutcome = curl_easy_perform(libcurlEasyHandle);
    std::cout << std::endl;

    if (libcurlRegisterOutcome != CURLE_OK)
    {
        std::cerr << "Failed to register with cloud service" << std::endl;
        return 1;
    }

    // Setup wiringPi for GPIO access. We need to be root so we can set the pull up/down state
    wiringPiSetupGpio();

    // Setup GPIO pins
    std::cout << "Registered with cloud service" << std::endl;
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

            // #ToDo: Notify cloud service of doorbell press
        }
        else if (triggerAtTime == std::chrono::time_point<std::chrono::system_clock>::max())
        {
            triggerAtTime = std::chrono::system_clock::now() + c_MinimumTriggerDuration;
        }
    }

    // Cleanup libcurl
    std::cout << "Cleaning up libcurl" << std::endl;
    curl_global_cleanup();

    return 0;
}