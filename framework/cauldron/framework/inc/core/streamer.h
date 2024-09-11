#pragma once
#include "core/framework.h"
#include <mutex>

namespace cauldron
{
    enum class StreamTimingType : uint32_t
    {
        BeginFrame = 0,
        EndFrame,
        EncodeFrame,
    };

    class Streamer
    {
    public:
        Streamer() = default;

        /**
         * @brief   Initialize the encoder. Sets up the encoder and opens Media-over-QUIC publisher process.
         */
        void Init();

        /**
         * @brief   Shutdown the encoder. Closes the encoder and publisher.
         */
        void Shutdown();

        /**
         * @brief   Encode the encoder target. This is the main function that is called to encode the encoder target.
         * @param backbufferIndex   The index of the back buffer to encode.
         * @param frameIndex        The index of the frame to encode.
         */
        void Encode(uint8_t currentBackBufferIndex, int64_t frameIndex);

        /**
         * @brief   Execute the copy command. This is the main function that is called to copy the encoder target to the swap chain.
         * @param pCmdList  The command list to execute the copy command on.
         */
        void ExecuteCopyCommand(CommandList* pCmdList);

        /**
         * @brief   Report timing information. This function is used to report timing information.
         * @param type  The type of timing information.
         * @param time  The time to report.
         */
        void ReportTiming(StreamTimingType type, int64_t frameIndex)
        {
            auto timestamp = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::high_resolution_clock::now().time_since_epoch());
            if (frameIndex >= 0)
            {
                std::lock_guard<std::mutex> lock(m_timingMutex);
                m_timingInfo[frameIndex][static_cast<uint32_t>(type)] = timestamp;
            }
        }

    private:
        /**
         * @brief   Create the encoder and publisher.
         * This function sets up the encoder and opens the Media-over-QUIC publisher process.
         */
        void CreateEncoderAndPublisher();

        /**
         * @brief   Terminate the publisher process.
         * This function is used to terminate the process.
         */
        void Streamer::TerminatePublisher();

        // Timing information
        std::map<int64_t, std::map<uint32_t, std::chrono::microseconds>> m_timingInfo;

        // Encoder/Publisher process
        HANDLE              m_hPipe = INVALID_HANDLE_VALUE;
        PROCESS_INFORMATION m_piPublisher;
        std::atomic<bool>   m_isPipeOpen = false;
        ResolutionInfo      m_resolutionInfo;

        // Syncronization
        std::mutex              m_encodeMutex;
        std::mutex              m_bufferMutex;
        std::mutex              m_timingMutex;
        std::atomic<uint8_t>    m_frameIndex = 0;
        std::condition_variable m_bufferCV;
    };
}  // namespace cauldron
