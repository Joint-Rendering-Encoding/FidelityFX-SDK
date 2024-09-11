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

        // Copy the encoder target to system memory
        uint8_t* pFrameData = nullptr;
        GetFramework()->GetSwapChain()->CopyReadbackToMemory(&pFrameData, backbufferIndex);
        CauldronAssert(ASSERT_CRITICAL, pFrameData != nullptr, L"Failed to copy encoder target data");

        {
            // Only one thread can pipe data to FFmpeg at a time
            std::lock_guard<std::mutex> lock(m_mutex);

            // Backbuffer might've queued up while the pipe was closed
            if (!m_isPipeOpen || !pFrameData)
                goto fail;

            int tries = 0;
        repeat:
            // Write the frame data to FFmpeg pipe
            uint32_t frameSize    = GetConfig()->Width * GetConfig()->Height * GetResourceFormatStride(ResourceFormat::RGBA8_UINT);
            size_t   bytesWritten = fwrite(pFrameData, 1, frameSize, ffmpegPipe);

            // Check if the pipe is still open
            if (bytesWritten != frameSize)
            {
                _pclose(ffmpegPipe);

                if (tries++ > 3)
                {
                    m_isPipeOpen = false;
                    CauldronWarning(L"FFmpeg pipe closed unexpectedly. Streaming will be disabled.");
                    goto fail;
                }

                CreateEncoderAndPublisher();
                goto repeat;
            }

            fflush(ffmpegPipe);
        }

    fail:
        // Free the frame data
        free(pFrameData);
    }

    void Streamer::ExecuteCopyCommand(CommandList* pCmdList)
    {
        if (!m_isPipeOpen)
            return;

        // Copy the encoder target to the swap chain
        GetFramework()->GetSwapChain()->CopySwapChainToReadback(pCmdList);
    }

}  // namespace cauldron
