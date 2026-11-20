#include <winapi/netapi32.h>
#include <winapi/support.h>
#include <winapi/ws2_32.h>
#include <lib/winapp.h>
#include <cstddef>
#include <cstring>

namespace win32 { namespace netapi32
{

// The network control block (nb30.h), 64 bytes.
struct NCB
{
    BYTE        ncb_command;
    BYTE        ncb_retcode;
    BYTE        ncb_lsn;
    BYTE        ncb_num;
    x86::reg32  ncb_buffer;
    WORD        ncb_length;
    BYTE        ncb_callname[16];
    BYTE        ncb_name[16];
    BYTE        ncb_rto;
    BYTE        ncb_sto;
    x86::reg32  ncb_post;
    BYTE        ncb_lana_num;
    BYTE        ncb_cmd_cplt;
    BYTE        ncb_reserve[10];
    x86::reg32  ncb_event;
} __attribute__((packed));
static_assert(sizeof(NCB) == 64, "NCB layout");
static_assert(offsetof(NCB, ncb_lana_num) == 48, "NCB layout");

static const BYTE NCBRESET = 0x32;
static const BYTE NCBASTAT = 0x33;
static const BYTE NCBENUM = 0x37;
static const BYTE ASYNCH = 0x80;
static const BYTE NRC_GOODRET = 0x00;
static const BYTE NRC_BUFLEN = 0x01;
static const BYTE NRC_ILLCMD = 0x03;
static const BYTE NRC_CMDTMO = 0x05;
static const BYTE NRC_INCOMP = 0x06;
static const BYTE NRC_BRIDGE = 0x23;    // invalid adapter number

// One adapter, LANA 0, when the network is on: the game reads its MAC (NCBASTAT) as the lobby's machine id.
// Asynchronous commands complete at once.
BYTE Netbios(WinApplication* app, x86::CPU& cpu, NCB* pncb)
{
    if (!pncb)
        return NRC_BRIDGE;
    unsigned char mac[6];
    const bool online = ws2_32::adapterAddress(mac);
    if (!online)
        offline(app, cpu, "Netbios");
    const BYTE command = pncb->ncb_command & ~ASYNCH;
    BYTE result = NRC_BRIDGE;
    if (command == NCBENUM)
    {
        // LANA_ENUM: a count byte, then the adapter numbers.
        if (!pncb->ncb_buffer || pncb->ncb_length < (online ? 2 : 1))
        {
            result = NRC_BUFLEN;
        }
        else
        {
            app->getMemory<BYTE>(pncb->ncb_buffer) = online ? 1 : 0;
            if (online)
                app->getMemory<BYTE>(pncb->ncb_buffer + 1) = 0;
            pncb->ncb_length = online ? 2 : 1;
            result = NRC_GOODRET;
        }
    }
    else if (online && pncb->ncb_lana_num == 0 && command == NCBRESET)
    {
        result = NRC_GOODRET;
    }
    else if (online && pncb->ncb_lana_num == 0 && command == NCBASTAT)
    {
        if (pncb->ncb_callname[0] != '*')
        {
            result = NRC_CMDTMO;    // a remote adapter's status
        }
        else
        {
            // ADAPTER_STATUS (60 bytes): the MAC, rev_major 3, adapter_type 0xFE (Ethernet), then the counters
            // from free_ncbs to name_count.
            BYTE status[60];
            memset(status, 0, sizeof(status));
            memcpy(status, mac, 6);
            status[6] = 3;
            status[8] = 0xFE;
            static const WORD words[] = { 255, 255, 255, 0, 1464, 0, 64, 64, 1700, 0 };
            for (int i = 0; i < 10; ++i)
            {
                status[40 + 2 * i] = BYTE(words[i]);
                status[41 + 2 * i] = BYTE(words[i] >> 8);
            }
            const WORD length = pncb->ncb_length < 60 ? pncb->ncb_length : 60;
            if (length && !pncb->ncb_buffer)
            {
                result = NRC_BUFLEN;
            }
            else
            {
                if (length)
                    memcpy(&app->getMemory<void>(pncb->ncb_buffer), status, length);
                pncb->ncb_length = length;
                result = length < 60 ? NRC_INCOMP : NRC_GOODRET;
            }
        }
    }
    else if (online && pncb->ncb_lana_num == 0)
    {
        result = NRC_ILLCMD;
    }
    pncb->ncb_retcode = result;
    pncb->ncb_cmd_cplt = result;
    return result;
}

}}
