/*++

    Copyright (c) Microsoft Corporation.
    Licensed under the MIT License.

Abstract:

    QUIC Raw (i.e. DPDK or XDP) Datapath Implementation (User Mode)

--*/

#include "datapath_raw_linux.h"
#ifdef QUIC_CLOG
#include "datapath_raw_linux.c.clog.h"
#endif

#pragma warning(disable:4116) // unnamed type definition in parentheses
#pragma warning(disable:4100) // unreferenced formal parameter

CXPLAT_THREAD_CALLBACK(CxPlatRouteResolutionWorkerThread, Context);

_IRQL_requires_max_(PASSIVE_LEVEL)
void
CxPlatDataPathRouteWorkerUninitialize(
    _In_ CXPLAT_ROUTE_RESOLUTION_WORKER* Worker
    )
{
    Worker->Enabled = FALSE;
    CxPlatEventSet(Worker->Ready);

    //
    // Wait for the thread to finish.
    //
    if (Worker->Thread) {
        CxPlatThreadWait(&Worker->Thread);
        CxPlatThreadDelete(&Worker->Thread);
    }

    CxPlatEventUninitialize(Worker->Ready);
    CxPlatDispatchLockUninitialize(&Worker->Lock);
    CxPlatPoolUninitialize(&Worker->OperationPool);
    CXPLAT_FREE(Worker, QUIC_POOL_ROUTE_RESOLUTION_WORKER);
}

_IRQL_requires_max_(PASSIVE_LEVEL)
QUIC_STATUS
CxPlatDataPathRouteWorkerInitialize(
    _Inout_ CXPLAT_DATAPATH* DataPath
    )
{
    QUIC_STATUS Status;
    CXPLAT_ROUTE_RESOLUTION_WORKER* Worker =
        CXPLAT_ALLOC_NONPAGED(
            sizeof(CXPLAT_ROUTE_RESOLUTION_WORKER), QUIC_POOL_ROUTE_RESOLUTION_WORKER);
    if (Worker == NULL) {
        QuicTraceEvent(
            AllocFailure,
            "Allocation of '%s' failed. (%llu bytes)",
            "CXPLAT_DATAPATH",
            sizeof(CXPLAT_ROUTE_RESOLUTION_WORKER));
        Status = QUIC_STATUS_OUT_OF_MEMORY;
        goto Error;
    }

    Worker->Enabled = TRUE;
    CxPlatEventInitialize(&Worker->Ready, FALSE, FALSE);
    CxPlatDispatchLockInitialize(&Worker->Lock);
    CxPlatListInitializeHead(&Worker->Operations);

    CxPlatPoolInitialize(
        FALSE,
        sizeof(CXPLAT_ROUTE_RESOLUTION_OPERATION),
        QUIC_POOL_ROUTE_RESOLUTION_OPER,
        &Worker->OperationPool);

    CXPLAT_THREAD_CONFIG ThreadConfig = {
        CXPLAT_THREAD_FLAG_NONE,
        0,
        "RouteResolutionWorkerThread",
        CxPlatRouteResolutionWorkerThread,
        Worker
    };

    Status = CxPlatThreadCreate(&ThreadConfig, &Worker->Thread);
    if (QUIC_FAILED(Status)) {
        QuicTraceEvent(
            LibraryErrorStatus,
            "[ lib] ERROR, %u, %s.",
            Status,
            "CxPlatThreadCreate");
        goto Error;
    }

    DataPath->RouteResolutionWorker = Worker;

Error:
    if (QUIC_FAILED(Status)) {
        if (Worker != NULL) {
            CxPlatDataPathRouteWorkerUninitialize(Worker);
        }
    }
    return Status;
}

