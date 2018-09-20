//#include <curl/curl.h>
#include <wiringPi.h>

#include <chrono>
#include <iostream>
#include <memory>
#include <netdb.h>
#include <thread>
#include <signal.h>
#include <string.h>
#include <syslog.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "external/flic/client_protocol_packets.h"

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

namespace Rings
{
    void Once(int const outputPin, std::chrono::milliseconds const strikeDuration)
    {
        digitalWrite(outputPin, HIGH);
        std::this_thread::sleep_for(strikeDuration);

        digitalWrite(outputPin, LOW);
        std::this_thread::sleep_for(strikeDuration);
    }

    void Classic(int const outputPin, std::chrono::milliseconds const strikeDuration)
    {
        for (int externalCounter = 0; externalCounter < 2; ++externalCounter)
        {
            for (int counter = 0; counter < 2; ++counter)
            {
                for (int internalCounter = 0; internalCounter < 5; ++internalCounter)
                {
                    Once(outputPin, strikeDuration);
                }

                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            }

            std::this_thread::sleep_for(std::chrono::seconds(2));
        }
    }
}

namespace Flic
{
    struct ButtonAddress 
    {
        static int const c_AddressLength = 6;
        uint8_t addr[c_AddressLength];

        ButtonAddress() 
        {
            memset(addr, 0, c_AddressLength);
        }

        ButtonAddress(const ButtonAddress& o) 
        {
            *this = o;
        }

        ButtonAddress(const uint8_t* a) 
        {
            *this = a;
        }

        ButtonAddress& operator=(const ButtonAddress& o) 
        {
            memcpy(addr, o.addr, c_AddressLength);
            return *this;
        }
        ButtonAddress& operator=(const uint8_t* a) 
        {
            memcpy(addr, a, c_AddressLength);
            return *this;
        }

        bool operator==(const ButtonAddress& o) const { return memcmp(addr, o.addr, c_AddressLength) == 0; }
        bool operator!=(const ButtonAddress& o) const { return memcmp(addr, o.addr, c_AddressLength) != 0; }
    };

    class Connection
    {
    public:
        int const c_InvalidHandle = -1;

        bool Connect(std::string const& hostname, in_port_t const port, ButtonAddress const& buttonAddress)
        {
            m_SocketHandle = socket(AF_INET, SOCK_STREAM, 0);
            if (m_SocketHandle < 0)
            {
                syslog(LOG_NOTICE, "Failed to create socket");
                m_SocketHandle = c_InvalidHandle;
                return false;
            }

            hostent const* serverHostEnt = gethostbyname(hostname.c_str());
            if (serverHostEnt == NULL)
            {
                syslog(LOG_NOTICE, "Failed to resolve '%s' [%d]", hostname.c_str(), errno);
                Disconnect();
                return false;
            }

            sockaddr_in serverAddress;
            memset(&serverAddress, 0, sizeof(serverAddress));
            serverAddress.sin_family = AF_INET;
            memcpy(&serverAddress.sin_addr.s_addr, serverHostEnt->h_addr, serverHostEnt->h_length);
            serverAddress.sin_port = htons(port);

            if (connect(m_SocketHandle, (struct sockaddr*)&serverAddress, sizeof(serverAddress)) < 0)
            {
                syslog(LOG_NOTICE, "Failed to connect to '%s:%d' [%s]", hostname.c_str(), port, GetErrorCodeString().c_str());
                Disconnect();
                return false;
            }

            syslog(LOG_NOTICE, "Connected to '%s:%d'", hostname.c_str(), port);

            FlicClientProtocol::CmdCreateConnectionChannel cmd;
            memcpy(cmd.bd_addr, buttonAddress.addr, ButtonAddress::c_AddressLength);
            cmd.conn_id = m_ConnectionId;
            cmd.latency_mode = FlicClientProtocol::NormalLatency;
            cmd.auto_disconnect_time = 5;
            if (!WritePacket(reinterpret_cast<uint8_t const*>(&cmd), sizeof(cmd)))
            {
                syslog(LOG_NOTICE, "Error %d sending CmdCreateConnectionChannel'", errno);
                Disconnect();
                return false;
            }

            return true;
        }

        bool IsConnected() const
        {
            return m_SocketHandle != c_InvalidHandle;
        }

        void Disconnect()
        {
            if (m_SocketHandle != c_InvalidHandle)
            {
                FlicClientProtocol::CmdRemoveConnectionChannel cmd;
                cmd.conn_id = m_ConnectionId;
                WritePacket(reinterpret_cast<uint8_t const*>(&cmd), sizeof(cmd));

                close(m_SocketHandle);
                m_SocketHandle = c_InvalidHandle;
            }
        }

