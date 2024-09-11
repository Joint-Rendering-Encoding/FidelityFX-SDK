#include "core/streamer.h"
#include "render/swapchain.h"
#include "d3d12.h"

#include <sstream>

constexpr auto MOQ_PUB_PROCESS      = ".\\moq-pub.exe --name live ";
constexpr auto FFMPEG_PROCESS_BEGIN = "ffmpeg -fflags nobuffer -y -f rawvideo -pixel_format rgba -video_size ";
constexpr auto FFMPEG_PROCESS_INPUT = "-i - -c:v libx264 -preset ultrafast -tune zerolatency -pix_fmt yuv420p -vf setpts=N -video_track_timescale 1";
constexpr auto FFMPEG_PROCESS_END   = "-f mp4 -movflags empty_moov+frag_every_frame+separate_moof+omit_tfhd_offset -";

namespace cauldron
{
    void Streamer::Init()
    {
        CreateEncoderAndPublisher();
    }

    void Streamer::Shutdown()
    {
        if (m_isPipeOpen)
        {
            _pclose(ffmpegPipe);
            m_isPipeOpen = false;
        }
    }

    void Streamer::CreateEncoderAndPublisher()
    {
        // FFmpeg command to run with pipe input in Windows
        std::wstringstream command;
        command << FFMPEG_PROCESS_BEGIN;
        command << GetConfig()->Width << "x" << GetConfig()->Height << " ";
        command << FFMPEG_PROCESS_INPUT << " " << FFMPEG_PROCESS_END;
        command << " | " << MOQ_PUB_PROCESS << GetConfig()->StreamingInfo.Host << ":" << GetConfig()->StreamingInfo.Port;

        // Open FFmpeg process with a pipe
        Log::Write(LOGLEVEL_TRACE, L"Starting publisher process...");
        ffmpegPipe = _popen(WStringToString(command.str()).c_str(), "wb");
        CauldronAssert(ASSERT_CRITICAL, ffmpegPipe != nullptr, L"Failed to open FFmpeg pipe");
        m_isPipeOpen = true;
    }

    void Streamer::Encode(uint8_t backbufferIndex)
    {
        if (!m_isPipeOpen)
            return;

        // Wait until the slot is empty and it's our turn
        {
            std::unique_lock<std::mutex> lock(m_bufferMutex);
            m_bufferCV.wait(lock, [&] { return m_frameIndex == backbufferIndex; });
            CauldronAssert(ASSERT_CRITICAL, m_frameIndex == backbufferIndex, L"Frame index mismatch");
        }

        // Copy the encoder target to system memory
        uint8_t* pFrameData  = nullptr;
        auto     releaseFunc = GetFramework()->GetSwapChain()->CopyReadbackToMemory(&pFrameData, backbufferIndex);
        CauldronAssert(ASSERT_CRITICAL, pFrameData != nullptr, L"Failed to copy encoder target data");

        {
            // Only one thread can pipe data to FFmpeg at a time
            std::lock_guard<std::mutex> lock(m_encodeMutex);

            // Backbuffer might've queued up while the pipe was closed
            if (!m_isPipeOpen || !pFrameData)
                goto fail;

            int      tries        = 0;
            uint32_t frameSize    = GetConfig()->Width * GetConfig()->Height * GetResourceFormatStride(ResourceFormat::RGBA8_UINT);
            size_t   bytesWritten = 0;

            while (tries < 3)
            {
                // Write the frame data to FFmpeg pipe
                bytesWritten = fwrite(pFrameData, 1, frameSize, ffmpegPipe);

                // Check if the pipe is still open
                if (bytesWritten == frameSize)
                {
                    fflush(ffmpegPipe);
                    goto release;
                }

                // If the pipe is closed, try to reopen it
                _pclose(ffmpegPipe);
                CreateEncoderAndPublisher();
                tries++;
            }

        fail:
            CauldronWarning(L"Failed to pipe frame data to FFmpeg, disabling streaming...");
            m_isPipeOpen = false;
            _pclose(ffmpegPipe);

        release:
            // Free the frame data
            releaseFunc();

            // Update the frame index and notify other threads
            m_frameIndex = (backbufferIndex + 1) % GetFramework()->GetSwapChain()->GetBackBufferCount();
            m_bufferCV.notify_all();
        }
    }

    void Streamer::ExecuteCopyCommand(CommandList* pCmdList)
    {
        if (!m_isPipeOpen)
            return;

        // Copy the encoder target to the swap chain
        GetFramework()->GetSwapChain()->CopySwapChainToReadback(pCmdList);
    }

}  // namespace cauldron