_IRQL_requires_max_(PASSIVE_LEVEL)
QUIC_STATUS
CxPlatDataPathInitialize(
    _In_ uint32_t ClientRecvContextLength,
    _In_opt_ const CXPLAT_UDP_DATAPATH_CALLBACKS* UdpCallbacks,
    _In_opt_ const CXPLAT_TCP_DATAPATH_CALLBACKS* TcpCallbacks,
    _In_opt_ QUIC_EXECUTION_CONFIG* Config,
    _Out_ CXPLAT_DATAPATH** NewDataPath
    )
{
    QUIC_STATUS Status = QUIC_STATUS_SUCCESS;
    const size_t DatapathSize = CxPlatDpRawGetDatapathSize(Config);
    BOOLEAN DpRawInitialized = FALSE;
    BOOLEAN SockPoolInitialized = FALSE;
    CXPLAT_FRE_ASSERT(DatapathSize > sizeof(CXPLAT_DATAPATH));

    UNREFERENCED_PARAMETER(TcpCallbacks);

    if (NewDataPath == NULL) {
        return QUIC_STATUS_INVALID_PARAMETER;
    }
    if (UdpCallbacks != NULL) {
        if (UdpCallbacks->Receive == NULL || UdpCallbacks->Unreachable == NULL) {
            return QUIC_STATUS_INVALID_PARAMETER;
        }
    }

    if (!CxPlatWorkersLazyStart(Config)) {
        return QUIC_STATUS_OUT_OF_MEMORY;
    }

    CXPLAT_DATAPATH* DataPath = CXPLAT_ALLOC_PAGED(DatapathSize, QUIC_POOL_DATAPATH);
    if (DataPath == NULL) {
        QuicTraceEvent(
            AllocFailure,
            "Allocation of '%s' failed. (%llu bytes)",
            "CXPLAT_DATAPATH",
            DatapathSize);
        return QUIC_STATUS_OUT_OF_MEMORY;
    }
    CxPlatZeroMemory(DataPath, DatapathSize);
    CXPLAT_FRE_ASSERT(CxPlatRundownAcquire(&CxPlatWorkerRundown));

    if (UdpCallbacks) {
        DataPath->UdpHandlers = *UdpCallbacks;
    }

    if (Config && (Config->Flags & QUIC_EXECUTION_CONFIG_FLAG_QTIP)) {
        DataPath->UseTcp = TRUE;
    }

    if (!CxPlatSockPoolInitialize(&DataPath->SocketPool)) {
        Status = QUIC_STATUS_OUT_OF_MEMORY;
        goto Error;
    }
    SockPoolInitialized = TRUE;

    Status = CxPlatDpRawInitialize(DataPath, ClientRecvContextLength, Config);
    if (QUIC_FAILED(Status)) {
        goto Error;
    }
    DpRawInitialized = TRUE;

    Status = CxPlatDataPathRouteWorkerInitialize(DataPath);
    if (QUIC_FAILED(Status)) {
        goto Error;
    }

    *NewDataPath = DataPath;
    DataPath = NULL;

Error:

    if (DataPath != NULL) {
#if DEBUG
        DataPath->Uninitialized = TRUE;
#endif
        if (DpRawInitialized) {
            CxPlatDpRawUninitialize(DataPath);
        } else {
            if (SockPoolInitialized) {
                CxPlatSockPoolUninitialize(&DataPath->SocketPool);
            }
            CXPLAT_FREE(DataPath, QUIC_POOL_DATAPATH);
            CxPlatRundownRelease(&CxPlatWorkerRundown);
        }
    }

    return Status;
}

_IRQL_requires_max_(PASSIVE_LEVEL)
void
CxPlatDataPathUninitialize(
    _In_ CXPLAT_DATAPATH* Datapath
    )
{
    if (Datapath != NULL) {
#if DEBUG
        CXPLAT_DBG_ASSERT(!Datapath->Freed);
        CXPLAT_DBG_ASSERT(!Datapath->Uninitialized);
        Datapath->Uninitialized = TRUE;
#endif
        CxPlatDataPathRouteWorkerUninitialize(Datapath->RouteResolutionWorker);
        CxPlatDpRawUninitialize(Datapath);
    }
}

