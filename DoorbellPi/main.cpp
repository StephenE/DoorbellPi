#include <curl/curl.h>
#include <wiringPi.h>

#include <chrono>
#include <iostream>
#include <memory>
#include <thread>
#include <signal.h>
#include <syslog.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

static bool s_RunProgram = true;

void exitHandler(int s)
{
    switch (s)
    {
    case SIGHUP:
    {
        break;
    }
    default:
    {
        syslog(LOG_NOTICE, "Handling signal %d by exiting", s);
        s_RunProgram = false;
        break;
    }
    }
}


int main()
{
    // #ToDo: Read these inputs from a config file
    int const c_InputPin = 17;
    int const c_OutputPin = 21; // On early revision PI, 27 was actually 21
    bool const c_PullUpInput = true;
    std::chrono::milliseconds const c_MinimumTriggerDuration = std::chrono::milliseconds(200);
    std::chrono::milliseconds const c_OutputDuration = std::chrono::milliseconds(20);
    std::string const c_CloudServiceUrl = "http://192.168.0.17:8888";
    bool const c_VerboseLibcurl = false;

    // Fork, so that the parent process can exit
    pid_t const processId = fork();
    if (processId < 0)
    {
        std::cerr << "Failed to fork" << std::endl;
        return -1;
    }
    else if (processId > 0)
    {
        // This is the parent process and a child has been created, so we can now exit
        return 0;
    }

    // Start logging to syslog
    setlogmask(LOG_UPTO(LOG_NOTICE));
    openlog("DoorbellPi", LOG_CONS | LOG_PID | LOG_NDELAY, LOG_DAEMON);
    syslog(LOG_NOTICE, "Starting DoorbellPi daemon with process id %d", getpid());

    // Only the forked child process will reach this point. Finalise our environment
    umask(0);

    pid_t const signatureId = setsid();
    if (signatureId < 0)
    {
        syslog(LOG_ERR, "Child process failed to change signature id");
        return -1;
    }

    int changeWorkingDirectoryResult = chdir("/");
    if (changeWorkingDirectoryResult < 0)
    {
        syslog(LOG_ERR, "Child process failed to change working directory");
        return -1;
    }

    close(STDIN_FILENO);
    close(STDOUT_FILENO);
    close(STDERR_FILENO);

    // Register to handle the "exit" signal
    struct sigaction signalInteruptHandler;

    signalInteruptHandler.sa_handler = exitHandler;
    sigemptyset(&signalInteruptHandler.sa_mask);
    signalInteruptHandler.sa_flags = 0;

    sigaction(SIGINT, &signalInteruptHandler, NULL);

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

    /*auto libcurlRegisterOutcome = curl_easy_perform(libcurlEasyHandle);
    std::cout << std::endl;

    if (libcurlRegisterOutcome != CURLE_OK)
    {
        std::cerr << "Failed to register with cloud service" << std::endl;
        return 1;
    }*/

    // Setup wiringPi for GPIO access. We need to be root so we can set the pull up/down state
    wiringPiSetupGpio();

    // Setup GPIO pins
    pinMode(c_InputPin, INPUT);
    pullUpDnControl(c_InputPin, c_PullUpInput ? PUD_UP : PUD_DOWN);
    pinMode(c_OutputPin, OUTPUT);

    std::chrono::time_point<std::chrono::system_clock> triggerAtTime = std::chrono::time_point<std::chrono::system_clock>::max();
    while (s_RunProgram)
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
            syslog(LOG_NOTICE, "Doorbell press detected!");

            for (int externalCounter = 0; externalCounter < 2; ++externalCounter)
            {
                for (int counter = 0; counter < 2; ++counter)
                {
                    for (int internalCounter = 0; internalCounter < 5; ++internalCounter)
                    {
                        digitalWrite(c_OutputPin, HIGH);
                        std::this_thread::sleep_for(c_OutputDuration);

                        digitalWrite(c_OutputPin, LOW);
                        std::this_thread::sleep_for(c_OutputDuration);
                    }

                    std::this_thread::sleep_for(std::chrono::milliseconds(500));
                }

                std::this_thread::sleep_for(std::chrono::seconds(2));
            }

            // #ToDo: Notify cloud service of doorbell press
        }
        else if (triggerAtTime == std::chrono::time_point<std::chrono::system_clock>::max())
        {
            triggerAtTime = std::chrono::system_clock::now() + c_MinimumTriggerDuration;
        }
    }

    // Cleanup libcurl
    syslog(LOG_NOTICE, "Cleaning up libcurl");
    curl_global_cleanup();

    syslog(LOG_NOTICE, "Closing PowerPi.Monitor daemon");
    closelog();

    return 0;
}