#include "core/streamer.h"
#include "render/swapchain.h"
#include "d3d12.h"

#include <sstream>

constexpr auto MOQ_PUB_PROCESS      = ".\\moq-pub.exe --name live ";
constexpr auto FFMPEG_PROCESS_BEGIN = "ffmpeg -fflags nobuffer -y -f rawvideo -pixel_format rgba -video_size ";
constexpr auto FFMPEG_PROCESS_INPUT = "-i - -c:v libx264 -preset ultrafast -tune zerolatency -pix_fmt yuv420p -vf setpts=N -video_track_timescale 1";
constexpr auto FFMPEG_PROCESS_END   = "-f mp4 -movflags empty_moov+frag_every_frame+separate_moof+omit_tfhd_offset -";

using namespace std::experimental;

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
            TerminatePublisher();
            m_bufferCV.notify_all();
        }

        // Dump the timing information
        {
            std::lock_guard<std::mutex> lock(m_timingMutex);

            // Define the output path
            const std::wstring pid = std::to_wstring(GetCurrentProcessId());
            std::wstringstream fileName;
            fileName << L"timing_" << pid << L".json";
            filesystem::path outputPath(GetConfig()->BenchmarkPath);

            // Defensive, in case path doesn't exist
            if (!outputPath.empty())
                filesystem::create_directories(outputPath);

            // Write the timing information to a file
            outputPath /= fileName.str();
            std::wofstream file(outputPath.c_str(), std::ios::out);
            if (!file)
            {
                char errmsg[256];
                strerror_s(errmsg, 256, errno);
                Log::Write(LOGLEVEL_FATAL, L"Opening timing file failed: %ls", StringToWString(errmsg).c_str());
            }

            json outputData = json::object();
            for (const auto& ti : m_timingInfo)
            {
                json frameData = json::array();
                for (const auto& timing : ti.second)
                {
                    json timingData    = json::object();
                    timingData["type"] = static_cast<uint32_t>(timing.first);
                    timingData["time"] = timing.second.count();
                    frameData.push_back(timingData);
                }
                outputData[std::to_string(ti.first)] = frameData;
            }

            file << StringToWString(outputData.dump());

            // Close the file
            file.close();
        }
    }

    void Streamer::CreateEncoderAndPublisher()
    {
        SECURITY_ATTRIBUTES saAttr;
        saAttr.nLength              = sizeof(SECURITY_ATTRIBUTES);
        saAttr.bInheritHandle       = TRUE;  // Allow the handle to be inherited
        saAttr.lpSecurityDescriptor = NULL;

        // Create a pipe for the child process's STDIN
        HANDLE hChildStdinRd, hChildStdinWr;
        CauldronThrowOnFail(CreatePipe(&hChildStdinRd, &hChildStdinWr, &saAttr, 0));
        SetHandleInformation(hChildStdinWr, HANDLE_FLAG_INHERIT, 0);  // Ensure the write handle is not inherited

        // Set up the STARTUPINFO structure
        STARTUPINFO si = {sizeof(si)};
        si.hStdInput   = hChildStdinRd;  // Redirect STDIN
        si.dwFlags |= STARTF_USESTDHANDLES;

        // Set up the PROCESS_INFORMATION structure
        PROCESS_INFORMATION pi;
        ZeroMemory(&pi, sizeof(PROCESS_INFORMATION));

        // FFmpeg command to run with pipe input in Windows
        std::stringstream command;
        command << "cmd.exe /C ";
        command << FFMPEG_PROCESS_BEGIN;
        command << GetConfig()->Width << "x" << GetConfig()->Height << " ";
        command << FFMPEG_PROCESS_INPUT << " " << FFMPEG_PROCESS_END;
        command << " | " << MOQ_PUB_PROCESS << WStringToString(GetConfig()->StreamingInfo.Host) << ":" << GetConfig()->StreamingInfo.Port;

        // Create the child process
        if (!CreateProcess(NULL, const_cast<LPSTR>(command.str().c_str()), NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi))
        {
            CloseHandle(hChildStdinRd);
            CloseHandle(hChildStdinWr);
            CauldronCritical(L"Failed to create FFmpeg process");
        }

        // Close the read end of the pipe in the parent process
        CloseHandle(hChildStdinRd);

        // Return the write handle to STDIN and the process information
        m_isPipeOpen  = true;
        m_hPipe       = hChildStdinWr;
        m_piPublisher = pi;
    }

    void Streamer::TerminatePublisher()
    {
        // Terminate the process
        m_isPipeOpen = false;
        TerminateProcess(m_piPublisher.hProcess, 0);
        WaitForSingleObject(m_piPublisher.hProcess, INFINITE);
        CloseHandle(m_piPublisher.hProcess);
        CloseHandle(m_piPublisher.hThread);
    }

    void Streamer::Encode(uint8_t backbufferIndex, int64_t frameIndex)
    {
        // Don't waste time if the pipe is closed
        if (!m_isPipeOpen)
            return;

        // Record when the frame has finished rendering
        ReportTiming(StreamTimingType::EndFrame, frameIndex);

        // Wait until the slot is empty and it's our turn
        {
            std::unique_lock<std::mutex> lock(m_bufferMutex);
            m_bufferCV.wait(lock, [&] { return m_frameIndex == backbufferIndex || !m_isPipeOpen; });
        }

        // Again, don't waste time if the pipe is closed
        if (!m_isPipeOpen)
            return;

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
            DWORD   bytesWritten = 0;

            while (tries < 3)
            {
                // Write the frame data to FFmpeg pipe
                WriteFile(m_hPipe, pFrameData, frameSize, &bytesWritten, NULL);

                // Check if the pipe is still open
                if (bytesWritten == frameSize)
                {
                    FlushFileBuffers(m_hPipe);
                    ReportTiming(StreamTimingType::EncodeFrame, frameIndex);
                    goto release;
                }

                // If the pipe is closed, try to reopen it
                TerminatePublisher();
                CreateEncoderAndPublisher();
                tries++;
            }

        fail:
            CauldronWarning(L"Failed to pipe frame data to FFmpeg, disabling streaming...");
            TerminatePublisher();

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