_IRQL_requires_max_(PASSIVE_LEVEL)
void
CxPlatDataPathUninitializeComplete(
    _In_ CXPLAT_DATAPATH* Datapath
    )
{
#if DEBUG
    CXPLAT_DBG_ASSERT(!Datapath->Freed);
    CXPLAT_DBG_ASSERT(Datapath->Uninitialized);
    Datapath->Freed = TRUE;
#endif
    CxPlatSockPoolUninitialize(&Datapath->SocketPool);
    CXPLAT_FREE(Datapath, QUIC_POOL_DATAPATH);
    CxPlatRundownRelease(&CxPlatWorkerRundown);
}

_IRQL_requires_max_(PASSIVE_LEVEL)
void
CxPlatDataPathUpdateConfig(
    _In_ CXPLAT_DATAPATH* Datapath,
    _In_ QUIC_EXECUTION_CONFIG* Config
    )
{
    UNREFERENCED_PARAMETER(Datapath);
    UNREFERENCED_PARAMETER(Config);
}

_IRQL_requires_max_(DISPATCH_LEVEL)
uint32_t
CxPlatDataPathGetSupportedFeatures(
    _In_ CXPLAT_DATAPATH* Datapath
    )
{
    UNREFERENCED_PARAMETER(Datapath);
    return 0;
}

_IRQL_requires_max_(DISPATCH_LEVEL)
BOOLEAN
CxPlatDataPathIsPaddingPreferred(
    _In_ CXPLAT_DATAPATH* Datapath
    )
{
    UNREFERENCED_PARAMETER(Datapath);
    return FALSE;
}

_IRQL_requires_max_(PASSIVE_LEVEL)
_Success_(QUIC_SUCCEEDED(return))
QUIC_STATUS
CxPlatDataPathGetLocalAddresses(
    _In_ CXPLAT_DATAPATH* Datapath,
    _Outptr_ _At_(*Addresses, __drv_allocatesMem(Mem))
        CXPLAT_ADAPTER_ADDRESS** Addresses,
    _Out_ uint32_t* AddressesCount
    )
{
    UNREFERENCED_PARAMETER(Datapath);
    UNREFERENCED_PARAMETER(Addresses);
    UNREFERENCED_PARAMETER(AddressesCount);
    return QUIC_STATUS_NOT_SUPPORTED;
}

_IRQL_requires_max_(PASSIVE_LEVEL)
_Success_(QUIC_SUCCEEDED(return))
QUIC_STATUS
CxPlatDataPathGetGatewayAddresses(
    _In_ CXPLAT_DATAPATH* Datapath,
    _Outptr_ _At_(*GatewayAddresses, __drv_allocatesMem(Mem))
        QUIC_ADDR** GatewayAddresses,
    _Out_ uint32_t* GatewayAddressesCount
    )
{
    UNREFERENCED_PARAMETER(Datapath);
    UNREFERENCED_PARAMETER(GatewayAddresses);
    UNREFERENCED_PARAMETER(GatewayAddressesCount);
    return QUIC_STATUS_NOT_SUPPORTED;
}

void
CxPlatDataPathPopulateTargetAddress(
    _In_ QUIC_ADDRESS_FAMILY Family,
    _In_ ADDRINFO* AddrInfo,
    _Out_ QUIC_ADDR* Address
    )
{
    UNREFERENCED_PARAMETER(Family);
    UNREFERENCED_PARAMETER(AddrInfo);
    UNREFERENCED_PARAMETER(Address);
}

