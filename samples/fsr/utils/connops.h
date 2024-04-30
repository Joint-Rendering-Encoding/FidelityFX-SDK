#pragma once

#include "validation_remap.h"
#include "../common.h"
#include <mutex>
#include <thread>

using namespace cauldron;
using namespace common;

// Link with ws2_32.lib
#pragma comment(lib, "Ws2_32.lib")

#ifdef FSR_REMOTE_GPU_ONLY
// We don't need to allocate more than 2 buffers for GPU only because we are not using it anyway
// We keep it just because it would mean changing a lot of code :)
constexpr int DEFAULT_BUFLEN = 2;
#else
constexpr int DEFAULT_BUFLEN = 6;
#endif

enum class MessageType : uint32_t
{
    // Inform renderer to reconfigure render resolution
    Reconfigure = 0,
    // Inform renderer to continue sending data
    Continue,
    // Inform renderer to proceed with the last message
    Proceed,

    // Inform relay that renderer is acknowledging the last message
    Ack,
    // Inform relay that renderer is not ready to send data
    NotReady,
    // Inform relay that renderer is actively sending data
    Data,

    // Error message
    Error,

    // Invalid message type
    Invalid,
};

enum class BufferState : uint32_t
{
    // Buffer is empty
    Empty = 0,
    // Buffer has been assigned
    Allocated,
    // Buffer is ready to be sent
    Ready,
};

class BufferRing
{
private:
    std::vector<FSRData>     mBuffers;
    std::vector<BufferState> mState;

    std::mutex mStateLock;
    std::mutex mWriteLock;
    std::mutex mReadLock;

    size_t mBufferSize;
    size_t mReadIndex;
    size_t mWriteIndex;

    void updateState(size_t index, BufferState state)
    {
        mState[index] = state;
    }

    size_t getBufferIndex(FSRData* buffer)
    {
        auto it = std::find_if(mBuffers.begin(), mBuffers.end(), [&](const FSRData& ptr) { return ptr.get() == buffer->get(); });
        if (it != mBuffers.end())
        {
            return std::distance(mBuffers.begin(), it);
        }

        return -1;
    }

public:
    BufferRing()
        : mBuffers(DEFAULT_BUFLEN)
        , mState(DEFAULT_BUFLEN)
        , mBufferSize(0)
    {
        reset(mBufferSize);
    }

    void reset(size_t size)
    {
        // Lock all the locks
        std::lock_guard<std::mutex> s(mStateLock);
        std::lock_guard<std::mutex> w(mWriteLock);
        std::lock_guard<std::mutex> r(mReadLock);

        // Set the buffer size
        mBufferSize = size;

        for (size_t i = 0; i < mBuffers.size(); i++)
        {
            updateState(i, BufferState::Empty);
            mBuffers[i].reset(new uint8_t[mBufferSize]);
        }
    }

    /**
     * @brief Get the write lock for the buffer ring
     */
    std::mutex& getWriteLock()
    {
        return mWriteLock;
    }

    /**
     * @brief Get the read lock for the buffer ring
     */
    std::mutex& getReadLock()
    {
        return mReadLock;
    }

    size_t getBufferSize() const
    {
        return mBufferSize;
    }

    void markBufferReady(FSRData* buffer)
    {
        std::lock_guard<std::mutex> lock(mStateLock);

        size_t index = getBufferIndex(buffer);
        if (index != -1)
        {
            updateState(index, BufferState::Ready);
        }
    }

    void releaseBuffer(FSRData* buffer)
    {
        std::lock_guard<std::mutex> lock(mStateLock);

        size_t index = getBufferIndex(buffer);
        if (index != -1)
        {
            updateState(index, BufferState::Empty);
        }
    }

    bool nextBufferReady()
    {
        std::lock_guard<std::mutex> lock(mStateLock);

        // Clamp the index to the buffer size
        size_t index = mReadIndex % mState.size();

        // Check if the buffer is ready
        return mState[index] == BufferState::Ready;
    }

    FSRData* getNextReadyBuffer()
    {
        std::lock_guard<std::mutex> lock(mStateLock);

        // Clamp the index to the buffer size
        size_t index = mReadIndex % mState.size();

        // Check if the buffer is ready
        if (mState[index] == BufferState::Ready)
        {
            mReadIndex++;
            return &mBuffers[index];
        }

        return nullptr;  // Current read index is not ready
    }

    bool nextBufferEmpty()
    {
        std::lock_guard<std::mutex> lock(mStateLock);

        // Clamp the index to the buffer size
        size_t index = mWriteIndex % mState.size();

        // Check if the buffer is empty
        return mState[index] == BufferState::Empty;
    }

    FSRData* getNextEmptyBuffer(size_t requestedSize)
    {
        std::lock_guard<std::mutex> lock(mStateLock);

        // See if our buffer size is large enough
        if (mBufferSize != requestedSize)
        {
            return nullptr;  // Buffer size is not equal to the requested size, it will be reset later
        }

        // Clamp the index to the buffer size
        size_t index = mWriteIndex % mState.size();

        // Check if the buffer is empty
        if (mState[index] == BufferState::Empty)
        {
            mWriteIndex++;
            updateState(index, BufferState::Allocated);
            return &mBuffers[index];
        }

        return nullptr;  // Current write index is not empty
    }
};

class Connection
{
public:
    Connection(const std::string& address, const std::string& port)
        : mAddress(address)
        , mPort(port)
        , mSocket(INVALID_SOCKET)
        , mQueue()
    {
    }

    ~Connection()
    {
        if (mSocket != INVALID_SOCKET)
        {
            closesocket(mSocket);
            WSACleanup();
        }
    }

    void run_server();
    void run_client();

    void reconfigure(const ResolutionInfo& resInfo)
    {
        mResInfo     = resInfo;
        mReconfigure = true;
    }

    bool shouldReconfigure()
    {
        return mReconfigure;
    }

    ResolutionInfo getResInfo()
    {
        mReconfigure = false;
        return mResInfo;
    }

    BufferRing& getQueue()
    {
        return mQueue;
    }

private:
    std::string mAddress;
    std::string mPort;
    SOCKET      mSocket;

    BufferRing mQueue;

    // Reconfigure state
    ResolutionInfo    mResInfo{2560, 1440, 2560, 1440};
    std::atomic<bool> mReconfigure = false;

    void HandleRelay(SOCKET socket);
    void HandleRenderer(SOCKET socket);
    void AcceptClients();
};
