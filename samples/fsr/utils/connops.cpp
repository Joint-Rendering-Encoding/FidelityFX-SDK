#include "connops.h"

void Connection::HandleRelay(SOCKET socket)
{
    int bytesReceived;

    // This function handles responses to relay requests
    do
    {
        // Get the message type
        MessageType type = MessageType::Invalid;
        bytesReceived    = recv(socket, (char*)&type, sizeof(type), 0);

        switch (type)
        {
        case MessageType::Reconfigure:
        {
            // TODO: Reconfigure the render resolution
            uint32_t width  = 0;
            uint32_t height = 0;
            recv(socket, (char*)&width, sizeof(width), 0);
            recv(socket, (char*)&height, sizeof(height), 0);

            // Inform the relay that the renderer is ready to proceed
            type = MessageType::Ack;
            send(socket, (char*)&type, sizeof(type), 0);
            break;
        }
        case MessageType::Continue:
            // Relay wants to continue to receive the same resolution
            {
                // Enter read lock
                std::lock_guard<std::mutex> lock(mQueue.getReadLock());

                // Get the next ready buffer
                FSRData* buffer = mQueue.getNextReadyBuffer();

                if (!buffer)
                {
                    // Renderer is either not ready or has no data to send
                    type = MessageType::NotReady;
                    send(socket, (char*)&type, sizeof(type), 0);
                    break;
                }

                // Renderer has data to send
                type = MessageType::Data;
                send(socket, (char*)&type, sizeof(type), 0);

                // Inform the relay of the buffer size
                size_t bufferSize = mQueue.getBufferSize();
                send(socket, (char*)&bufferSize, sizeof(size_t), 0);

                // Wait for the relay to acknowledge the buffer size
                recv(socket, (char*)&type, sizeof(type), 0);

                if (type != MessageType::Proceed)
                {
                    // This means the relay has sent a Reconfigure message but this buffer was sent before acknowledging the Reconfigure message
                    mQueue.releaseBuffer(buffer);
                    break;
                }

                // Send the buffer
                CauldronAssert(ASSERT_CRITICAL, bufferSize <= UINT32_MAX, L"Buffer size is too large");
                send(socket, (char*)buffer->get(), (int)bufferSize, 0);

                // Release the buffer
                mQueue.releaseBuffer(buffer);
                break;
            }

        default:
            CauldronWarning(L"Invalid message type received. %d", (int)type);
            break;
        }

    } while (bytesReceived > 0);

    closesocket(socket);
}

void Connection::HandleRenderer(SOCKET socket)
{
    int bytesReceived;

    // Initiate the message chain
    // TODO: Properly send the reconfigure message
    MessageType type   = MessageType::Reconfigure;
    uint32_t    width  = 1920;
    uint32_t    height = 1080;

    send(socket, (char*)&type, sizeof(type), 0);
    send(socket, (char*)&width, sizeof(width), 0);
    send(socket, (char*)&height, sizeof(height), 0);

    do
    {
        // Get the message type
        MessageType type = MessageType::Invalid;
        bytesReceived    = recv(socket, (char*)&type, sizeof(type), 0);

        switch (type)
        {
        case MessageType::Ack:
        case MessageType::NotReady:
            // Renderer is acknowledging the last message or is not ready to send data
            // Do nothing
            break;
        case MessageType::Data:
            // Renderer is actively sending data
            {
                // Enter write lock
                std::lock_guard<std::mutex> lock(mQueue.getWriteLock());

                // Get the total size from the renderer
                size_t bufferSize = 0;
                recv(socket, (char*)&bufferSize, sizeof(bufferSize), 0);

                // Try to get a buffer
                FSRData* buffer = mQueue.getNextEmptyBuffer(bufferSize);

                if (!buffer)
                {
                    // Relay is not ready to receive data
                    type = MessageType::NotReady;
                    send(socket, (char*)&type, sizeof(type), 0);
                    break;
                }

                // Relay is ready to receive data
                type = MessageType::Proceed;
                send(socket, (char*)&type, sizeof(type), 0);

                // Receive the buffer
                CauldronAssert(ASSERT_CRITICAL, bufferSize <= UINT32_MAX, L"Buffer size is too large");
                recv(socket, (char*)buffer->get(), (int)bufferSize, 0);

                // Mark the buffer as ready
                mQueue.markBufferReady(buffer);
                break;
            }
            break;
        default:
            CauldronWarning(L"Invalid message type received. %d", (int)type);
            break;
        }

        // Request more data
        type = MessageType::Continue;
        send(socket, (char*)&type, sizeof(type), 0);
    } while (bytesReceived > 0);

    closesocket(socket);
}