// ->CxPlat
_IRQL_requires_max_(PASSIVE_LEVEL)
QUIC_STATUS
CxPlatDataPathResolveAddress(
    _In_ CXPLAT_DATAPATH* Datapath,
    _In_z_ const char* HostName,
    _Inout_ QUIC_ADDR* Address
    )
{
    UNREFERENCED_PARAMETER(Datapath);
    UNREFERENCED_PARAMETER(HostName);
    UNREFERENCED_PARAMETER(Address);
    QUIC_STATUS Status = QUIC_STATUS_NOT_SUPPORTED;
    return Status;
}

_IRQL_requires_max_(PASSIVE_LEVEL)
QUIC_STATUS
CxPlatSocketCreateUdp(
    _In_ CXPLAT_DATAPATH* Datapath,
    _In_ const CXPLAT_UDP_CONFIG* Config,
    _Out_ CXPLAT_SOCKET** NewSocket
    )
{
    QUIC_STATUS Status = QUIC_STATUS_SUCCESS;

    *NewSocket = CXPLAT_ALLOC_PAGED(sizeof(CXPLAT_SOCKET), QUIC_POOL_SOCKET);
    if (*NewSocket == NULL) {
        QuicTraceEvent(
            AllocFailure,
            "Allocation of '%s' failed. (%llu bytes)",
            "CXPLAT_SOCKET",
            sizeof(CXPLAT_SOCKET));
        Status = QUIC_STATUS_OUT_OF_MEMORY;
        goto Error;
    }

    QuicTraceEvent(
        DatapathCreated,
        "[data][%p] Created, local=%!ADDR!, remote=%!ADDR!",
        *NewSocket,
        CASTED_CLOG_BYTEARRAY(Config->LocalAddress ? sizeof(*Config->LocalAddress) : 0, Config->LocalAddress),
        CASTED_CLOG_BYTEARRAY(Config->RemoteAddress ? sizeof(*Config->RemoteAddress) : 0, Config->RemoteAddress));

    CxPlatZeroMemory(*NewSocket, sizeof(CXPLAT_SOCKET));
    CxPlatRundownInitialize(&(*NewSocket)->Rundown);
    (*NewSocket)->Datapath = Datapath;
    (*NewSocket)->CallbackContext = Config->CallbackContext;
    (*NewSocket)->CibirIdLength = Config->CibirIdLength;
    (*NewSocket)->CibirIdOffsetSrc = Config->CibirIdOffsetSrc;
    (*NewSocket)->CibirIdOffsetDst = Config->CibirIdOffsetDst;
    (*NewSocket)->UseTcp = Datapath->UseTcp;
    if (Config->CibirIdLength) {
        memcpy((*NewSocket)->CibirId, Config->CibirId, Config->CibirIdLength);
    }

    if (Config->RemoteAddress) {
        CXPLAT_FRE_ASSERT(!QuicAddrIsWildCard(Config->RemoteAddress));  // No wildcard remote addresses allowed.
        (*NewSocket)->Connected = TRUE;
        (*NewSocket)->RemoteAddress = *Config->RemoteAddress;
    }

    if (Config->LocalAddress) {
        (*NewSocket)->LocalAddress = *Config->LocalAddress;
        if (QuicAddrIsWildCard(Config->LocalAddress)) {
            if (!(*NewSocket)->Connected) {
                (*NewSocket)->Wildcard = TRUE;
            }
        } else {
            CXPLAT_FRE_ASSERT((*NewSocket)->Connected); // Assumes only connected sockets fully specify local address
        }
    } else {
        QuicAddrSetFamily(&(*NewSocket)->LocalAddress, QUIC_ADDRESS_FAMILY_INET6);
        if (!(*NewSocket)->Connected) {
            (*NewSocket)->Wildcard = TRUE;
        }
    }

    CXPLAT_FRE_ASSERT((*NewSocket)->Wildcard ^ (*NewSocket)->Connected); // Assumes either a pure wildcard listener or a
                                                                         // connected socket; not both.

    Status = CxPlatTryAddSocket(&Datapath->SocketPool, *NewSocket);
    if (QUIC_FAILED(Status)) {
        goto Error;
    }

    CxPlatDpRawPlumbRulesOnSocket(*NewSocket, TRUE);

Error:

    if (QUIC_FAILED(Status)) {
        if (*NewSocket != NULL) {
            CxPlatRundownUninitialize(&(*NewSocket)->Rundown);
            CXPLAT_FREE(*NewSocket, QUIC_POOL_SOCKET);
            *NewSocket = NULL;
        }
    }

    return Status;
}