        bool WaitForRing()
        {
            unsigned char lengthBuffer[2];
            int bytesInLengthBuffer = 0;
            while (true)
            {
                int nbytes = read(m_SocketHandle, lengthBuffer + bytesInLengthBuffer, 2 - bytesInLengthBuffer);
                if (nbytes < 0) 
                {
                    syslog(LOG_NOTICE, "Error %d reading from socket'", errno);
                    Disconnect();
                    return false;
                }

                // Wait until we have two bytes, which should be the length
                bytesInLengthBuffer += nbytes;
                if (bytesInLengthBuffer != 2)
                {
                    continue;
                }
                bytesInLengthBuffer = 0;

                // Read the specified amount of data into a temporary buffer
                int const packetLength = lengthBuffer[0] | (lengthBuffer[1] << 8);
                std::unique_ptr<unsigned char[]> readBuffer = std::make_unique<unsigned char[]>(packetLength);
                int readPosition = 0;
                int bytesLeft = packetLength;
                syslog(LOG_NOTICE, "Recieved packet header of %d bytes from socket'", packetLength);

                while (bytesLeft > 0)
                {
                    nbytes = read(m_SocketHandle, readBuffer.get() + readPosition, bytesLeft);
                    if (nbytes < 0) 
                    {
                        syslog(LOG_NOTICE, "Error %d reading from socket'", errno);
                        Disconnect();
                        return false;
                    }

                    readPosition += nbytes;
                    bytesLeft -= nbytes;
                }

                uint8_t const opCode = *static_cast<uint8_t const*>(readBuffer.get());
                syslog(LOG_NOTICE, "Got a message of type %d from socket'", opCode);

                switch(opCode)
                {
                    case EVT_BUTTON_UP_OR_DOWN_OPCODE:
                    {
                        auto eventData = reinterpret_cast<FlicClientProtocol::EvtButtonEvent const*>(readBuffer.get());
                        if (eventData->time_diff < 10 && eventData->click_type == FlicClientProtocol::ButtonDown)
                        {
                            syslog(LOG_NOTICE, "Saw a button click event of click type %d", eventData->click_type);
                            return true;
                        }
                        else
                        {
                            syslog(LOG_NOTICE, "Saw a button click event, but it was %d seconds old and a click type of %d'", eventData->time_diff, eventData->click_type);
                        }
                        break;
                    }
                    case EVT_BUTTON_CLICK_OR_HOLD_OPCODE:
                    case EVT_BUTTON_SINGLE_OR_DOUBLE_CLICK_OPCODE:
                    case EVT_BUTTON_SINGLE_OR_DOUBLE_CLICK_OR_HOLD_OPCODE:
                    {
                        // Ignore the other button op codes. We just care about up/down events
                        break;
                    }
                    default:
                    {
                        // Ignored
                        break;
                    }
                }
            }
        }

    private:
        bool WritePacket(uint8_t const* buf, int len)
        {
            uint8_t const lengthPrefix[2] = { static_cast<uint8_t>(len & 0xff), static_cast<uint8_t>(len >> 8) };
            if (WriteBuffer(lengthPrefix, 2))
            {
                return WriteBuffer(buf, len);
            }
            else
            {
                return false;
            }
        }

        bool WriteBuffer(uint8_t const* buf, int len)
        {
            int pos = 0;
            int left = len;
            while (left) 
            {
                int res = write(m_SocketHandle, buf + pos, left);
                if (res < 0) 
                {
                    if (errno == EINTR) 
                    {
                        return false;
                    }
                }
                pos += res;
                left -= res;
            }

            return true;
        }

        std::string GetErrorCodeString() const
        {
            int const errorCode = errno;
            char const* errorMessage = strerror(errorCode);
            return std::string(errorMessage);
        }

        int m_SocketHandle = c_InvalidHandle;
        int const m_ConnectionId = 1;
    };
}

int main()
{
    // #ToDo: Read these inputs from a config file
    int const c_OutputPin = 23;
    //std::chrono::milliseconds const c_MinimumTriggerDuration = std::chrono::milliseconds(200);
    std::chrono::milliseconds const c_OutputDuration = std::chrono::milliseconds(20);
    std::string const c_CloudServiceUrl = "http://192.168.0.17:8888";
    std::string const c_FlicdHost = "localhost";
    int const c_FlicdPort = 5551;
    // Remember: the display order is big endian but the address needs to be little endian
    uint8_t const c_ButtonAddressArray[] = { 0x40, 0xBF, 0x73, 0xDA, 0xE4, 0x80 };
    Flic::ButtonAddress const c_ButtonAddress(c_ButtonAddressArray);
    //bool const c_VerboseLibcurl = false;

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

#ifdef ENABLE_LIBCURL
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
#endif // ENABLE_LIBCURL

    // Setup wiringPi for GPIO access. We need to be root so we can set the pull up/down state
    wiringPiSetupGpio();

    // Setup GPIO pins
    pinMode(c_OutputPin, OUTPUT);

    // Connect to flic deamon
    Flic::Connection doorbell;
    if(doorbell.Connect(c_FlicdHost, c_FlicdPort, c_ButtonAddress))
    {
        while (doorbell.IsConnected())
        {
            bool ring = doorbell.WaitForRing();
            if (!ring)
            {
                // No action required?
            }
            else
            {
                // #ToDo: Select ring based upon time of day/night
                bool const doNightRing = false;
                if(doNightRing)
                {
                    Rings::Once(c_OutputPin, c_OutputDuration);
                }
                else
                {
                    Rings::Classic(c_OutputPin, c_OutputDuration);
                }

                // #ToDo: Notify cloud service of doorbell press

                // #ToDo: Ignore further rings for a short period
            }
        }
    }

#ifdef ENABLE_LIBCURL
    // Cleanup libcurl
    syslog(LOG_NOTICE, "Cleaning up libcurl");
    curl_global_cleanup();
#endif // ENABLE_LIBCURL

    syslog(LOG_NOTICE, "Closing PowerPi.Monitor daemon");
    closelog();

    return 0;
}