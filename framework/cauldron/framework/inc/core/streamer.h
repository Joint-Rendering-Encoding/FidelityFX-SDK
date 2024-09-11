#pragma once
#include "core/framework.h"
#include <mutex>

namespace cauldron
{
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
         */
        void Encode(uint8_t currentBackBufferIndex);

        /**
         * @brief   Execute the copy command. This is the main function that is called to copy the encoder target to the swap chain.
         * @param pCmdList  The command list to execute the copy command on.
         */
        void Streamer::ExecuteCopyCommand(CommandList* pCmdList);

    private:
        /**
         * @brief   Create the encoder and publisher.
         * This function sets up the encoder and opens the Media-over-QUIC publisher process.
         */
        void CreateEncoderAndPublisher();

        // Encoder/Publisher process
        FILE*             ffmpegPipe   = nullptr;
        std::atomic<bool> m_isPipeOpen = false;
        ResolutionInfo    m_resolutionInfo;

        // Syncronization
        std::mutex              m_encodeMutex;
        std::mutex              m_bufferMutex;
        std::atomic<uint8_t>    m_frameIndex = 0;
        std::condition_variable m_bufferCV;
    };
}  // namespace cauldron