_IRQL_requires_max_(PASSIVE_LEVEL)
QUIC_STATUS
CxPlatSocketCreateTcp(
    _In_ CXPLAT_DATAPATH* Datapath,
    _In_opt_ const QUIC_ADDR* LocalAddress,
    _In_ const QUIC_ADDR* RemoteAddress,
    _In_opt_ void* CallbackContext,
    _Out_ CXPLAT_SOCKET** Socket
    )
{
    UNREFERENCED_PARAMETER(Datapath);
    UNREFERENCED_PARAMETER(LocalAddress);
    UNREFERENCED_PARAMETER(RemoteAddress);
    UNREFERENCED_PARAMETER(CallbackContext);
    UNREFERENCED_PARAMETER(Socket);
    return QUIC_STATUS_NOT_SUPPORTED;
}

_IRQL_requires_max_(PASSIVE_LEVEL)
QUIC_STATUS
CxPlatSocketCreateTcpListener(
    _In_ CXPLAT_DATAPATH* Datapath,
    _In_opt_ const QUIC_ADDR* LocalAddress,
    _In_opt_ void* RecvCallbackContext,
    _Out_ CXPLAT_SOCKET** NewSocket
    )
{
    UNREFERENCED_PARAMETER(Datapath);
    UNREFERENCED_PARAMETER(LocalAddress);
    UNREFERENCED_PARAMETER(RecvCallbackContext);
    UNREFERENCED_PARAMETER(NewSocket);
    return QUIC_STATUS_NOT_SUPPORTED;
}

_IRQL_requires_max_(PASSIVE_LEVEL)
void
CxPlatSocketDelete(
    _In_ CXPLAT_SOCKET* Socket
    )
{
    CxPlatDpRawPlumbRulesOnSocket(Socket, FALSE);
    CxPlatRemoveSocket(&Socket->Datapath->SocketPool, Socket);
    CxPlatRundownReleaseAndWait(&Socket->Rundown);
    if (Socket->PausedTcpSend) {
        CxPlatDpRawTxFree(Socket->PausedTcpSend);
    }

    if (Socket->CachedRstSend) {
        CxPlatDpRawTxEnqueue(Socket->CachedRstSend);
    }

    CXPLAT_FREE(Socket, QUIC_POOL_SOCKET);
}

_IRQL_requires_max_(PASSIVE_LEVEL)
QUIC_STATUS
CxPlatSocketUpdateQeo(
    _In_ CXPLAT_SOCKET* Socket,
    _In_reads_(OffloadCount)
        const CXPLAT_QEO_CONNECTION* Offloads,
    _In_ uint32_t OffloadCount
    )
{
    UNREFERENCED_PARAMETER(Socket);
    UNREFERENCED_PARAMETER(Offloads);
    UNREFERENCED_PARAMETER(OffloadCount);
    return QUIC_STATUS_NOT_SUPPORTED;
}

_IRQL_requires_max_(DISPATCH_LEVEL)
uint16_t
CxPlatSocketGetLocalMtu(
    _In_ CXPLAT_SOCKET* Socket
    )
{
    if (Socket->UseTcp) {
        return 1488; // Reserve space for TCP header.
    } else {
        return 1500;
    }
}

_IRQL_requires_max_(DISPATCH_LEVEL)
void
CxPlatSocketGetLocalAddress(
    _In_ CXPLAT_SOCKET* Socket,
    _Out_ QUIC_ADDR* Address
    )
{
    *Address = Socket->LocalAddress;
}