void Connection::AcceptClients()
{
    while (true)
    {
        // Accept connection
        SOCKET socket = accept(mSocket, NULL, NULL);
        if (socket == INVALID_SOCKET)
        {
            closesocket(mSocket);
            WSACleanup();
            CauldronCritical(L"Accept failed.");
        }

        // Handle client in a separate thread
        std::thread thread(&Connection::HandleRelay, this, socket);
        thread.detach();  // Detach the thread since we don't join it
    }
}

void Connection::run_server()
{
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0)
    {
        CauldronCritical(L"WSAStartup failed.");
    }

    // Create socket
    mSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (mSocket == INVALID_SOCKET)
    {
        WSACleanup();
        CauldronCritical(L"Socket creation failed.");
    }

    // Disable Nagle's algorithm
    int flag = 1;
    setsockopt(mSocket, IPPROTO_TCP, TCP_NODELAY, (char*)&flag, sizeof(int));

    // Bind socket
    sockaddr_in serverAddr{};
    serverAddr.sin_family      = AF_INET;
    serverAddr.sin_addr.s_addr = inet_addr(mAddress.c_str());
    serverAddr.sin_port        = htons(std::stoi(mPort));  // Convert port to integer

    if (bind(mSocket, (sockaddr*)&serverAddr, sizeof(serverAddr)) == SOCKET_ERROR)
    {
        closesocket(mSocket);
        WSACleanup();
        CauldronCritical(L"Bind failed.");
    }

    // Listen for incoming connections
    if (listen(mSocket, SOMAXCONN) == SOCKET_ERROR)
    {
        closesocket(mSocket);
        WSACleanup();
        CauldronCritical(L"Listen failed.");
    }

    std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>> converter;
    std::wstring                                           wideAddress = converter.from_bytes(mAddress);
    std::wstring                                           widePort    = converter.from_bytes(mPort);

    Log::Write(LOGLEVEL_INFO, L"Server listening on %s:%s", wideAddress.c_str(), widePort.c_str());

    // Start a thread for accepting client connections
    std::thread acceptThread(&Connection::AcceptClients, this);
    acceptThread.detach();  // Detach the thread since we don't join it
}

void Connection::run_client()
{
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0)
    {
        CauldronCritical(L"WSAStartup failed.");
    }

    // Create socket
    mSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (mSocket == INVALID_SOCKET)
    {
        WSACleanup();
        CauldronCritical(L"Socket creation failed.");
    }

    // Disable Nagle's algorithm
    int flag = 1;
    setsockopt(mSocket, IPPROTO_TCP, TCP_NODELAY, (char*)&flag, sizeof(int));

    // Resolve server address
    sockaddr_in serverAddr{};
    serverAddr.sin_family      = AF_INET;
    serverAddr.sin_addr.s_addr = inet_addr(mAddress.c_str());
    serverAddr.sin_port        = htons(std::stoi(mPort));

    // Connect to server
    if (connect(mSocket, (sockaddr*)&serverAddr, sizeof(serverAddr)) == SOCKET_ERROR)
    {
        closesocket(mSocket);
        WSACleanup();
        CauldronCritical(L"Connect failed.");
    }

    // Start a thread for handling the renderer
    std::thread thread(&Connection::HandleRenderer, this, mSocket);
    thread.detach();  // Detach the thread since we don't join it
}