_IRQL_requires_max_(DISPATCH_LEVEL)
void
CxPlatSocketGetRemoteAddress(
    _In_ CXPLAT_SOCKET* Socket,
    _Out_ QUIC_ADDR* Address
    )
{
    *Address = Socket->RemoteAddress;
}

_IRQL_requires_max_(DISPATCH_LEVEL)
void
CxPlatDpRawRxEthernet(
    _In_ const CXPLAT_DATAPATH* Datapath,
    _In_reads_(PacketCount)
        CXPLAT_RECV_DATA** Packets,
    _In_ uint16_t PacketCount
    )
{
    for (uint16_t i = 0; i < PacketCount; i++) {
        CXPLAT_SOCKET* Socket = NULL;
        CXPLAT_RECV_DATA* PacketChain = Packets[i];
        CXPLAT_DBG_ASSERT(PacketChain->Next == NULL);

        if (PacketChain->Reserved >= L4_TYPE_UDP) {
            Socket =
                CxPlatGetSocket(
                    &Datapath->SocketPool,
                    &PacketChain->Route->LocalAddress,
                    &PacketChain->Route->RemoteAddress);
        }

        if (Socket) {
            if (PacketChain->Reserved == L4_TYPE_UDP || PacketChain->Reserved == L4_TYPE_TCP) {
                uint8_t SocketType = Socket->UseTcp ? L4_TYPE_TCP : L4_TYPE_UDP;

                //
                // Found a match. Chain and deliver contiguous packets with the same 4-tuple.
                //
                while (i < PacketCount) {
                    QuicTraceEvent(
                        DatapathRecv,
                        "[data][%p] Recv %u bytes (segment=%hu) Src=%!ADDR! Dst=%!ADDR!",
                        Socket,
                        Packets[i]->BufferLength,
                        Packets[i]->BufferLength,
                        CASTED_CLOG_BYTEARRAY(sizeof(Packets[i]->Route->LocalAddress), &Packets[i]->Route->LocalAddress),
                        CASTED_CLOG_BYTEARRAY(sizeof(Packets[i]->Route->RemoteAddress), &Packets[i]->Route->RemoteAddress));
                    if (i == PacketCount - 1 ||
                        Packets[i+1]->Reserved != SocketType ||
                        Packets[i+1]->Route->LocalAddress.Ipv4.sin_port != Socket->LocalAddress.Ipv4.sin_port ||
                        !CxPlatSocketCompare(Socket, &Packets[i+1]->Route->LocalAddress, &Packets[i+1]->Route->RemoteAddress)) {
                        break;
                    }
                    Packets[i]->Next = Packets[i+1];
                    CXPLAT_DBG_ASSERT(Packets[i+1]->Next == NULL);
                    i++;
                }
                Datapath->UdpHandlers.Receive(Socket, Socket->CallbackContext, (CXPLAT_RECV_DATA*)PacketChain);
            } else if (PacketChain->Reserved == L4_TYPE_TCP_SYN || PacketChain->Reserved == L4_TYPE_TCP_SYNACK) {
                CxPlatDpRawSocketAckSyn(Socket, PacketChain);
                CxPlatDpRawRxFree(PacketChain);
            } else if (PacketChain->Reserved == L4_TYPE_TCP_FIN) {
                CxPlatDpRawSocketAckFin(Socket, PacketChain);
                CxPlatDpRawRxFree(PacketChain);
            } else {
                CxPlatDpRawRxFree(PacketChain);
            }

            CxPlatRundownRelease(&Socket->Rundown);
        } else {
            CxPlatDpRawRxFree(PacketChain);
        }
    }
}

_IRQL_requires_max_(DISPATCH_LEVEL)
void
CxPlatRecvDataReturn(
    _In_opt_ CXPLAT_RECV_DATA* RecvDataChain
    )
{
    CxPlatDpRawRxFree((const CXPLAT_RECV_DATA*)RecvDataChain);
}

_IRQL_requires_max_(DISPATCH_LEVEL)
_Success_(return != NULL)
CXPLAT_SEND_DATA*
CxPlatSendDataAlloc(
    _In_ CXPLAT_SOCKET* Socket,
    _Inout_ CXPLAT_SEND_CONFIG* Config
    )
{
    return CxPlatDpRawTxAlloc(Socket, Config);
}

_IRQL_requires_max_(DISPATCH_LEVEL)
_Success_(return != NULL)
QUIC_BUFFER*
CxPlatSendDataAllocBuffer(
    _In_ CXPLAT_SEND_DATA* SendData,
    _In_ uint16_t MaxBufferLength
    )
{
    SendData->Buffer.Length = MaxBufferLength;
    return &SendData->Buffer;
}

_IRQL_requires_max_(DISPATCH_LEVEL)
void
CxPlatSendDataFree(
    _In_ CXPLAT_SEND_DATA* SendData
    )
{
    CxPlatDpRawTxFree(SendData);
}

_IRQL_requires_max_(DISPATCH_LEVEL)
void
CxPlatSendDataFreeBuffer(
    _In_ CXPLAT_SEND_DATA* SendData,
    _In_ QUIC_BUFFER* Buffer
    )
{
    UNREFERENCED_PARAMETER(SendData);
    UNREFERENCED_PARAMETER(Buffer);
    // No-op
}

_IRQL_requires_max_(DISPATCH_LEVEL)
BOOLEAN
CxPlatSendDataIsFull(
    _In_ CXPLAT_SEND_DATA* SendData
    )
{
    UNREFERENCED_PARAMETER(SendData);
    return TRUE;
}

#define TH_ACK 0x10

_IRQL_requires_max_(DISPATCH_LEVEL)
QUIC_STATUS
CxPlatSocketSend(
    _In_ CXPLAT_SOCKET* Socket,
    _In_ const CXPLAT_ROUTE* Route,
    _In_ CXPLAT_SEND_DATA* SendData
    )
{
    if (Socket->UseTcp &&
        Socket->Connected &&
        Route->TcpState.Syncd == FALSE) {
        Socket->PausedTcpSend = SendData;
        CxPlatDpRawSocketSyn(Socket, Route);
        return QUIC_STATUS_SUCCESS;
    }

    QuicTraceEvent(
        DatapathSend,
        "[data][%p] Send %u bytes in %hhu buffers (segment=%hu) Dst=%!ADDR!, Src=%!ADDR!",
        Socket,
        SendData->Buffer.Length,
        1,
        (uint16_t)SendData->Buffer.Length,
        CASTED_CLOG_BYTEARRAY(sizeof(Route->RemoteAddress), &Route->RemoteAddress),
        CASTED_CLOG_BYTEARRAY(sizeof(Route->LocalAddress), &Route->LocalAddress));
    CXPLAT_DBG_ASSERT(Route->State == RouteResolved);
    CXPLAT_DBG_ASSERT(Route->Queue != NULL);
    const CXPLAT_INTERFACE* Interface = CxPlatDpRawGetInterfaceFromQueue(Route->Queue);

    CxPlatFramingWriteHeaders(
        Socket, Route, &SendData->Buffer, SendData->ECN,
        Interface->OffloadStatus.Transmit.NetworkLayerXsum,
        Interface->OffloadStatus.Transmit.TransportLayerXsum,
        Route->TcpState.SequenceNumber,
        Route->TcpState.AckNumber,
        TH_ACK);
    CxPlatDpRawTxEnqueue(SendData);
    return QUIC_STATUS_SUCCESS;
}

// ->CxPlat?
CXPLAT_THREAD_CALLBACK(CxPlatRouteResolutionWorkerThread, Context)
{
    UNREFERENCED_PARAMETER(Context);
    return 0;
}
